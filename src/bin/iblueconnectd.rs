use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, SyncSender};
use std::thread;
use std::time::{Duration, SystemTime};

use iblueconnect::audio::{AudioRoute, observe_pipewire};
use iblueconnect::backend::{Backend, Command, CommandError, RawCall};
use iblueconnect::call::{AudioLocation, CallAction, CallPhase, Observation};
use iblueconnect::call_log::{self, PhoneCall};
use iblueconnect::contacts_store::{ContactStore, Entry};
use iblueconnect::history::CallRecord;
use iblueconnect::history_tracker::HistoryTracker;
use iblueconnect::notifications::{self, IncomingAction, IncomingNotifications, NotificationEvent};
use iblueconnect::ofono::{OfonoBackend, OfonoCall};
use iblueconnect::pbap::pull_phonebook;
use iblueconnect::presentation::{Effect, PresentationPolicy};
use iblueconnect::vcard;
use zbus::blocking::{Connection, Proxy, connection::Builder};

const PATH: &str = "/org/iblueconnect/Daemon";
const IFACE: &str = "org.iblueconnect.Daemon";
type Snapshot = (String, String, String, String, String);
type ContactList = Vec<(String, String, String)>;
type RecentList = Vec<(String, String, String, i64)>;
const SYNC_FAILED: &str = "Contact sync failed — allow Sync Contacts for this laptop in the iPhone's Bluetooth settings";

enum Operation {
    Snapshot,
    Action(CallAction, String),
    Dial(String),
    Tone(String, char),
    Contacts,
    SyncContacts,
    Recents,
    BatteryStatus,
}

enum Reply {
    Snapshot(Snapshot),
    Done(Result<(), String>),
    Contacts(String, ContactList),
    Recents(String, RecentList),
    BatteryStatus(String),
}

enum Message {
    Request(Operation, SyncSender<Reply>),
    Changed(u64),
    Lost(u64),
    Retry,
    ContactsPulled(Option<Vec<Entry>>, Option<Vec<PhoneCall>>),
    Notification(NotificationEvent),
}

#[derive(Default)]
struct ListenerEpoch(u64);

impl ListenerEpoch {
    fn subscribe(&mut self) -> u64 {
        self.invalidate();
        self.0
    }

    fn invalidate(&mut self) { self.0 = self.0.wrapping_add(1); }
    fn accepts(&self, event: u64) -> bool { self.0 == event }
}

struct Service(Sender<Message>);

impl Service {
    fn request(&self, operation: Operation) -> zbus::fdo::Result<Reply> {
        let (reply_tx, reply_rx) = mpsc::sync_channel(1);
        self.0.send(Message::Request(operation, reply_tx))
            .map_err(|_| zbus::fdo::Error::Failed("Calling service stopped".into()))?;
        reply_rx.recv().map_err(|_| zbus::fdo::Error::Failed("Calling service stopped".into()))
    }

    fn command(&self, operation: Operation) -> zbus::fdo::Result<()> {
        match self.request(operation)? {
            Reply::Done(Ok(())) => Ok(()),
            Reply::Done(Err(message)) => Err(zbus::fdo::Error::Failed(message)),
            _ => unreachable!(),
        }
    }
}

#[zbus::interface(name = "org.iblueconnect.Daemon")]
impl Service {
    fn snapshot(&self) -> zbus::fdo::Result<Snapshot> {
        match self.request(Operation::Snapshot)? {
            Reply::Snapshot(snapshot) => Ok(snapshot),
            _ => unreachable!(),
        }
    }

    fn contacts(&self) -> zbus::fdo::Result<(String, ContactList)> {
        match self.request(Operation::Contacts)? {
            Reply::Contacts(status, list) => Ok((status, list)),
            _ => unreachable!(),
        }
    }
    fn sync_contacts(&self) -> zbus::fdo::Result<()> { self.command(Operation::SyncContacts) }

    /// Recent calls, newest first: (name, number, kind, unix start); "" for unknown name/withheld number.
    fn recents(&self) -> zbus::fdo::Result<(String, RecentList)> {
        match self.request(Operation::Recents)? {
            Reply::Recents(status, list) => Ok((status, list)),
            _ => unreachable!(),
        }
    }

    fn status(&self) -> zbus::fdo::Result<String> { Ok(self.snapshot()?.0) }
    fn battery_status(&self) -> zbus::fdo::Result<String> {
        match self.request(Operation::BatteryStatus)? {
            Reply::BatteryStatus(status) => Ok(status),
            _ => unreachable!(),
        }
    }
    fn answer(&self, id: &str) -> zbus::fdo::Result<()> {
        self.command(Operation::Action(CallAction::Answer, id.into()))
    }
    fn decline(&self, id: &str) -> zbus::fdo::Result<()> {
        self.command(Operation::Action(CallAction::Decline, id.into()))
    }
    fn hang_up(&self, id: &str) -> zbus::fdo::Result<()> {
        self.command(Operation::Action(CallAction::HangUp, id.into()))
    }
    fn dial(&self, number: &str) -> zbus::fdo::Result<()> {
        self.command(Operation::Dial(number.into()))
    }
    fn send_tone(&self, id: &str, tone: &str) -> zbus::fdo::Result<()> {
        let mut chars = tone.chars();
        let key = chars.next().filter(|_| chars.next().is_none())
            .ok_or_else(|| zbus::fdo::Error::InvalidArgs("Enter one keypad tone".into()))?;
        self.command(Operation::Tone(id.into(), key))
    }
}

struct Worker {
    backend: Backend,
    history: Option<HistoryTracker>,
    ofono: Option<OfonoBackend>,
    tx: Sender<Message>,
    session: Connection,
    lock_session: Connection,
    epoch: ListenerEpoch,
    calls: Vec<OfonoCall>,
    presenter: Presenter,
    notifications: IncomingNotifications,
    contacts: Contacts,
}

impl Worker {
    fn new(tx: Sender<Message>, session: Connection, lock_session: Connection) -> Self {
        let history = history_path().and_then(|path| match HistoryTracker::open(path) {
            Ok(history) => Some(history),
            Err(_) => { eprintln!("Call history unavailable"); None }
        });
        Self { backend: Backend::new(), history, ofono: None, tx, notifications: IncomingNotifications::new(session.clone()), session, lock_session, epoch: ListenerEpoch::default(), calls: Vec::new(), presenter: Presenter::new(), contacts: Contacts::open() }
    }

    fn reconcile(&mut self, calls: &[OfonoCall]) {
        let raw: Vec<_> = calls.iter().map(|call| RawCall {
            path: &call.path, state: &call.state, multiparty: call.multiparty,
        }).collect();
        let audio = if raw.len() == 1 { observe_location() } else { AudioLocation::Unknown };
        self.backend.reconcile(true, &raw, audio);
        let before = self.history_head();
        if let Some(history) = &mut self.history {
            if history.reconcile(self.backend.observation(), calls, SystemTime::now()).is_err() {
                eprintln!("Call history could not be saved");
            }
        }
        if self.history_head() != before { self.recents_changed(); }
        self.calls = calls.to_vec();
        self.present();
    }

    fn present(&mut self) {
        let contacts = matches!(self.backend.observation(), Observation::Call(_))
            .then(|| self.contacts.entries()).unwrap_or_default();
        let caller = caller_label(self.backend.observation(), &self.calls, &contacts);
        let lock_session = &self.lock_session;
        let mut effects = Vec::new();
        self.presenter.observe_with(self.backend.observation(), &caller,
            || (screen_locked(lock_session), dnd_active()), &mut |effect| effects.push(effect.clone()));
        for effect in effects {
            match effect {
                Effect::ShowIncoming(id, label) => {
                    if self.notifications.show(&id, &label).is_err() { eprintln!("Incoming call notification unavailable"); }
                }
                Effect::HideIncoming(id) => self.notifications.hide(&id),
                Effect::Alert(..) => run_effect(&effect),
                _ => {}
            }
        }
    }

    fn unavailable(&mut self) {
        self.epoch.invalidate();
        if let Some(history) = &mut self.history { history.backend_lost(SystemTime::now()); }
        self.ofono = None;
        self.calls.clear();
        self.backend.reconcile(false, &[], AudioLocation::Unknown);
        self.present();
    }

    fn refresh(&mut self) -> Result<(), String> {
        if self.ofono.is_none() {
            let ofono = OfonoBackend::connect().map_err(|error| error.to_string())?;
            let mut events = ofono.subscribe().map_err(|error| error.to_string())?;
            let epoch = self.epoch.subscribe();
            self.reconcile(&events.calls());
            let tx = self.tx.clone();
            thread::spawn(move || loop {
                match events.next_update() {
                    Ok(_) => {
                        if tx.send(Message::Changed(epoch)).is_err() { break; }
                    }
                    Err(_) => { let _ = tx.send(Message::Lost(epoch)); break; }
                }
            });
            self.ofono = Some(ofono);
            // The user just connected the phone (or the daemon started with it connected).
            self.sync_contacts().ok();
        }
        let calls = match self.ofono.as_ref().unwrap().calls() {
            Ok(calls) => calls,
            Err(error) => { self.unavailable(); return Err(error.to_string()); }
        };
        self.reconcile(&calls);
        Ok(())
    }

    fn handle(&mut self, operation: Operation) -> Reply {
        if let Operation::Contacts = operation {
            return Reply::Contacts(self.contacts.status(), self.contacts.list());
        }
        if let Operation::Recents = operation {
            return Reply::Recents(self.contacts.status(), self.contacts.recents(&self.local_history()));
        }
        if let Operation::BatteryStatus = operation {
            return Reply::BatteryStatus(match self.ofono.as_ref().and_then(OfonoBackend::battery_percent) {
                Some(percent) => format!("iPhone battery about {percent}%"),
                None => "iPhone battery unavailable".into(),
            });
        }
        let refreshed = self.refresh();
        if let Operation::SyncContacts = operation {
            let result = if refreshed.is_ok() { self.sync_contacts() } else { Err("Phone connection required") };
            return Reply::Done(result.map_err(str::to_owned));
        }
        if let Operation::Snapshot = operation {
            let contacts = matches!(self.backend.observation(), Observation::Call(_))
                .then(|| self.contacts.entries()).unwrap_or_default();
            return Reply::Snapshot(public_snapshot(refreshed.is_ok(), self.backend.observation(), &self.calls, &contacts));
        }
        if let Err(message) = refreshed { return Reply::Done(Err(message)); }
        if let Err(message) = self.guard_operation(&operation) {
            return Reply::Done(Err(message.into()));
        }
        let command = match operation {
            Operation::Action(action, id) => self.backend.plan_action(action, &id),
            Operation::Dial(number) => self.backend.plan_dial(&number),
            Operation::Tone(id, tone) => self.backend.plan_tone(&id, tone),
            Operation::Snapshot | Operation::Contacts | Operation::SyncContacts | Operation::Recents | Operation::BatteryStatus => unreachable!(),
        };
        let command = match command {
            Ok(command) => command,
            Err(error) => return Reply::Done(Err(command_error(error).into())),
        };
        let result = match &command {
            Command::Dial { number, .. } => self.ofono.as_ref().unwrap().dial(number).map(|_| ()),
            Command::Answer { path, .. } => self.ofono.as_ref().unwrap().act(path, CallAction::Answer),
            Command::HangUp { path, action, .. } => self.ofono.as_ref().unwrap().act(path, *action),
            Command::SendTone { path, tone, .. } => self.ofono.as_ref().unwrap().send_tone(path, *tone),
        };
        if result.is_ok() {
            if let (Some(history), Command::HangUp { path, action, .. }) = (&mut self.history, &command) {
                history.action_succeeded(path, *action);
            }
        }
        self.backend.command_result(&command, result.is_ok());
        let reply = result.map_err(|error| error.to_string());
        let _ = self.refresh();
        Reply::Done(reply)
    }

    fn changed(&self) {
        let _ = self.session.emit_signal(None::<&str>, PATH, IFACE, "Changed", &());
    }

    fn contacts_changed(&self) {
        let _ = self.session.emit_signal(None::<&str>, PATH, IFACE, "ContactsChanged", &());
    }

    fn recents_changed(&self) {
        let _ = self.session.emit_signal(None::<&str>, PATH, IFACE, "RecentsChanged", &());
    }

    fn local_history(&self) -> Vec<CallRecord> {
        self.history.as_ref().map(|history| history.recent().cloned().collect()).unwrap_or_default()
    }

    /// Newest local record and whether it finished: changes when a call starts or ends.
    fn history_head(&self) -> Option<(String, bool)> {
        self.history.as_ref()?.recent().next().map(|record| (record.id.clone(), record.ended_at.is_some()))
    }

    fn sync_contacts(&mut self) -> Result<(), &'static str> {
        let address = self.ofono.as_ref().and_then(OfonoBackend::phone_address);
        self.contacts.start(address, &self.tx)?;
        self.contacts_changed();
        Ok(())
    }

    fn guard_operation(&self, operation: &Operation) -> Result<(), &'static str> {
        answer_guard(operation, || screen_locked(&self.lock_session))
    }
}

/// Contact cache plus the state of the one background PBAP download.
struct Contacts {
    store: Option<ContactStore>,
    running: bool,
    failed: bool,
}

impl Contacts {
    fn open() -> Self {
        let store = contacts_path().and_then(|path| ContactStore::open(path).ok());
        if store.is_none() { eprintln!("Contact cache unavailable"); }
        Self { store, running: false, failed: false }
    }

    fn status(&self) -> String {
        let label = self.store.as_ref().and_then(|store| store.last_sync_label().ok().flatten());
        contact_status(self.running, self.failed, label.as_deref())
    }

    fn list(&self) -> ContactList {
        self.entries().into_iter()
            .map(|entry| (entry.name, entry.label, entry.number)).collect()
    }

    fn entries(&self) -> Vec<Entry> {
        self.store.as_ref().and_then(|store| store.list().ok()).unwrap_or_default()
    }

    /// Downloads on its own thread so calls are never blocked by a slow phonebook.
    fn start(&mut self, address: Option<String>, tx: &Sender<Message>) -> Result<(), &'static str> {
        if self.running { return Err("Contact sync already running"); }
        let address = address.ok_or("Phone connection required")?;
        self.running = true;
        let tx = tx.clone();
        thread::spawn(move || {
            // Contacts and call history are pulled separately so either can fail alone.
            let pull = |book| Connection::session().ok()
                .and_then(|bus| pull_phonebook(&bus, &address, book, Duration::from_secs(60)).ok());
            let entries = pull("pb").map(|text| vcard::parse(&text));
            let calls = pull("cch").map(|text| call_log::parse(&text));
            let _ = tx.send(Message::ContactsPulled(entries, calls));
        });
        Ok(())
    }

    /// An empty download is treated as refused access, so it never wipes the last good cache.
    /// Synced call history is stored silently: it never raises popups, sounds, or notifications.
    fn finish(&mut self, entries: Option<Vec<Entry>>, calls: Option<Vec<PhoneCall>>, now: i64) {
        self.running = false;
        self.failed = match (entries, &mut self.store) {
            (Some(entries), Some(store)) if !entries.is_empty() => store.replace_all(&entries, now).is_err(),
            _ => true,
        };
        if self.failed { eprintln!("Contact sync failed"); }
        let history_failed = match (calls, &mut self.store) {
            (Some(calls), Some(store)) if !calls.is_empty() => store.replace_calls(&calls).is_err(),
            _ => true,
        };
        if history_failed { eprintln!("Call history sync failed"); }
    }

    fn recents(&self, local: &[CallRecord]) -> RecentList {
        let (phone, contacts) = self.store.as_ref()
            .map(|store| (store.phone_calls().unwrap_or_default(), store.list().unwrap_or_default()))
            .unwrap_or_default();
        call_log::merge(local, &phone, &contacts).into_iter()
            .map(|recent| (recent.name, recent.number, recent.kind.into(), recent.started_at)).collect()
    }
}

fn contact_status(running: bool, failed: bool, last_sync: Option<&str>) -> String {
    match (running, failed, last_sync) {
        (true, _, _) => "Syncing contacts…".into(),
        (false, true, _) => SYNC_FAILED.into(),
        (false, false, Some(label)) => format!("Last synced {label}"),
        (false, false, None) => "Not synced yet".into(),
    }
}

fn contacts_path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_DATA_HOME")
        .filter(|value| !value.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".local/share")))?;
    Some(base.join("iblueconnect/contacts.sqlite3"))
}

fn screen_locked(session: &Connection) -> Option<bool> {
    Proxy::new(session, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver")
        .ok()?.call("GetActive", &()).ok()
}

struct Presenter {
    policy: PresentationPolicy,
    ringing: Option<String>,
}

impl Presenter {
    fn new() -> Self { Self { policy: PresentationPolicy::new(true, true), ringing: None } }

    #[cfg(test)]
    fn observe(&mut self, observed: &Observation, caller: &str, locked: Option<bool>, dnd: bool, run: &mut impl FnMut(&Effect)) {
        self.observe_with(observed, caller, || (locked, dnd), run);
    }

    /// Reads lock/DND only while a call is or was ringing, so idle costs nothing.
    fn observe_with(&mut self, observed: &Observation, caller: &str, desktop: impl FnOnce() -> (Option<bool>, bool),
                    run: &mut impl FnMut(&Effect)) {
        let incoming = match observed {
            Observation::Call(call) if call.phase == CallPhase::Incoming => Some(call.id.clone()),
            _ => None,
        };
        if incoming.is_none() && self.ringing.is_none() { return; }
        let (locked, dnd) = desktop();
        self.policy.set_dnd(dnd);
        let mut effects = self.policy.set_locked(locked.unwrap_or(true));
        if self.ringing != incoming {
            if let Some(old) = self.ringing.take() { effects.extend(self.policy.end_ringing(&old)); }
            if let Some(id) = &incoming { effects.extend(self.policy.ring(id, caller)); }
            self.ringing = incoming;
        }
        effects.iter().filter(|effect| self.policy.effect_is_current(effect)).for_each(|effect| run(effect));
    }
}

fn dnd_active() -> bool {
    std::process::Command::new("gsettings")
        .args(["get", "org.gnome.desktop.notifications", "show-banners"])
        .output().ok().filter(|output| output.status.success())
        .is_none_or(|output| output.stdout.trim_ascii() != b"true")
}

fn run_effect(effect: &Effect) {
    let (program, args): (std::path::PathBuf, &[&str]) = match effect {
        Effect::Alert(..) => ("canberra-gtk-play".into(), &["-i", "phone-incoming-call"]),
        _ => return,
    };
    match std::process::Command::new(program).args(args).spawn() {
        // Reap in the background so finished helpers do not linger as zombies.
        Ok(mut child) => { thread::spawn(move || child.wait()); }
        Err(_) => eprintln!("Incoming call sound could not start"),
    }
}

fn caller_label(observed: &Observation, calls: &[OfonoCall], contacts: &[Entry]) -> String {
    let Observation::Call(current) = observed else { return String::new() };
    let call = calls.iter().find(|call| call.path == current.id);
    call.and_then(|call| call.name.as_deref().filter(|name| !name.is_empty())
            .or_else(|| call.number.as_deref().and_then(|number| call_log::contact_name(number, contacts)))
            .or(call.number.as_deref().filter(|number| !number.is_empty())))
        .unwrap_or("Unknown caller").to_owned()
}

fn history_path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|value| !value.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".local/state")))?;
    Some(base.join("iblueconnect/history"))
}

fn route_location(route: &AudioRoute) -> AudioLocation {
    if route.laptop_verified { AudioLocation::Laptop } else { AudioLocation::Unknown }
}

fn observe_location() -> AudioLocation {
    observe_pipewire().map(|route| route_location(&route)).unwrap_or(AudioLocation::Unknown)
}

fn answer_guard(operation: &Operation, read_locked: impl FnOnce() -> Option<bool>) -> Result<(), &'static str> {
    if matches!(operation, Operation::Action(CallAction::Answer, _)) {
        match read_locked() {
            Some(false) => Ok(()),
            Some(true) => Err("Unlock the screen before answering"),
            None => Err("Screen lock state unavailable; answer on the phone"),
        }
    } else {
        Ok(())
    }
}

fn command_error(error: CommandError) -> &'static str {
    match error {
        CommandError::NotReady => "Phone connection required",
        CommandError::UnsupportedCalls => "Manage this call on the phone",
        CommandError::Busy => "Another call action is pending",
        CommandError::StaleCall => "The call changed; refresh before acting",
        CommandError::InvalidNumber => "Enter a valid phone number",
        CommandError::InvalidTone => "Enter one keypad tone",
    }
}

fn public_snapshot(ready: bool, observed: &Observation, calls: &[OfonoCall], contacts: &[Entry]) -> Snapshot {
    let status = if !ready { "calling unavailable" }
        else if matches!(observed, Observation::Unavailable) { "manage calls on phone" }
        else { "ready" };
    let (id, phase, audio) = match observed {
        Observation::Call(call) => {
            let phase = match call.phase {
                CallPhase::Incoming => "incoming", CallPhase::OutgoingDialing => "dialing",
                CallPhase::OutgoingAlerting => "alerting", CallPhase::Active => "active",
                CallPhase::Ending => "ending", CallPhase::Terminal(_) => "ended",
            };
            let audio = match call.audio {
                AudioLocation::Phone => "phone", AudioLocation::Laptop => "laptop",
                AudioLocation::Switching => "switching", AudioLocation::Unknown => "unknown",
            };
            (call.id.clone(), phase, audio)
        }
        _ => (String::new(), "idle", "unknown"),
    };
    (status.into(), id, phase.into(), audio.into(), caller_label(observed, calls, contacts))
}

fn next_message(rx: &Receiver<Message>, timeout: Duration) -> Option<Message> {
    match rx.recv_timeout(timeout) {
        Ok(message) => Some(message),
        Err(RecvTimeoutError::Timeout) => Some(Message::Retry),
        Err(RecvTimeoutError::Disconnected) => None,
    }
}

fn retry_delay(attempt: usize) -> Duration {
    Duration::from_secs([1, 2, 5, 10, 30][attempt.min(4)])
}

fn run_worker(rx: Receiver<Message>, tx: Sender<Message>, session: Connection, lock_session: Connection) {
    let mut worker = Worker::new(tx, session, lock_session);
    let mut retry_attempt = 0usize;
    let _ = worker.refresh();
    while let Some(message) = next_message(&rx, if worker.ofono.is_some() { Duration::from_secs(5) } else { retry_delay(retry_attempt) }) {
        match message {
            Message::Request(operation, reply) => { let _ = reply.send(worker.handle(operation)); }
            Message::Changed(epoch) if worker.epoch.accepts(epoch) => {
                let _ = worker.refresh(); worker.changed();
            }
            Message::Lost(epoch) if worker.epoch.accepts(epoch) => {
                worker.unavailable(); retry_attempt = 0; worker.changed();
            }
            Message::Retry if worker.ofono.is_none() => {
                if worker.refresh().is_ok() {
                    retry_attempt = 0;
                    worker.changed();
                } else {
                    retry_attempt = retry_attempt.saturating_add(1);
                }
            }
            Message::Retry => {
                // Also detect a vanished oFono service if it could not emit ModemRemoved.
                let before = worker.backend.observation().clone();
                let lost = worker.refresh().is_err();
                if lost || worker.backend.observation() != &before { worker.changed(); }
            }
            Message::Notification(NotificationEvent::Action(id, key)) => {
                if let Some((call_id, action)) = worker.notifications.action(id, &key) {
                    worker.notifications.ignore(id);
                    match action {
                        IncomingAction::Answer => { let _ = worker.handle(Operation::Action(CallAction::Answer, call_id)); }
                        IncomingAction::Decline => { let _ = worker.handle(Operation::Action(CallAction::Decline, call_id)); }
                        IncomingAction::Ignore => {}
                    }
                }
            }
            Message::Notification(NotificationEvent::Closed(id)) => worker.notifications.ignore(id),
            Message::ContactsPulled(entries, calls) => {
                let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |time| time.as_secs() as i64);
                worker.contacts.finish(entries, calls, now);
                worker.contacts_changed();
                worker.recents_changed();
                worker.changed();
            }
            _ => {}
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let (tx, rx) = mpsc::channel();
    let session = Builder::session()?
        .name("org.iblueconnect.Daemon")?
        .serve_at(PATH, Service(tx.clone()))?
        .build()?;
    let lock_session = Connection::session()?;
    let notification_session = Connection::session()?;
    let notify_tx = tx.clone();
    if notifications::listen(notification_session, move |event| { let _ = notify_tx.send(Message::Notification(event)); }).is_err() {
        eprintln!("Desktop notification actions unavailable");
    }
    thread::spawn(move || run_worker(rx, tx, session, lock_session)).join()
        .expect("calling worker stopped");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use iblueconnect::call::{AudioLocation, Call, CallPhase};
    use iblueconnect::call_log::{Kind, PhoneCall};
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Command as ProcessCommand, Stdio};

    struct PrivateBus(Child);

    impl PrivateBus {
        fn start() -> (Self, String) {
            let mut child = ProcessCommand::new("dbus-daemon")
                .args(["--session", "--nofork", "--print-address=1", "--address=unix:tmpdir=/tmp"])
                .stdout(Stdio::piped()).stderr(Stdio::null()).spawn().unwrap();
            let mut address = String::new();
            BufReader::new(child.stdout.take().unwrap()).read_line(&mut address).unwrap();
            (Self(child), address.trim().to_owned())
        }
    }

    impl Drop for PrivateBus {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }

    struct UnlockedScreen;

    #[zbus::interface(name = "org.gnome.ScreenSaver")]
    impl UnlockedScreen {
        fn get_active(&self) -> bool { false }
    }

    #[test]
    fn answer_completes_while_daemon_checks_screen_on_session_bus() {
        let (_bus, address) = PrivateBus::start();
        let _screen = Builder::address(address.as_str()).unwrap()
            .name("org.gnome.ScreenSaver").unwrap()
            .serve_at("/org/gnome/ScreenSaver", UnlockedScreen).unwrap()
            .build().unwrap();
        let (tx, rx) = mpsc::channel();
        let session = Builder::address(address.as_str()).unwrap()
            .name("org.iblueconnect.Daemon").unwrap()
            .serve_at(PATH, Service(tx.clone())).unwrap().build().unwrap();
        let lock_session = Builder::address(address.as_str()).unwrap().build().unwrap();
        thread::spawn(move || {
            let worker = Worker {
                backend: Backend::new(), history: None, ofono: None, tx,
                notifications: IncomingNotifications::new(session.clone()), session,
                lock_session, epoch: ListenerEpoch::default(), calls: Vec::new(), presenter: Presenter::new(),
                contacts: Contacts { store: None, running: false, failed: false },
            };
            if let Some(Message::Request(Operation::Action(CallAction::Answer, _), reply)) = rx.recv().ok() {
                let allowed = worker.guard_operation(&Operation::Action(CallAction::Answer, "test".into()));
                let _ = reply.send(Reply::Done(allowed.map_err(str::to_owned)));
            }
        });
        let (result_tx, result_rx) = mpsc::channel();
        thread::spawn(move || {
            let client = Builder::address(address.as_str()).unwrap().build().unwrap();
            let result = Proxy::new(&client, "org.iblueconnect.Daemon", PATH, IFACE).unwrap()
                .call::<_, _, ()>("Answer", &("test",));
            let _ = result_tx.send(result.is_ok());
        });
        assert_eq!(result_rx.recv_timeout(Duration::from_secs(2)), Ok(true));
    }

    #[test]
    fn listener_events_expire_on_loss_and_reconnect() {
        let mut epoch = ListenerEpoch::default();
        let first = epoch.subscribe();
        assert!(epoch.accepts(first));
        epoch.invalidate();
        assert!(!epoch.accepts(first));
        let second = epoch.subscribe();
        assert!(epoch.accepts(second));
        assert!(!epoch.accepts(first));
    }

    #[test]
    fn idle_worker_receives_retry_tick() {
        let (_tx, rx) = mpsc::channel();
        assert!(matches!(next_message(&rx, std::time::Duration::from_millis(1)), Some(Message::Retry)));
    }

    #[test]
    fn unavailable_backend_retry_backs_off_and_caps_at_thirty_seconds() {
        let seconds: Vec<_> = (0..7).map(|attempt| retry_delay(attempt).as_secs()).collect();
        assert_eq!(seconds, [1, 2, 5, 10, 30, 30, 30]);
    }

    #[test]
    fn answer_requires_confirmed_unlocked_screen() {
        let answer = Operation::Action(CallAction::Answer, "call-1".into());
        assert!(answer_guard(&answer, || Some(false)).is_ok());
        assert!(answer_guard(&answer, || Some(true)).is_err());
        assert!(answer_guard(&answer, || None).is_err());
        let decline = Operation::Action(CallAction::Decline, "call-1".into());
        assert!(answer_guard(&decline, || panic!("lock query should be skipped")).is_ok());
    }

    fn ofono_call(number: Option<&str>, name: Option<&str>) -> OfonoCall {
        OfonoCall { path: "/modem0/voicecall01".into(), state: "incoming".into(), multiparty: false,
            number: number.map(str::to_owned), name: name.map(str::to_owned) }
    }

    #[test]
    fn snapshot_reports_call_phase_route_and_caller_label() {
        let observed = Observation::Call(Call::new("/modem0/voicecall01", CallPhase::Incoming, AudioLocation::Phone));
        assert_eq!(public_snapshot(true, &observed, &[ofono_call(Some("123"), None)], &[]),
            ("ready".into(), "/modem0/voicecall01".into(), "incoming".into(), "phone".into(), "123".into()));
    }

    #[test]
    fn snapshot_uses_synced_contact_name_for_incoming_and_outgoing_calls() {
        let mut contacts = test_contacts();
        contacts.finish(Some(vec![entry("Alice", "+20 100 123 4567")]), None, 100);
        let call = [ofono_call(Some("01001234567"), None)];
        for phase in [CallPhase::Incoming, CallPhase::OutgoingDialing] {
            let observed = Observation::Call(Call::new("/modem0/voicecall01", phase, AudioLocation::Phone));
            assert_eq!(public_snapshot(true, &observed, &call, &contacts.entries()).4, "Alice");
        }
        contacts.finish(Some(vec![entry("Alice", "+20 100 123 4567"), entry("Other", "01001234567")]), None, 200);
        let observed = Observation::Call(Call::new("/modem0/voicecall01", CallPhase::Active, AudioLocation::Phone));
        assert_eq!(public_snapshot(true, &observed, &call, &contacts.entries()).4, "01001234567");
    }

    #[test]
    fn caller_label_prefers_name_then_number_then_unknown() {
        let observed = Observation::Call(Call::new("/modem0/voicecall01", CallPhase::Incoming, AudioLocation::Phone));
        assert_eq!(caller_label(&observed, &[ofono_call(Some("123"), Some("Alice"))], &[]), "Alice");
        assert_eq!(caller_label(&observed, &[ofono_call(Some("123"), Some(""))], &[]), "123");
        assert_eq!(caller_label(&observed, &[ofono_call(Some(""), None)], &[]), "Unknown caller");
        assert_eq!(caller_label(&observed, &[], &[]), "Unknown caller");
        assert_eq!(caller_label(&Observation::Idle, &[ofono_call(Some("123"), None)], &[]), "");
    }

    fn presented(presenter: &mut Presenter, phase: CallPhase, locked: Option<bool>, dnd: bool) -> Vec<String> {
        let observed = Observation::Call(Call::new("call-1", phase, AudioLocation::Phone));
        let mut run = Vec::new();
        presenter.observe(&observed, "Alice", locked, dnd, &mut |effect: &Effect| run.push(format!("{effect:?}")));
        run
    }

    #[test]
    fn unlocked_incoming_shows_popup_and_alerts_once() {
        let mut presenter = Presenter::new();
        assert_eq!(presented(&mut presenter, CallPhase::Incoming, Some(false), false),
            [r#"ShowIncoming("call-1", "Alice")"#, r#"Alert("call-1", 0)"#]);
        assert!(presented(&mut presenter, CallPhase::Incoming, Some(false), false).is_empty());
    }

    #[test]
    fn dnd_incoming_shows_popup_without_alert() {
        assert_eq!(presented(&mut Presenter::new(), CallPhase::Incoming, Some(false), true),
            [r#"ShowIncoming("call-1", "Alice")"#]);
    }

    #[test]
    fn locked_or_unknown_lock_incoming_presents_nothing() {
        assert!(presented(&mut Presenter::new(), CallPhase::Incoming, Some(true), false).is_empty());
        assert!(presented(&mut Presenter::new(), CallPhase::Incoming, None, false).is_empty());
    }

    #[test]
    fn ringing_hidden_while_locked_appears_on_unlock() {
        let mut presenter = Presenter::new();
        assert!(presented(&mut presenter, CallPhase::Incoming, Some(true), false).is_empty());
        assert_eq!(presented(&mut presenter, CallPhase::Incoming, Some(false), false),
            [r#"ShowIncoming("call-1", "Alice")"#, r#"Alert("call-1", 0)"#]);
    }

    #[test]
    fn ringing_that_ends_never_reappears() {
        let mut presenter = Presenter::new();
        presented(&mut presenter, CallPhase::Incoming, Some(false), false);
        presented(&mut presenter, CallPhase::Active, Some(false), false);
        assert!(presented(&mut presenter, CallPhase::Incoming, Some(false), false).is_empty());
    }

    #[test]
    fn unavailable_backend_and_unsupported_calls_are_distinct() {
        assert_eq!(public_snapshot(false, &Observation::Unavailable, &[], &[]).0, "calling unavailable");
        assert_eq!(public_snapshot(true, &Observation::Unavailable, &[], &[]).0, "manage calls on phone");
    }

    #[test]
    fn contact_status_prefers_running_then_failure_then_last_sync() {
        assert_eq!(contact_status(false, false, None), "Not synced yet");
        assert_eq!(contact_status(false, false, Some("2026-09-24 20:30")), "Last synced 2026-09-24 20:30");
        assert_eq!(contact_status(true, true, Some("x")), "Syncing contacts…");
        assert_eq!(contact_status(false, true, Some("x")), SYNC_FAILED);
    }

    fn test_contacts() -> Contacts {
        let stamp = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("iblueconnectd-contacts-{}-{stamp}", std::process::id())).join("c.sqlite3");
        Contacts { store: ContactStore::open(path).ok(), running: false, failed: false }
    }

    fn entry(name: &str, number: &str) -> Entry {
        Entry { name: name.into(), label: "Mobile".into(), number: number.into() }
    }

    #[test]
    fn contact_sync_needs_a_connected_phone_and_runs_once_at_a_time() {
        let (tx, _rx) = mpsc::channel();
        let mut contacts = test_contacts();
        assert_eq!(contacts.start(None, &tx), Err("Phone connection required"));
        contacts.running = true;
        assert_eq!(contacts.start(Some("AA:BB:CC:DD:EE:FF".into()), &tx), Err("Contact sync already running"));
    }

    #[test]
    fn finished_sync_replaces_cache_but_failed_or_empty_download_keeps_it() {
        let mut contacts = test_contacts();
        contacts.running = true;
        contacts.finish(Some(vec![entry("Alice", "1")]), None, 100);
        assert!(!contacts.running);
        assert_eq!(contacts.list(), vec![("Alice".into(), "Mobile".into(), "1".into())]);
        assert!(contacts.status().starts_with("Last synced "));

        contacts.finish(None, None, 200);
        assert_eq!(contacts.status(), SYNC_FAILED);
        contacts.finish(Some(vec![]), None, 300);
        assert_eq!(contacts.status(), SYNC_FAILED);
        assert_eq!(contacts.list().len(), 1);
    }

    fn missed(number: &str, at: &str) -> PhoneCall {
        PhoneCall { number: Some(number.into()), name: None, kind: Kind::Missed, started_at: at.into(), utc: true }
    }

    #[test]
    fn call_history_is_stored_independently_of_contacts_and_kept_on_failure() {
        let mut contacts = test_contacts();
        contacts.finish(None, Some(vec![missed("1", "2026-01-01 00:00:00")]), 100);
        assert_eq!(contacts.status(), SYNC_FAILED);
        assert_eq!(contacts.recents(&[]), vec![(String::new(), "1".into(), "missed".into(), 1_767_225_600)]);

        contacts.finish(Some(vec![entry("Alice", "1")]), None, 200);
        contacts.finish(Some(vec![entry("Alice", "1")]), Some(vec![]), 300);
        assert!(contacts.status().starts_with("Last synced "));
        assert_eq!(contacts.recents(&[]), vec![("Alice".into(), "1".into(), "missed".into(), 1_767_225_600)]);
    }

    #[test]
    fn unverified_pipewire_graph_never_claims_phone_or_laptop_audio() {
        use iblueconnect::audio::AudioRoute;
        assert_eq!(route_location(&AudioRoute::default()), AudioLocation::Unknown);
        assert_eq!(route_location(&AudioRoute { playback: Some("output".into()), capture: None, laptop_verified: false }), AudioLocation::Unknown);
        assert_eq!(route_location(&AudioRoute { playback: Some("output".into()), capture: Some("input".into()), laptop_verified: true }), AudioLocation::Laptop);
    }
}
