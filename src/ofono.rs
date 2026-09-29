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

    /// HFP reports the phone's battery in six coarse steps (0–5), not an exact percentage.
    pub fn battery_level(&self) -> Option<u8> {
        let handsfree = Proxy::new(&self.connection, "org.ofono", self.modem.as_str(), "org.ofono.Handsfree").ok()?;
        let properties: Properties = handsfree.call("GetProperties", &()).ok()?;
        properties.get("BatteryChargeLevel")
            .and_then(|value| u8::try_from(value).ok())
            .filter(|level| *level <= 5)
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
        let calls = self.calls()?;
        if calls.len() != 1 || calls[0].state != "active" || calls[0].multiparty {
            return Err(OfonoError::StaleCall);
        }
        let path: OwnedObjectPath = self.call_manager()?.call("Dial", &(number, "default"))?;
        Ok(path.to_string())
    }

    pub fn merge(&self) -> Result<(), OfonoError> {
        let calls = self.calls()?;
        if calls.len() != 2 || calls.iter().any(|call| call.multiparty)
            || calls.iter().filter(|call| call.state == "active").count() != 1
            || calls.iter().filter(|call| call.state == "held").count() != 1 {
            return Err(OfonoError::StaleCall);
        }
        let _: Vec<OwnedObjectPath> = self.call_manager()?.call("CreateMultiparty", &())?;
        Ok(())
    }

    pub fn end_conference(&self) -> Result<(), OfonoError> {
        let calls = self.calls()?;
        if calls.len() < 2 || calls.iter().any(|call| !call.multiparty || call.state != "active") {
            return Err(OfonoError::StaleCall);
        }
        self.call_manager()?.call("HangupMultiparty", &()).map_err(Into::into)
    }

    pub fn end_call(&self, call_id: &str) -> Result<(), OfonoError> {
        let calls = self.calls()?;
        if calls.iter().any(|call| call.multiparty)
            || !calls.iter().any(|call| call.path == call_id && matches!(call.state.as_str(), "active" | "held" | "dialing" | "alerting")) {
            return Err(OfonoError::StaleCall);
        }
        let call = Proxy::new(&self.connection, "org.ofono", call_id, "org.ofono.VoiceCall")?;
        call.call("Hangup", &()).map_err(Into::into)
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

    #[derive(Default)]
    struct MockState {
        call_state: Option<&'static str>,
        second_state: Option<&'static str>,
        multiparty: bool,
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
            calls
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
    fn adding_a_call_requires_one_active_call_and_valid_number() {
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
        assert_eq!(state.lock().unwrap().commands, ["Dial"]);
    }

    #[test]
    fn merge_requires_one_active_and_one_held_call() {
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
        assert_eq!(state.lock().unwrap().commands, ["CreateMultiparty"]);
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
        assert_eq!(state.lock().unwrap().commands, ["HangupMultiparty"]);
    }

    #[test]
    fn end_call_targets_only_a_current_nonconference_participant() {
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
        assert_eq!(backend.end_call("/test_modem/call1"), Err(OfonoError::StaleCall));
        assert_eq!(state.lock().unwrap().commands, ["Hangup"]);
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
