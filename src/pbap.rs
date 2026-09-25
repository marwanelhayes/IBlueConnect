use std::collections::HashMap;
use std::fmt;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use zbus::blocking::{Connection, MessageIterator, Proxy};
use zbus::zvariant::{OwnedObjectPath, OwnedValue, Value};
use zbus::{MatchRule, message};

const OBEX: &str = "org.bluez.obex";

/// Contact download failed; deliberately carries no phone or contact details.
#[derive(Debug)]
pub struct PbapError;

impl fmt::Display for PbapError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str("Contact download failed") }
}

impl std::error::Error for PbapError {}

impl From<zbus::Error> for PbapError {
    fn from(_: zbus::Error) -> Self { Self }
}

/// Downloads one PBAP phonebook (`pb` contacts, `cch`/`mch`/`ich`/`och` call history) as vCard text
/// over an already connected Bluetooth link.
/// obexd opens only the PBAP profile to `address`; it never pairs or powers Bluetooth.
pub fn pull_phonebook(session_bus: &Connection, address: &str, book: &str, timeout: Duration) -> Result<String, PbapError> {
    let client = Proxy::new(session_bus, OBEX, "/org/bluez/obex", "org.bluez.obex.Client1")?;
    let args = HashMap::from([("Target", Value::from("PBAP"))]);
    let session: OwnedObjectPath = client.call("CreateSession", &(address, args))?;
    let result = pull(session_bus, &session, book, timeout);
    let _ = client.call::<_, _, ()>("RemoveSession", &(&session,));
    result
}

fn pull(session_bus: &Connection, session: &OwnedObjectPath, book: &str, timeout: Duration) -> Result<String, PbapError> {
    // Subscribe before PullAll: obexd can finish, signal, and drop the transfer before the reply is read.
    let rule = MatchRule::builder()
        .msg_type(message::Type::Signal)
        .sender(OBEX)?
        .interface("org.freedesktop.DBus.Properties")?
        .member("PropertiesChanged")?
        .build();
    let messages = MessageIterator::for_match_rule(rule, session_bus, Some(64))?;
    let (tx, rx) = mpsc::channel();
    // ponytail: on timeout this reader lingers until obexd's next signal; fine for a rare manual sync.
    thread::spawn(move || {
        for message in messages.flatten() {
            if tx.send(message).is_err() { break; }
        }
    });
    let phonebook = Proxy::new(session_bus, OBEX, session.as_str(), "org.bluez.obex.PhonebookAccess1")?;
    phonebook.call::<_, _, ()>("Select", &("int", book))?;
    let filters = HashMap::from([("Format", Value::from("vcard30"))]);
    let (transfer, properties): (OwnedObjectPath, HashMap<String, OwnedValue>) =
        phonebook.call("PullAll", &("", filters))?;
    let file = properties.get("Filename").and_then(|value| <&str>::try_from(value).ok())
        .map(str::to_owned).ok_or(PbapError)?;
    let completed = wait_for_transfer(&rx, transfer.as_str(), timeout);
    let text = completed.and_then(|_| std::fs::read(&file).map_err(|_| PbapError));
    let _ = std::fs::remove_file(&file);
    text.map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
}

fn wait_for_transfer(rx: &mpsc::Receiver<message::Message>, transfer: &str, timeout: Duration) -> Result<(), PbapError> {
    let deadline = Instant::now() + timeout;
    loop {
        let message = rx.recv_timeout(deadline.saturating_duration_since(Instant::now())).map_err(|_| PbapError)?;
        if message.header().path().map(|path| path.as_str()) != Some(transfer) { continue; }
        let Ok((interface, changed, _)) = message.body().deserialize::<(String, HashMap<String, OwnedValue>, Vec<String>)>()
            else { continue };
        if interface != "org.bluez.obex.Transfer1" { continue; }
        match changed.get("Status").and_then(|value| <&str>::try_from(value).ok()) {
            Some("complete") => return Ok(()),
            Some("error") => return Err(PbapError),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::path::PathBuf;
    use std::process::{Child, Command, Stdio};
    use std::sync::{Arc, Mutex};
    use zbus::blocking::connection::Builder;

    struct PrivateBus(Child);

    impl PrivateBus {
        fn start() -> (Self, String) {
            let mut child = Command::new("dbus-daemon")
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

    const SESSION: &str = "/org/bluez/obex/client/session0";
    const TRANSFER: &str = "/org/bluez/obex/client/session0/transfer0";

    struct FakeClient(Arc<Mutex<Vec<String>>>);

    #[zbus::interface(name = "org.bluez.obex.Client1")]
    impl FakeClient {
        fn create_session(&self, destination: &str, args: HashMap<String, OwnedValue>) -> OwnedObjectPath {
            let target = args.get("Target").and_then(|value| <&str>::try_from(value).ok()).unwrap_or("").to_owned();
            self.0.lock().unwrap().push(format!("create {destination} {target}"));
            OwnedObjectPath::try_from(SESSION).unwrap()
        }

        fn remove_session(&self, session: OwnedObjectPath) {
            self.0.lock().unwrap().push(format!("remove {}", session.as_str()));
        }
    }

    struct FakePhonebook { file: PathBuf, status: &'static str, calls: Arc<Mutex<Vec<String>>> }

    #[zbus::interface(name = "org.bluez.obex.PhonebookAccess1")]
    impl FakePhonebook {
        fn select(&self, location: &str, phonebook: &str) {
            self.calls.lock().unwrap().push(format!("select {location} {phonebook}"));
        }

        fn pull_all(&self, target: &str, filters: HashMap<String, OwnedValue>,
                    #[zbus(connection)] connection: &zbus::Connection) -> (OwnedObjectPath, HashMap<String, OwnedValue>) {
            let format = filters.get("Format").and_then(|value| <&str>::try_from(value).ok()).unwrap_or("").to_owned();
            self.calls.lock().unwrap().push(format!("pull [{target}] {format}"));
            std::fs::write(&self.file, "BEGIN:VCARD\r\nFN:Alice\r\nTEL:1\r\nEND:VCARD\r\n").unwrap();
            let connection = zbus::blocking::Connection::from(connection.clone());
            let status = self.status;
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(50));
                let changed = HashMap::from([("Status", Value::from(status))]);
                connection.emit_signal(None::<&str>, TRANSFER, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                    &("org.bluez.obex.Transfer1", changed, Vec::<String>::new())).unwrap();
            });
            let file = self.file.to_str().unwrap();
            (OwnedObjectPath::try_from(TRANSFER).unwrap(),
             HashMap::from([("Filename".to_owned(), OwnedValue::try_from(Value::from(file)).unwrap())]))
        }
    }

    fn fake_obex(status: &'static str) -> (PrivateBus, Connection, zbus::blocking::Connection, PathBuf, Arc<Mutex<Vec<String>>>) {
        let (bus, address) = PrivateBus::start();
        let calls = Arc::new(Mutex::new(Vec::new()));
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let unique = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let file = std::env::temp_dir().join(format!("iblueconnect-pbap-{}-{unique}.vcf", std::process::id()));
        let obex = Builder::address(address.as_str()).unwrap().name("org.bluez.obex").unwrap()
            .serve_at("/org/bluez/obex", FakeClient(calls.clone())).unwrap()
            .serve_at(SESSION, FakePhonebook { file: file.clone(), status, calls: calls.clone() }).unwrap()
            .build().unwrap();
        let client = Builder::address(address.as_str()).unwrap().build().unwrap();
        (bus, client, obex, file, calls)
    }

    #[test]
    fn pulls_phonebook_then_deletes_file_and_session() {
        let (_bus, client, _obex, file, calls) = fake_obex("complete");
        let vcards = pull_phonebook(&client, "AA:BB:CC:DD:EE:FF", "pb", Duration::from_secs(5)).unwrap();
        assert!(vcards.contains("FN:Alice"));
        assert!(!file.exists());
        assert_eq!(*calls.lock().unwrap(), [
            "create AA:BB:CC:DD:EE:FF PBAP", "select int pb", "pull [] vcard30", &format!("remove {SESSION}"),
        ]);
    }

    #[test]
    fn pulls_the_requested_call_history_phonebook() {
        let (_bus, client, _obex, _file, calls) = fake_obex("complete");
        pull_phonebook(&client, "AA:BB:CC:DD:EE:FF", "cch", Duration::from_secs(5)).unwrap();
        assert_eq!(calls.lock().unwrap()[1], "select int cch");
    }

    /// Opt-in hardware probe: `cargo test --lib live_call_history_probe -- --ignored --nocapture`.
    /// Prints only counts, never numbers, names, or times.
    #[test]
    #[ignore]
    fn live_call_history_probe() {
        let address = crate::ofono::OfonoBackend::connect().expect("oFono modem").phone_address().expect("phone address");
        let bus = Connection::session().unwrap();
        let text = pull_phonebook(&bus, &address, "cch", Duration::from_secs(60)).expect("call history pull");
        let cards = text.matches("BEGIN:VCARD").count();
        let dated = text.lines().filter(|line| line.contains("X-IRMC-CALL-DATETIME")).count();
        let kind = |tag: &str| text.lines().filter(|line| line.contains("X-IRMC-CALL-DATETIME") && line.contains(tag)).count();
        println!("cch entries={cards} dated={dated} missed={} received={} dialed={}",
                 kind("MISSED"), kind("RECEIVED"), kind("DIALED"));
    }

    #[test]
    fn failed_transfer_is_an_error_and_still_cleans_up() {
        let (_bus, client, _obex, file, calls) = fake_obex("error");
        assert!(pull_phonebook(&client, "AA:BB:CC:DD:EE:FF", "pb", Duration::from_secs(5)).is_err());
        assert!(!file.exists());
        assert_eq!(calls.lock().unwrap().last().unwrap(), &format!("remove {SESSION}"));
    }
}
