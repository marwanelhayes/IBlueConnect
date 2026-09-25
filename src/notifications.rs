use std::collections::HashMap;
use std::thread;
use zbus::blocking::{Connection, MessageIterator, Proxy};
use zbus::{message, MatchRule};
use zbus::zvariant::Value;

const DEST: &str = "org.freedesktop.Notifications";
const PATH: &str = "/org/freedesktop/Notifications";

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum IncomingAction { Answer, Decline, Ignore }

#[derive(Debug, Eq, PartialEq)]
pub enum NotificationEvent { Action(u32, String), Closed(u32) }

pub fn listen(connection: Connection, on_event: impl Fn(NotificationEvent) + Send + 'static) -> zbus::Result<()> {
    let rule = MatchRule::builder().msg_type(message::Type::Signal)
        .sender(DEST)?.path(PATH)?.interface(DEST)?.build();
    let mut messages = MessageIterator::for_match_rule(rule, &connection, Some(64))?;
    thread::spawn(move || {
        for message in &mut messages {
            let Ok(message) = message else { break };
            let event = match message.header().member().map(|member| member.as_str()) {
                Some("ActionInvoked") => message.body().deserialize::<(u32, String)>()
                    .ok().map(|(id, key)| NotificationEvent::Action(id, key)),
                Some("NotificationClosed") => message.body().deserialize::<(u32, u32)>()
                    .ok().map(|(id, _)| NotificationEvent::Closed(id)),
                _ => None,
            };
            if let Some(event) = event {
                on_event(event);
            }
        }
    });
    Ok(())
}

pub fn action_for(key: &str) -> Option<IncomingAction> {
    match key {
        "answer" => Some(IncomingAction::Answer),
        "decline" => Some(IncomingAction::Decline),
        "ignore" => Some(IncomingAction::Ignore),
        _ => None,
    }
}

struct Notice<'a> {
    summary: &'static str,
    body: &'a str,
    actions: [&'static str; 6],
    timeout: i32,
}

fn incoming_notice(label: &str) -> Notice<'_> {
    Notice {
        summary: "Incoming call",
        body: label,
        actions: ["answer", "Answer", "decline", "Decline", "ignore", "Ignore"],
        timeout: 0,
    }
}

/// One live incoming-call notification, owned by the daemon rather than a GTK window.
pub struct IncomingNotifications {
    connection: Connection,
    current: Option<(String, u32)>,
    ignored: Option<String>,
}

impl IncomingNotifications {
    pub fn new(connection: Connection) -> Self {
        Self { connection, current: None, ignored: None }
    }

    pub fn show(&mut self, call_id: &str, label: &str) -> zbus::Result<()> {
        if self.ignored.as_deref() == Some(call_id) || self.current.as_ref().is_some_and(|(id, _)| id == call_id) {
            return Ok(());
        }
        self.close_current();
        let notice = incoming_notice(label);
        let proxy = Proxy::new(&self.connection, DEST, PATH, DEST)?;
        let hints: HashMap<&str, Value<'_>> = [
            ("desktop-entry", Value::from("org.iblueconnect.IBlueConnect")),
            ("resident", Value::from(true)),
            ("suppress-sound", Value::from(true)),
        ].into();
        let notification_id: u32 = proxy.call("Notify", &(
            "IBlueConnect", 0u32, "call-start-symbolic", notice.summary, notice.body,
            notice.actions.as_slice(), hints, notice.timeout,
        ))?;
        self.current = Some((call_id.into(), notification_id));
        Ok(())
    }

    pub fn hide(&mut self, call_id: &str) {
        if self.current.as_ref().is_some_and(|(id, _)| id == call_id) { self.close_current(); }
        if self.ignored.as_deref() == Some(call_id) { self.ignored = None; }
    }

    pub fn action(&self, notification_id: u32, key: &str) -> Option<(String, IncomingAction)> {
        let (call_id, id) = self.current.as_ref()?;
        if notification_id != *id { return None; }
        Some((call_id.clone(), action_for(key)?))
    }

    pub fn ignore(&mut self, notification_id: u32) {
        if let Some((call_id, id)) = &self.current {
            if notification_id == *id {
                self.ignored = Some(call_id.clone());
                self.close_current();
            }
        }
    }

    fn close_current(&mut self) {
        if let Some((_, id)) = self.current.take() {
            if let Ok(proxy) = Proxy::new(&self.connection, DEST, PATH, DEST) {
                let _ = proxy.call::<_, _, ()>("CloseNotification", &(id,));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Command, Stdio};
    use std::sync::{Arc, Mutex};
    use zbus::blocking::connection::Builder;
    use zbus::zvariant::OwnedValue;

    struct PrivateBus(Child);
    impl PrivateBus {
        fn start() -> (Self, String) {
            let mut child = Command::new("dbus-daemon")
                .args(["--session", "--nofork", "--print-address=1", "--address=unix:tmpdir=/tmp"])
                .stdout(Stdio::piped()).stderr(Stdio::null()).spawn().unwrap();
            let mut address = String::new();
            BufReader::new(child.stdout.take().unwrap()).read_line(&mut address).unwrap();
            (Self(child), address.trim().into())
        }
    }
    impl Drop for PrivateBus {
        fn drop(&mut self) { let _ = self.0.kill(); let _ = self.0.wait(); }
    }

    struct FakeNotifications(Arc<Mutex<Vec<String>>>);
    #[zbus::interface(name = "org.freedesktop.Notifications")]
    impl FakeNotifications {
        fn notify(&self, _app_name: &str, _replaces_id: u32, _app_icon: &str,
                  summary: &str, _body: &str, actions: Vec<String>,
                  _hints: HashMap<String, OwnedValue>, _expire_timeout: i32) -> u32 {
            assert_eq!(summary, "Incoming call");
            assert_eq!(actions, ["answer", "Answer", "decline", "Decline", "ignore", "Ignore"]);
            self.0.lock().unwrap().push("notify".into());
            42
        }
        fn close_notification(&self, id: u32) {
            assert_eq!(id, 42);
            self.0.lock().unwrap().push("close".into());
        }
    }

    #[test]
    fn incoming_notification_offers_call_actions_and_can_be_removed() {
        let notice = incoming_notice("Sample Caller");
        assert_eq!(notice.summary, "Incoming call");
        assert_eq!(notice.body, "Sample Caller");
        assert_eq!(notice.actions, ["answer", "Answer", "decline", "Decline", "ignore", "Ignore"]);
        assert_eq!(notice.timeout, 0);
        assert_eq!(action_for("answer"), Some(IncomingAction::Answer));
        assert_eq!(action_for("decline"), Some(IncomingAction::Decline));
        assert_eq!(action_for("ignore"), Some(IncomingAction::Ignore));
        assert_eq!(action_for("default"), None);
    }

    #[test]
    fn incoming_notification_reaches_desktop_and_ignore_only_removes_it() {
        let (_bus, address) = PrivateBus::start();
        let events = Arc::new(Mutex::new(Vec::new()));
        let _server = Builder::address(address.as_str()).unwrap()
            .name(DEST).unwrap().serve_at(PATH, FakeNotifications(events.clone())).unwrap().build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let mut incoming = IncomingNotifications::new(client);
        incoming.show("call-1", "Sample Caller").unwrap();
        assert_eq!(incoming.action(42, "answer"), Some(("call-1".into(), IncomingAction::Answer)));
        incoming.ignore(42);
        assert_eq!(incoming.action(42, "answer"), None);
        incoming.show("call-1", "Sample Caller").unwrap();
        assert_eq!(*events.lock().unwrap(), ["notify", "close"]);
        incoming.hide("call-1");
    }

    #[test]
    fn desktop_action_signal_is_delivered_to_daemon_listener() {
        let (_bus, address) = PrivateBus::start();
        let server = Builder::address(address.as_str()).unwrap()
            .name(DEST).unwrap().serve_at(PATH, FakeNotifications(Arc::new(Mutex::new(Vec::new())))).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        listen(client, move |event| { let _ = tx.send(event); }).unwrap();
        server.emit_signal(None::<&str>, PATH, DEST, "ActionInvoked", &(42u32, "answer")).unwrap();
        assert_eq!(rx.recv_timeout(std::time::Duration::from_secs(2)),
                   Ok(NotificationEvent::Action(42, "answer".into())));
    }
}
