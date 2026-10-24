use std::collections::HashMap;
use std::fmt;

use zbus::blocking::{Connection, MessageIterator, Proxy};
use zbus::{MatchRule, message};
use zbus::zvariant::{OwnedObjectPath, OwnedValue};

use crate::call::{can_apply, AudioLocation, Call, CallAction, CallPhase, Observation};

type Properties = HashMap<String, OwnedValue>;
type Objects = Vec<(OwnedObjectPath, Properties)>;

#[derive(Clone, Eq, PartialEq)]
pub struct OfonoCall {
    pub path: String,
    pub state: String,
    pub multiparty: bool,
    pub number: Option<String>,
    pub name: Option<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OfonoError {
    BackendUnavailable,
    NoReadyModem,
    MultipleModems,
    MultipleCalls,
    UnsupportedCallState,
    StaleCall,
    InvalidNumber,
    InvalidTone,
}

impl fmt::Display for OfonoError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::BackendUnavailable => "Calling backend unavailable",
            Self::NoReadyModem => "No phone is ready for calls",
            Self::MultipleModems => "More than one phone is ready for calls",
            Self::MultipleCalls => "Manage additional calls on the phone",
            Self::UnsupportedCallState => "Manage this call on the phone",
            Self::StaleCall => "The call changed; refresh before acting",
            Self::InvalidNumber => "Enter a valid phone number",
            Self::InvalidTone => "Enter one keypad tone",
        };
        f.write_str(message)
    }
}

impl std::error::Error for OfonoError {}

impl From<zbus::Error> for OfonoError {
    fn from(_: zbus::Error) -> Self {
        Self::BackendUnavailable
    }
}

/// Call control for the one ready oFono HFP modem. Audio routing is separate.
pub struct OfonoBackend {
    connection: Connection,
    modem: String,
}

pub struct OfonoEvents {
    messages: MessageIterator,
    backend: OfonoBackend,
    calls: Vec<OfonoCall>,
}

impl OfonoEvents {
    pub fn calls(&self) -> Vec<OfonoCall> {
        self.calls.clone()
    }

    pub fn next_update(&mut self) -> Result<Vec<OfonoCall>, OfonoError> {
        loop {
            let message = self.messages.next().ok_or(OfonoError::BackendUnavailable)??;
            if is_modem_removed(&message, &self.backend.modem) {
                return Err(OfonoError::BackendUnavailable);
            }
            if !is_backend_event(&message, &self.backend.modem) {
                continue;
            }
            // Resync after every event; queued signals may predate the initial GetCalls.
            self.calls = self.backend.calls()?;
            return Ok(self.calls());
        }
    }
}

/// Wakes when oFono adds a modem or a modem's state changes, e.g. the user reconnects the phone.
pub struct ModemWatch(MessageIterator);

impl ModemWatch {
    pub fn system() -> Result<Self, OfonoError> {
        Self::on(Connection::system()?)
    }

    pub fn on(connection: Connection) -> Result<Self, OfonoError> {
        let rule = MatchRule::builder()
            .msg_type(message::Type::Signal)
            .sender("org.ofono")?
            .build();
        Ok(Self(MessageIterator::for_match_rule(rule, &connection, Some(64))?))
    }

    pub fn next_change(&mut self) -> Result<(), OfonoError> {
        loop {
            let message = self.0.next().ok_or(OfonoError::BackendUnavailable)??;
            let header = message.header();
            let (Some(interface), Some(member)) = (header.interface(), header.member()) else { continue };
            if matches!((interface.as_str(), member.as_str()),
                        ("org.ofono.Manager", "ModemAdded") | ("org.ofono.Modem", "PropertyChanged")) {
                return Ok(());
            }
        }
    }
}

impl OfonoBackend {
    pub fn connect() -> Result<Self, OfonoError> {
        Self::connect_on(Connection::system()?)
    }

    fn connect_on(connection: Connection) -> Result<Self, OfonoError> {
        let manager = Proxy::new(&connection, "org.ofono", "/", "org.ofono.Manager")?;
        let modems: Objects = manager.call("GetModems", &())?;
        let mut ready = modems.into_iter().filter(|(_, properties)| {
            property_bool(properties, "Powered") == Some(true)
                && property_bool(properties, "Online") == Some(true)
                && properties
                    .get("Interfaces")
                    .and_then(|value| value.try_clone().ok())
                    .and_then(|value| Vec::<String>::try_from(value).ok())
                    .is_some_and(|interfaces| interfaces.iter().any(|i| i == "org.ofono.VoiceCallManager"))
        });
        let modem = ready.next().ok_or(OfonoError::NoReadyModem)?.0.to_string();
        if ready.next().is_some() {
            return Err(OfonoError::MultipleModems);
        }
        Ok(Self { connection, modem })
    }

    /// Bluetooth address of the connected phone, for contact sync over the same link.
    pub fn phone_address(&self) -> Option<String> {
        modem_address(&self.modem)
    }

    /// The existing BlueZ device only; never creates a Bluetooth connection.
    pub fn bluetooth_path(&self) -> Option<&str> {
        self.phone_address()?;
        self.modem.strip_prefix("/hfp").filter(|path| path.starts_with("/org/bluez/"))
    }

    /// HFP reports the phone's battery in six coarse steps (0–5), not an exact percentage.
    pub fn battery_level(&self) -> Option<u8> {
        let handsfree = Proxy::new(&self.connection, "org.ofono", self.modem.as_str(), "org.ofono.Handsfree").ok()?;
        let properties: Properties = handsfree.call("GetProperties", &()).ok()?;
        properties.get("BatteryChargeLevel")
            .and_then(|value| u8::try_from(value).ok())
            .filter(|level| *level <= 5)
    }

    /// HFP voice recognition (AT+BVRA): Siri on the phone, heard and answered through the laptop.
    /// Starting needs no calls in progress; stopping is always allowed.
    pub fn set_voice_assistant(&self, active: bool) -> Result<(), OfonoError> {
        if active { self.require(|calls| calls.is_empty())?; }
        let handsfree = Proxy::new(&self.connection, "org.ofono", self.modem.as_str(), "org.ofono.Handsfree")?;
        let set = |value: bool| handsfree.call::<_, _, ()>("SetProperty", &("VoiceRecognition", zbus::zvariant::Value::from(value)));
        // The iPhone never reports that a Siri session ended, so oFono keeps VoiceRecognition true and
        // silently ignores the next "true". Reset first (best effort) so every start reaches the phone.
        if active { let _ = set(false); }
        set(active).map_err(Into::into)
    }

    /// Reconciles authoritative oFono call state; route remains unknown here.
    pub fn snapshot(&self) -> Result<Observation, OfonoError> {
        let calls = self.calls()?;
        let mut calls = calls.into_iter();
        let Some(call) = calls.next() else {
            return Ok(Observation::Idle);
        };
        if calls.next().is_some() {
            return Err(OfonoError::MultipleCalls);
        }
        if call.multiparty {
            return Err(OfonoError::UnsupportedCallState);
        }
        let state = phase_for_state(&call.state).ok_or(OfonoError::UnsupportedCallState)?;
        Ok(Observation::Call(Call::new(
            call.path,
            state,
            AudioLocation::Unknown,
        )))
    }

    pub fn calls(&self) -> Result<Vec<OfonoCall>, OfonoError> {
        let manager = self.call_manager()?;
        let calls: Objects = manager.call("GetCalls", &())?;
        calls
            .into_iter()
            .map(|(path, properties)| decode_call(path, properties))
            .collect()
    }

    /// Subscribe before the initial GetCalls so call signals cannot be missed at startup.
    pub fn subscribe(&self) -> Result<OfonoEvents, OfonoError> {
        let rule = MatchRule::builder()
            .msg_type(message::Type::Signal)
            .sender("org.ofono")?
            .build();
        let messages = MessageIterator::for_match_rule(rule, &self.connection, Some(256))?;
        let calls = self.calls()?;
        Ok(OfonoEvents {
            messages,
            backend: Self { connection: self.connection.clone(), modem: self.modem.clone() },
            calls,
        })
    }

    pub fn act(&self, call_id: &str, action: CallAction) -> Result<(), OfonoError> {
        if !can_apply(action, call_id, &self.snapshot()?) {
            return Err(OfonoError::StaleCall);
        }
        let call = Proxy::new(&self.connection, "org.ofono", call_id, "org.ofono.VoiceCall")?;
        let method = match action {
            CallAction::Answer => "Answer",
            CallAction::Decline | CallAction::HangUp => "Hangup",
        };
        call.call(method, &()).map_err(Into::into)
    }

    pub fn dial(&self, number: &str) -> Result<String, OfonoError> {
        if !valid_number(number) {
            return Err(OfonoError::InvalidNumber);
        }
        if self.snapshot()? != Observation::Idle {
            return Err(OfonoError::StaleCall);
        }
        let manager = self.call_manager()?;
        let path: OwnedObjectPath = manager.call("Dial", &(number, "default"))?;
        Ok(path.to_string())
    }

    pub fn hold(&self, call_id: &str) -> Result<(), OfonoError> {
        self.swap_target(call_id, "active")
    }

    pub fn resume(&self, call_id: &str) -> Result<(), OfonoError> {
        self.swap_target(call_id, "held")
    }

    pub fn add_call(&self, number: &str) -> Result<String, OfonoError> {
        if !valid_number(number) {
            return Err(OfonoError::InvalidNumber);
        }
        self.require(can_add_call)?;
        let path: OwnedObjectPath = self.call_manager()?.call("Dial", &(number, "default"))?;
        Ok(path.to_string())
    }

    pub fn swap(&self) -> Result<(), OfonoError> {
        self.require(can_swap)?;
        self.call_manager()?.call("SwapCalls", &()).map_err(Into::into)
    }

    pub fn merge(&self) -> Result<(), OfonoError> {
        self.require(can_merge)?;
        let _: Vec<OwnedObjectPath> = self.call_manager()?.call("CreateMultiparty", &())?;
        Ok(())
    }

    pub fn end_conference(&self) -> Result<(), OfonoError> {
        self.require(can_end_conference)?;
        self.call_manager()?.call("HangupMultiparty", &()).map_err(Into::into)
    }

    pub fn end_call(&self, call_id: &str) -> Result<(), OfonoError> {
        self.require(|calls| can_end_call(calls, call_id))?;
        let call = Proxy::new(&self.connection, "org.ofono", call_id, "org.ofono.VoiceCall")?;
        call.call("Hangup", &()).map_err(Into::into)
    }

    pub fn private_chat(&self, call_id: &str) -> Result<(), OfonoError> {
        self.require(|calls| can_private_chat(calls, call_id))?;
        let path = OwnedObjectPath::try_from(call_id).map_err(|_| OfonoError::StaleCall)?;
        let _: Vec<OwnedObjectPath> = self.call_manager()?.call("PrivateChat", &(path,))?;
        Ok(())
    }

    pub fn hold_and_answer(&self, call_id: &str) -> Result<(), OfonoError> {
        self.require(|calls| can_hold_and_answer(calls, call_id))?;
        self.call_manager()?.call("HoldAndAnswer", &()).map_err(Into::into)
    }

    pub fn release_and_answer(&self, call_id: &str) -> Result<(), OfonoError> {
        self.require(|calls| can_release_and_answer(calls, call_id))?;
        self.call_manager()?.call("ReleaseAndAnswer", &()).map_err(Into::into)
    }

    /// Re-reads oFono so every multi-call action is judged against the phone's current calls.
    fn require(&self, eligible: impl FnOnce(&[OfonoCall]) -> bool) -> Result<(), OfonoError> {
        if eligible(&self.calls()?) { Ok(()) } else { Err(OfonoError::StaleCall) }
    }

    fn swap_target(&self, call_id: &str, state: &str) -> Result<(), OfonoError> {
        let calls = self.calls()?;
        if calls.is_empty() || calls.len() > 2 || calls.iter().any(|call| call.multiparty)
            || !calls.iter().any(|call| call.path == call_id && call.state == state)
            || (calls.len() == 2 && (calls.iter().filter(|call| call.state == "active").count() != 1
                || calls.iter().filter(|call| call.state == "held").count() != 1)) {
            return Err(OfonoError::StaleCall);
        }
        self.call_manager()?.call("SwapCalls", &()).map_err(Into::into)
    }

    pub fn send_tone(&self, call_id: &str, tone: char) -> Result<(), OfonoError> {
        if !valid_tone(tone) {
            return Err(OfonoError::InvalidTone);
        }
        let Observation::Call(call) = self.snapshot()? else {
            return Err(OfonoError::StaleCall);
        };
        if call.id != call_id || call.phase != CallPhase::Active {
            return Err(OfonoError::StaleCall);
        }
        self.call_manager()?
            .call("SendTones", &(tone.to_string()))
            .map_err(Into::into)
    }

    fn call_manager(&self) -> Result<Proxy<'_>, OfonoError> {
        Proxy::new(
            &self.connection,
            "org.ofono",
            self.modem.as_str(),
            "org.ofono.VoiceCallManager",
        )
        .map_err(Into::into)
    }
}

fn count(calls: &[OfonoCall], state: &str) -> usize {
    calls.iter().filter(|call| call.state == state).count()
}

/// Every call is active or held, with at least one of each.
fn active_and_held(calls: &[OfonoCall]) -> bool {
    count(calls, "active") > 0 && count(calls, "held") > 0
        && calls.iter().all(|call| matches!(call.state.as_str(), "active" | "held"))
}

/// One call, or one whole conference, all active; HFP conferences hold at most five calls.
fn can_add_call(calls: &[OfonoCall]) -> bool {
    (1..5).contains(&calls.len()) && count(calls, "active") == calls.len()
        && (calls.len() == 1 || calls.iter().all(|call| call.multiparty))
}

/// Each side is one plain call or only conference members.
fn can_swap(calls: &[OfonoCall]) -> bool {
    let side = |state| {
        let members: Vec<_> = calls.iter().filter(|call| call.state == state).collect();
        members.len() == 1 || members.iter().all(|call| call.multiparty)
    };
    active_and_held(calls) && side("active") && side("held")
}

fn can_merge(calls: &[OfonoCall]) -> bool {
    active_and_held(calls) && calls.len() <= 5
}

/// An active conference; a separate held call may remain.
fn can_end_conference(calls: &[OfonoCall]) -> bool {
    calls.iter().filter(|call| call.multiparty).count() >= 2
        && calls.iter().all(|call| if call.multiparty { call.state == "active" } else { call.state == "held" })
}

/// Hangup releases one participant, conference members included, or rejects a waiting call.
fn can_end_call(calls: &[OfonoCall], id: &str) -> bool {
    calls.iter().any(|call| call.path == id
        && matches!(call.state.as_str(), "active" | "held" | "dialing" | "alerting" | "waiting"))
}

fn can_private_chat(calls: &[OfonoCall], id: &str) -> bool {
    calls.len() >= 2 && calls.iter().all(|call| call.multiparty && call.state == "active")
        && calls.iter().any(|call| call.path == id)
}

/// `id` is the only waiting call and nothing else is ringing or being dialed.
fn answerable_waiting(calls: &[OfonoCall], id: &str) -> bool {
    count(calls, "waiting") == 1
        && calls.iter().any(|call| call.path == id && call.state == "waiting")
        && !calls.iter().any(|call| matches!(call.state.as_str(), "incoming" | "dialing" | "alerting"))
}

fn can_hold_and_answer(calls: &[OfonoCall], id: &str) -> bool {
    answerable_waiting(calls, id) && count(calls, "held") == 0
}

fn can_release_and_answer(calls: &[OfonoCall], id: &str) -> bool {
    answerable_waiting(calls, id)
}

fn decode_call(path: OwnedObjectPath, properties: Properties) -> Result<OfonoCall, OfonoError> {
    let state = properties
        .get("State")
        .and_then(|value| <&str>::try_from(value).ok())
        .ok_or(OfonoError::UnsupportedCallState)?;
    Ok(OfonoCall {
        path: path.to_string(),
        state: state.to_owned(),
        multiparty: property_bool(&properties, "Multiparty").unwrap_or(false),
        number: property_string(&properties, "LineIdentification")
            .filter(|number| number != "withheld"),
        name: property_string(&properties, "Name"),
    })
}

fn is_backend_event(message: &message::Message, modem: &str) -> bool {
    let header = message.header();
    let (Some(path), Some(interface), Some(member)) = (header.path(), header.interface(), header.member()) else {
        return false;
    };
    let path = path.as_str();
    match (interface.as_str(), member.as_str()) {
        ("org.ofono.VoiceCallManager", "CallAdded" | "CallRemoved") => path == modem,
        ("org.ofono.VoiceCall", "PropertyChanged" | "DisconnectReason") =>
            path.strip_prefix(modem).is_some_and(|rest| rest.starts_with('/')),
        ("org.ofono.Handsfree", "PropertyChanged") => path == modem,
        _ => false,
    }
}

fn is_modem_removed(message: &message::Message, modem: &str) -> bool {
    let header = message.header();
    header.path().is_some_and(|path| path.as_str() == "/")
        && header.interface().is_some_and(|interface| interface.as_str() == "org.ofono.Manager")
        && header.member().is_some_and(|member| member.as_str() == "ModemRemoved")
        && message.body().deserialize::<(OwnedObjectPath,)>()
            .is_ok_and(|(path,)| path.as_str() == modem)
}

fn property_bool(properties: &Properties, name: &str) -> Option<bool> {
    properties.get(name).and_then(|value| bool::try_from(value).ok())
}

fn property_string(properties: &Properties, name: &str) -> Option<String> {
    properties.get(name)
        .and_then(|value| <&str>::try_from(value).ok())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
}

pub(crate) fn phase_for_state(state: &str) -> Option<CallPhase> {
    match state {
        "incoming" => Some(CallPhase::Incoming),
        "dialing" => Some(CallPhase::OutgoingDialing),
        "alerting" => Some(CallPhase::OutgoingAlerting),
        "active" => Some(CallPhase::Active),
        "disconnected" => Some(CallPhase::Ending),
        _ => None,
    }
}

/// oFono names HFP modems after the BlueZ device path (`.../dev_AA_BB_CC_DD_EE_FF`).
pub(crate) fn modem_address(modem: &str) -> Option<String> {
    let octets: Vec<&str> = modem.rsplit_once("/dev_")?.1.split('_').collect();
    let valid = octets.len() == 6 && octets.iter().all(|octet| octet.len() == 2 && octet.bytes().all(|b| b.is_ascii_hexdigit()));
    valid.then(|| octets.join(":"))
}

pub(crate) fn valid_number(number: &str) -> bool {
    let digits = number.strip_prefix('+').unwrap_or(number);
    (1..=80).contains(&digits.len())
        && digits.bytes().all(|byte| byte.is_ascii_digit() || byte == b'*' || byte == b'#')
}

pub(crate) fn valid_tone(tone: char) -> bool {
    tone.is_ascii_digit() || tone == '*' || tone == '#'
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Command, Stdio};
    use std::sync::{Arc, Mutex};

    #[test]
    fn hfp_modem_path_names_the_phone_bluetooth_address() {
        assert_eq!(modem_address("/hfp/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0F").as_deref(), Some("AA:BB:CC:DD:EE:0F"));
        assert_eq!(modem_address("/hfp/org/bluez/hci0/dev_AA_BB"), None);
        assert_eq!(modem_address("/ril_0"), None);
        assert_eq!(modem_address("/hfp/org/bluez/hci0/dev_AA_BB_CC_DD_EE_GG"), None);
    }
    use zbus::blocking::connection::Builder;
    use zbus::zvariant::{Str, Value};

    struct PrivateBus(Child);

    impl PrivateBus {
        fn start() -> (Self, String) {
            let mut child = Command::new("dbus-daemon")
                .args(["--session", "--nofork", "--print-address=1", "--address=unix:tmpdir=/tmp"])
                .stdout(Stdio::piped())
                .stderr(Stdio::null())
                .spawn()
                .unwrap();
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

    fn string_value(value: &str) -> OwnedValue {
        OwnedValue::from(Str::from(value))
    }

    fn call_properties(state: &str) -> Properties {
        HashMap::from([
            ("State".to_owned(), string_value(state)),
            ("Multiparty".to_owned(), OwnedValue::from(false)),
        ])
    }

    struct MockManager;

    #[zbus::interface(name = "org.ofono.Manager")]
    impl MockManager {
        fn get_modems(&self) -> Objects {
            let interfaces = OwnedValue::try_from(Value::from(vec!["org.ofono.VoiceCallManager"]))
                .unwrap();
            vec![(
                OwnedObjectPath::try_from("/test_modem").unwrap(),
                HashMap::from([
                    ("Powered".to_owned(), OwnedValue::from(true)),
                    ("Online".to_owned(), OwnedValue::from(true)),
                    ("Interfaces".to_owned(), interfaces),
                ]),
            )]
        }
    }

    struct ReadinessManager(Arc<Mutex<(bool, bool, bool)>>);

    #[zbus::interface(name = "org.ofono.Manager")]
    impl ReadinessManager {
        fn get_modems(&self) -> Objects {
            let (powered, online, voice_calls) = *self.0.lock().unwrap();
            let interfaces = if voice_calls { vec!["org.ofono.VoiceCallManager"] } else { Vec::new() };
            vec![(
                OwnedObjectPath::try_from("/test_modem").unwrap(),
                HashMap::from([
                    ("Powered".to_owned(), OwnedValue::from(powered)),
                    ("Online".to_owned(), OwnedValue::from(online)),
                    ("Interfaces".to_owned(), OwnedValue::try_from(Value::from(interfaces)).unwrap()),
                ]),
            )]
        }
    }

    #[test]
    fn phone_is_ready_only_after_power_online_and_call_interface_are_available() {
        let (_bus, address) = PrivateBus::start();
        let readiness = Arc::new(Mutex::new((false, false, false)));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", ReadinessManager(readiness.clone())).unwrap()
            .serve_at("/test_modem", MockCalls(Arc::new(Mutex::new(MockState::default())))).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        for state in [(false, false, false), (false, true, true), (true, false, true), (true, true, false)] {
            *readiness.lock().unwrap() = state;
            assert!(matches!(OfonoBackend::connect_on(client.clone()), Err(OfonoError::NoReadyModem)));
        }
        *readiness.lock().unwrap() = (true, true, true);
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.snapshot(), Ok(Observation::Idle));
    }

    #[derive(Default)]
    struct MockState {
        call_state: Option<&'static str>,
        second_state: Option<&'static str>,
        multiparty: bool,
        /// Calls three and up: (state, multiparty).
        more: Vec<(&'static str, bool)>,
        commands: Vec<&'static str>,
    }

    struct MockCalls(Arc<Mutex<MockState>>);

    #[zbus::interface(name = "org.ofono.VoiceCallManager")]
    impl MockCalls {
        fn get_calls(&self) -> Objects {
            let state = self.0.lock().unwrap();
            let mut calls = state.call_state.map_or_else(Vec::new, |phase| {
                let mut properties = call_properties(phase);
                properties.insert("Multiparty".into(), OwnedValue::from(state.multiparty));
                vec![(OwnedObjectPath::try_from("/test_modem/call1").unwrap(), properties)]
            });
            if let Some(phase) = state.second_state {
                let mut properties = call_properties(phase);
                properties.insert("Multiparty".into(), OwnedValue::from(state.multiparty));
                calls.push((OwnedObjectPath::try_from("/test_modem/call2").unwrap(), properties));
            }
            for (index, (phase, multiparty)) in state.more.iter().enumerate() {
                let mut properties = call_properties(phase);
                properties.insert("Multiparty".into(), OwnedValue::from(*multiparty));
                calls.push((OwnedObjectPath::try_from(format!("/test_modem/call{}", index + 3)).unwrap(), properties));
            }
            calls
        }

        fn hold_and_answer(&self) {
            self.0.lock().unwrap().commands.push("HoldAndAnswer");
        }

        fn release_and_answer(&self) {
            self.0.lock().unwrap().commands.push("ReleaseAndAnswer");
        }

        fn private_chat(&self, _call: OwnedObjectPath) -> Vec<OwnedObjectPath> {
            self.0.lock().unwrap().commands.push("PrivateChat");
            vec![OwnedObjectPath::try_from("/test_modem/call2").unwrap()]
        }

        fn swap_calls(&self) {
            self.0.lock().unwrap().commands.push("SwapCalls");
        }

        fn create_multiparty(&self) -> Vec<OwnedObjectPath> {
            self.0.lock().unwrap().commands.push("CreateMultiparty");
            vec![OwnedObjectPath::try_from("/test_modem/call1").unwrap(), OwnedObjectPath::try_from("/test_modem/call2").unwrap()]
        }

        fn hangup_multiparty(&self) {
            self.0.lock().unwrap().commands.push("HangupMultiparty");
        }

        fn dial(&self, _number: &str, _hide_callerid: &str) -> OwnedObjectPath {
            self.0.lock().unwrap().commands.push("Dial");
            OwnedObjectPath::try_from("/test_modem/call1").unwrap()
        }

        fn send_tones(&self, _tones: &str) {
            self.0.lock().unwrap().commands.push("SendTones");
        }
    }

    struct MockCall(Arc<Mutex<MockState>>);

    #[zbus::interface(name = "org.ofono.VoiceCall")]
    impl MockCall {
        fn answer(&self) {
            self.0.lock().unwrap().commands.push("Answer");
        }

        fn hangup(&self) {
            self.0.lock().unwrap().commands.push("Hangup");
        }
    }

    struct MockHandsfree;

    #[zbus::interface(name = "org.ofono.Handsfree")]
    impl MockHandsfree {
        fn get_properties(&self) -> Properties {
            HashMap::from([("BatteryChargeLevel".to_owned(), OwnedValue::from(5u8))])
        }
    }

    /// Records every VoiceRecognition write, in order.
    struct VoiceHandsfree(Arc<Mutex<Vec<bool>>>);

    #[zbus::interface(name = "org.ofono.Handsfree")]
    impl VoiceHandsfree {
        fn set_property(&self, name: &str, value: OwnedValue) -> zbus::fdo::Result<()> {
            assert_eq!(name, "VoiceRecognition");
            self.0.lock().unwrap().push(bool::try_from(value).unwrap());
            Ok(())
        }
    }

    #[test]
    fn siri_starts_only_without_calls_and_can_always_stop() {
        let (_bus, address) = PrivateBus::start();
        let voice = Arc::new(Mutex::new(Vec::new()));
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .serve_at("/test_modem", VoiceHandsfree(voice.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.set_voice_assistant(true), Err(OfonoError::StaleCall));
        assert!(voice.lock().unwrap().is_empty());
        backend.set_voice_assistant(false).unwrap();
        assert_eq!(*voice.lock().unwrap(), [false]);
        state.lock().unwrap().call_state = None;
        voice.lock().unwrap().clear();
        backend.set_voice_assistant(true).unwrap();
        // The iPhone never reports the end of a Siri session, so oFono keeps VoiceRecognition true and
        // would ignore a second "true"; each start resets it first so the phone receives AT+BVRA=1 again.
        assert_eq!(*voice.lock().unwrap(), [false, true]);
    }

    #[test]
    fn connected_phone_battery_is_reported_as_raw_level() {
        let (_bus, address) = PrivateBus::start();
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockHandsfree).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.battery_level(), Some(5));
    }

    #[test]
    fn private_bus_reconciles_and_guards_real_dbus_method_calls() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("incoming"), ..Default::default() }));
        let server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .serve_at("/test_modem/call1", MockCall(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.snapshot(), Ok(Observation::Call(Call::new(
            "/test_modem/call1", CallPhase::Incoming, AudioLocation::Unknown,
        ))));
        backend.act("/test_modem/call1", CallAction::Answer).unwrap();
        state.lock().unwrap().call_state = Some("active");
        assert_eq!(backend.act("/test_modem/call1", CallAction::Answer), Err(OfonoError::StaleCall));
        backend.send_tone("/test_modem/call1", '5').unwrap();
        backend.act("/test_modem/call1", CallAction::HangUp).unwrap();
        state.lock().unwrap().call_state = None;
        backend.dial("+123").unwrap();
        assert_eq!(state.lock().unwrap().commands, ["Answer", "SendTones", "Hangup", "Dial"]);

        let mut events = backend.subscribe().unwrap();
        assert!(events.calls().is_empty());
        let path = OwnedObjectPath::try_from("/test_modem/call1").unwrap();
        state.lock().unwrap().call_state = Some("incoming");
        server.emit_signal(None::<&str>, "/test_modem", "org.ofono.VoiceCallManager", "CallAdded",
            &(path.clone(), call_properties("incoming"))).unwrap();
        assert_eq!(events.next_update().unwrap()[0].state, "incoming");
        state.lock().unwrap().call_state = Some("active");
        server.emit_signal(None::<&str>, "/test_modem/call1", "org.ofono.VoiceCall", "PropertyChanged",
            &("State", string_value("active"))).unwrap();
        assert_eq!(events.next_update().unwrap()[0].state, "active");
        state.lock().unwrap().call_state = None;
        server.emit_signal(None::<&str>, "/test_modem", "org.ofono.VoiceCallManager", "CallRemoved",
            &(path,)).unwrap();
        assert!(events.next_update().unwrap().is_empty());
    }

    #[test]
    fn hold_and_resume_target_only_the_current_single_call() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.hold("/test_modem/old"), Err(OfonoError::StaleCall));
        backend.hold("/test_modem/call1").unwrap();
        state.lock().unwrap().call_state = Some("held");
        assert_eq!(backend.hold("/test_modem/call1"), Err(OfonoError::StaleCall));
        backend.resume("/test_modem/call1").unwrap();
        assert_eq!(state.lock().unwrap().commands, ["SwapCalls", "SwapCalls"]);
    }

    #[test]
    fn active_and_held_calls_can_swap_by_target_id() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("held"), second_state: Some("active"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        backend.resume("/test_modem/call1").unwrap();
        backend.hold("/test_modem/call2").unwrap();
        assert_eq!(backend.hold("/test_modem/call1"), Err(OfonoError::StaleCall));
        assert_eq!(state.lock().unwrap().commands, ["SwapCalls", "SwapCalls"]);
    }

    #[test]
    fn adding_a_call_requires_only_active_calls_and_valid_number() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.add_call("12 3"), Err(OfonoError::InvalidNumber));
        backend.add_call("123").unwrap();
        state.lock().unwrap().second_state = Some("waiting");
        assert_eq!(backend.add_call("456"), Err(OfonoError::StaleCall));
        // A whole active conference may add a call too.
        { let mut state = state.lock().unwrap(); state.second_state = Some("active"); state.multiparty = true; }
        backend.add_call("789").unwrap();
        assert_eq!(state.lock().unwrap().commands, ["Dial", "Dial"]);
    }

    #[test]
    fn merge_requires_only_active_and_held_calls() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), second_state: Some("waiting"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.merge(), Err(OfonoError::StaleCall));
        state.lock().unwrap().second_state = Some("held");
        backend.merge().unwrap();
        // An active conference merges a further held call.
        { let mut state = state.lock().unwrap(); state.second_state = Some("active"); state.multiparty = true; state.more = vec![("held", false)]; }
        backend.merge().unwrap();
        assert_eq!(state.lock().unwrap().commands, ["CreateMultiparty", "CreateMultiparty"]);
    }

    #[test]
    fn conference_hangup_needs_confirmed_multiparty_calls() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), second_state: Some("active"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.end_conference(), Err(OfonoError::StaleCall));
        state.lock().unwrap().multiparty = true;
        backend.end_conference().unwrap();
        // A separate held call may remain; a held conference cannot be ended from here.
        state.lock().unwrap().more = vec![("held", false)];
        backend.end_conference().unwrap();
        { let mut state = state.lock().unwrap(); state.call_state = Some("held"); state.second_state = Some("held"); state.more = vec![("active", false)]; }
        assert_eq!(backend.end_conference(), Err(OfonoError::StaleCall));
        assert_eq!(state.lock().unwrap().commands, ["HangupMultiparty", "HangupMultiparty"]);
    }

    #[test]
    fn end_call_targets_one_current_participant_including_conference_members() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("held"), second_state: Some("dialing"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .serve_at("/test_modem/call1", MockCall(state.clone())).unwrap()
            .serve_at("/test_modem/call2", MockCall(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.end_call("/test_modem/old"), Err(OfonoError::StaleCall));
        backend.end_call("/test_modem/call2").unwrap();
        state.lock().unwrap().multiparty = true;
        backend.end_call("/test_modem/call1").unwrap();
        // A waiting call is rejected with Hangup; a ringing call is declined through Decline instead.
        { let mut state = state.lock().unwrap(); state.multiparty = false; state.call_state = Some("active"); state.second_state = Some("waiting"); }
        backend.end_call("/test_modem/call2").unwrap();
        state.lock().unwrap().second_state = Some("incoming");
        assert_eq!(backend.end_call("/test_modem/call2"), Err(OfonoError::StaleCall));
        assert_eq!(state.lock().unwrap().commands, ["Hangup", "Hangup", "Hangup"]);
    }

    #[test]
    fn waiting_swap_and_private_chat_send_their_ofono_commands() {
        let (_bus, address) = PrivateBus::start();
        let state = Arc::new(Mutex::new(MockState { call_state: Some("active"), second_state: Some("waiting"), ..Default::default() }));
        let _server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(state.clone())).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        assert_eq!(backend.hold_and_answer("/test_modem/call1"), Err(OfonoError::StaleCall));
        backend.hold_and_answer("/test_modem/call2").unwrap();
        backend.release_and_answer("/test_modem/call2").unwrap();
        assert_eq!(backend.swap(), Err(OfonoError::StaleCall));
        assert_eq!(backend.private_chat("/test_modem/call1"), Err(OfonoError::StaleCall));
        state.lock().unwrap().second_state = Some("held");
        backend.swap().unwrap();
        { let mut state = state.lock().unwrap(); state.second_state = Some("active"); state.multiparty = true; }
        backend.private_chat("/test_modem/call1").unwrap();
        state.lock().unwrap().more = vec![("held", false)];
        assert_eq!(backend.private_chat("/test_modem/call1"), Err(OfonoError::StaleCall));
        assert_eq!(backend.hold_and_answer("/test_modem/call3"), Err(OfonoError::StaleCall));
        assert_eq!(state.lock().unwrap().commands, ["HoldAndAnswer", "ReleaseAndAnswer", "SwapCalls", "PrivateChat"]);
    }

    fn listed(states: &[(&str, bool)]) -> Vec<OfonoCall> {
        states.iter().enumerate().map(|(index, (state, multiparty))| OfonoCall {
            path: format!("/c{}", index + 1), state: (*state).into(), multiparty: *multiparty, number: None, name: None,
        }).collect()
    }

    const A: (&str, bool) = ("active", false);
    const H: (&str, bool) = ("held", false);
    const W: (&str, bool) = ("waiting", false);
    const MA: (&str, bool) = ("active", true);
    const MH: (&str, bool) = ("held", true);

    #[test]
    fn add_call_needs_one_active_call_or_one_active_conference_below_five() {
        assert!(can_add_call(&listed(&[A])));
        assert!(can_add_call(&listed(&[MA, MA, MA, MA])));
        for calls in [&[][..], &[MA; 5], &[A, A], &[A, H], &[A, W], &[H], &[("dialing", false)], &[MA, MA, A], &[A, ("incoming", false)]] {
            assert!(!can_add_call(&listed(calls)));
        }
    }

    #[test]
    fn swap_needs_one_active_and_one_held_side() {
        for calls in [&[A, H][..], &[MA, MA, H], &[MH, MH, A], &[H, MA, MA]] {
            assert!(can_swap(&listed(calls)));
        }
        for calls in [&[A][..], &[H], &[A, A], &[A, H, H], &[MA, A, H], &[A, W], &[A, H, W], &[A, ("alerting", false)]] {
            assert!(!can_swap(&listed(calls)));
        }
    }

    #[test]
    fn merge_needs_active_and_held_calls_up_to_five() {
        for calls in [&[A, H][..], &[MA, MA, H], &[MH, MH, A], &[MA, MA, MA, MA, H]] {
            assert!(can_merge(&listed(calls)));
        }
        for calls in [&[A][..], &[A, A], &[MA, MA, MA, MA, MA, H], &[A, W], &[A, H, W], &[H, ("dialing", false)]] {
            assert!(!can_merge(&listed(calls)));
        }
    }

    #[test]
    fn end_call_targets_any_current_participant_but_not_a_ringing_call() {
        for (calls, id) in [(&[A][..], "/c1"), (&[H, ("dialing", false)], "/c2"), (&[("alerting", false)], "/c1"),
                            (&[MA, MA, H], "/c2"), (&[MH, MH, A], "/c1"), (&[A, W], "/c2")] {
            assert!(can_end_call(&listed(calls), id));
        }
        for (calls, id) in [(&[A][..], "/old"), (&[("incoming", false)], "/c1"), (&[("disconnected", false)], "/c1")] {
            assert!(!can_end_call(&listed(calls), id));
        }
    }

    #[test]
    fn private_chat_needs_an_active_conference_and_nothing_else() {
        assert!(can_private_chat(&listed(&[MA, MA]), "/c1"));
        assert!(can_private_chat(&listed(&[MA, MA, MA]), "/c3"));
        for (calls, id) in [(&[MA][..], "/c1"), (&[MA, MA, H], "/c1"), (&[MA, MH], "/c1"), (&[MH, MH], "/c1"),
                            (&[A, H], "/c1"), (&[MA, MA], "/c3"), (&[MA, MA, W], "/c1")] {
            assert!(!can_private_chat(&listed(calls), id));
        }
    }

    #[test]
    fn hold_and_answer_needs_the_only_waiting_call_and_no_held_call() {
        assert!(can_hold_and_answer(&listed(&[A, W]), "/c2"));
        assert!(can_hold_and_answer(&listed(&[MA, MA, W]), "/c3"));
        for (calls, id) in [(&[A, W][..], "/c1"), (&[H, A, W], "/c3"), (&[A, W, W], "/c2"), (&[("incoming", false), W], "/c2"),
                            (&[("dialing", false), W], "/c2"), (&[("alerting", false), W], "/c2"), (&[A], "/c1")] {
            assert!(!can_hold_and_answer(&listed(calls), id));
        }
    }

    #[test]
    fn release_and_answer_needs_the_only_waiting_call_and_no_ringing_or_dialing_call() {
        for (calls, id) in [(&[A, W][..], "/c2"), (&[H, A, W], "/c3"), (&[H, W], "/c2"), (&[MA, MA, W], "/c3")] {
            assert!(can_release_and_answer(&listed(calls), id));
        }
        for (calls, id) in [(&[A, W][..], "/c1"), (&[A, W, W], "/c2"), (&[("incoming", false), W], "/c2"),
                            (&[("dialing", false), W], "/c2"), (&[("alerting", false), W], "/c2")] {
            assert!(!can_release_and_answer(&listed(calls), id));
        }
    }

    #[test]
    fn end_conference_needs_an_active_conference_and_only_a_held_call_beside_it() {
        assert!(can_end_conference(&listed(&[MA, MA])));
        assert!(can_end_conference(&listed(&[MA, MA, H])));
        for calls in [&[MA][..], &[A, A], &[MH, MH], &[MH, MH, A], &[MA, MA, A], &[MA, MA, W]] {
            assert!(!can_end_conference(&listed(calls)));
        }
    }

    #[test]
    fn removing_the_connected_modem_ends_its_event_stream() {
        let (_bus, address) = PrivateBus::start();
        let server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .serve_at("/test_modem", MockCalls(Arc::new(Mutex::new(MockState::default())))).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let backend = OfonoBackend::connect_on(client).unwrap();
        let mut events = backend.subscribe().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || { let _ = tx.send(events.next_update()); });
        server.emit_signal(None::<&str>, "/", "org.ofono.Manager", "ModemRemoved",
            &(OwnedObjectPath::try_from("/test_modem").unwrap(),)).unwrap();
        assert!(matches!(rx.recv_timeout(std::time::Duration::from_secs(1)), Ok(Err(OfonoError::BackendUnavailable))));
    }

    #[test]
    fn a_phone_coming_online_wakes_the_modem_watch() {
        let (_bus, address) = PrivateBus::start();
        let server = Builder::address(address.as_str()).unwrap()
            .name("org.ofono").unwrap()
            .serve_at("/", MockManager).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        let mut watch = ModemWatch::on(client).unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || loop {
            if tx.send(watch.next_change()).is_err() { break; }
        });
        // Call activity is not a modem change.
        server.emit_signal(None::<&str>, "/test_modem/call1", "org.ofono.VoiceCall", "PropertyChanged",
            &("State", zbus::zvariant::Value::from("active"))).unwrap();
        assert!(rx.recv_timeout(std::time::Duration::from_millis(300)).is_err());
        server.emit_signal(None::<&str>, "/test_modem", "org.ofono.Modem", "PropertyChanged",
            &("Online", zbus::zvariant::Value::from(true))).unwrap();
        assert!(matches!(rx.recv_timeout(std::time::Duration::from_secs(1)), Ok(Ok(()))));
    }

    #[test]
    fn dial_numbers_follow_ofono_131_format() {
        assert!(valid_number("+20123"));
        assert!(valid_number("*123#"));
        assert!(!valid_number("+"));
        assert!(!valid_number("12 34"));
        assert!(!valid_number("12;34"));
        assert!(!valid_number(&"1".repeat(81)));
    }

    #[test]
    fn call_states_preserve_unsupported_cases() {
        assert_eq!(phase_for_state("incoming"), Some(CallPhase::Incoming));
        assert_eq!(phase_for_state("dialing"), Some(CallPhase::OutgoingDialing));
        assert_eq!(phase_for_state("alerting"), Some(CallPhase::OutgoingAlerting));
        assert_eq!(phase_for_state("active"), Some(CallPhase::Active));
        assert_eq!(phase_for_state("disconnected"), Some(CallPhase::Ending));
        assert_eq!(phase_for_state("held"), None);
        assert_eq!(phase_for_state("waiting"), None);
    }

    #[test]
    fn dtmf_is_one_supported_key() {
        for tone in "0123456789*#".chars() {
            assert!(valid_tone(tone));
        }
        assert!(!valid_tone('+'));
        assert!(!valid_tone('A'));
    }

    #[test]
    fn withheld_caller_identity_is_absent() {
        let path = OwnedObjectPath::try_from("/test_modem/call1").unwrap();
        let mut properties = call_properties("incoming");
        properties.insert("LineIdentification".to_owned(), string_value("withheld"));
        properties.insert("Name".to_owned(), string_value("  "));
        let call = decode_call(path, properties).unwrap();
        assert_eq!(call.number, None);
        assert_eq!(call.name, None);
    }
}
