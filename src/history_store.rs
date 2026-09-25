use crate::history::{CallRecord, Direction, History, ObservedCall, Outcome};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

const MAX_FILE_BYTES: u64 = 4_000_000;

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid call history")
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(DIGITS[(byte >> 4) as usize] as char);
        out.push(DIGITS[(byte & 15) as usize] as char);
    }
    out
}

fn unhex(value: &str) -> io::Result<String> {
    if value.len() > 2048 || !value.len().is_multiple_of(2) {
        return Err(invalid());
    }
    let mut bytes = Vec::with_capacity(value.len() / 2);
    for pair in value.as_bytes().chunks_exact(2) {
        let high = (pair[0] as char).to_digit(16).ok_or_else(invalid)?;
        let low = (pair[1] as char).to_digit(16).ok_or_else(invalid)?;
        bytes.push(((high << 4) | low) as u8);
    }
    String::from_utf8(bytes).map_err(|_| invalid())
}

fn optional_hex(value: Option<&str>) -> io::Result<String> {
    match value {
        None => Ok("-".into()),
        Some(value) if value.len() <= 1024 => Ok(hex(value.as_bytes())),
        Some(_) => Err(io::Error::new(io::ErrorKind::InvalidInput, "call history field too long")),
    }
}

fn parse_optional(value: &str) -> io::Result<Option<String>> {
    if value == "-" { Ok(None) } else { unhex(value).map(Some) }
}

fn nanos(time: SystemTime) -> io::Result<u128> {
    Ok(time.duration_since(SystemTime::UNIX_EPOCH).map_err(|_| {
        io::Error::new(io::ErrorKind::InvalidInput, "call timestamp before Unix epoch")
    })?.as_nanos())
}

fn time(value: &str) -> io::Result<SystemTime> {
    let nanos: u128 = value.parse().map_err(|_| invalid())?;
    let secs: u64 = (nanos / 1_000_000_000).try_into().map_err(|_| invalid())?;
    SystemTime::UNIX_EPOCH
        .checked_add(Duration::new(secs, (nanos % 1_000_000_000) as u32))
        .ok_or_else(invalid)
}

fn encode(history: &History) -> io::Result<String> {
    let mut data = String::from("IBC1\n");
    for call in history.recent() {
        if call.id.len() > 1024 { return Err(io::Error::new(io::ErrorKind::InvalidInput, "call history field too long")); }
        let outcome = match call.outcome {
            Outcome::Completed => 'c', Outcome::Missed => 'm', Outcome::Declined => 'd',
            Outcome::Busy => 'b', Outcome::Failed => 'f', Outcome::Interrupted => 'i',
            Outcome::Unknown => 'u',
        };
        let direction = match call.direction { Direction::Incoming => 'i', Direction::Outgoing => 'o', Direction::Unknown => 'u' };
        let ended = match call.ended_at { Some(at) => nanos(at)?.to_string(), None => "-".into() };
        let line = format!("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            hex(call.id.as_bytes()), optional_hex(call.number.as_deref())?,
            optional_hex(call.name_snapshot.as_deref())?, direction, nanos(call.started_at)?,
            ended, call.active_duration.as_nanos(), outcome);
        data.push_str(&line);
    }
    if data.len() as u64 > MAX_FILE_BYTES { return Err(io::Error::new(io::ErrorKind::InvalidInput, "call history too large")); }
    Ok(data)
}

fn decode(data: &str) -> io::Result<History> {
    let mut lines = data.lines();
    if lines.next() != Some("IBC1") { return Err(invalid()); }
    let mut records = Vec::new();
    for line in lines {
        if records.len() == 500 { return Err(invalid()); }
        let parts: Vec<_> = line.split('\t').collect();
        let [id, number, name, direction, started, ended, duration, outcome] = parts.as_slice() else { return Err(invalid()); };
        let direction = match *direction { "i" => Direction::Incoming, "o" => Direction::Outgoing, "u" => Direction::Unknown, _ => return Err(invalid()) };
        let outcome = match *outcome {
            "c" => Outcome::Completed, "m" => Outcome::Missed, "d" => Outcome::Declined,
            "b" => Outcome::Busy, "f" => Outcome::Failed, "i" => Outcome::Interrupted,
            "u" => Outcome::Unknown, _ => return Err(invalid()),
        };
        let nanos: u128 = duration.parse().map_err(|_| invalid())?;
        let secs: u64 = (nanos / 1_000_000_000).try_into().map_err(|_| invalid())?;
        records.push(CallRecord {
            id: unhex(id)?, number: parse_optional(number)?, name_snapshot: parse_optional(name)?,
            direction, started_at: time(started)?,
            ended_at: if *ended == "-" { None } else { Some(time(ended)?) },
            active_duration: Duration::new(secs, (nanos % 1_000_000_000) as u32), outcome,
        });
    }
    Ok(History::from_records(records))
}

pub struct HistoryStore {
    path: PathBuf,
    history: History,
}

impl HistoryStore {
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        let history = match File::open(&path) {
            Ok(file) => {
                let mut data = String::new();
                file.take(MAX_FILE_BYTES + 1).read_to_string(&mut data)?;
                if data.len() as u64 > MAX_FILE_BYTES { return Err(invalid()); }
                decode(&data)?
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => History::default(),
            Err(error) => return Err(error),
        };
        Ok(Self { path, history })
    }

    pub fn recent(&self) -> impl Iterator<Item = &CallRecord> { self.history.recent() }

    pub fn observe(&mut self, call: ObservedCall) -> io::Result<bool> {
        let mut next = self.history.clone();
        if !next.observe(call) { return Ok(false); }
        self.save(&next)?;
        self.history = next;
        Ok(true)
    }

    pub fn finish(&mut self, id: &str, outcome: Outcome, ended_at: SystemTime, active_duration: Duration) -> io::Result<bool> {
        let mut next = self.history.clone();
        if !next.finish(id, outcome, ended_at, active_duration) { return Ok(false); }
        self.save(&next)?;
        self.history = next;
        Ok(true)
    }

    pub fn clear(&mut self) -> io::Result<()> {
        let next = History::default();
        self.save(&next)?;
        self.history = next;
        Ok(())
    }

    fn save(&self, history: &History) -> io::Result<()> {
        let data = encode(history)?;
        let parent = self.path.parent().filter(|path| !path.as_os_str().is_empty()).unwrap_or(Path::new("."));
        fs::DirBuilder::new().recursive(true).mode(0o700).create(parent)?;
        for suffix in 0..100 {
            let temporary = self.path.with_extension(format!("tmp-{}-{suffix}", std::process::id()));
            let file = OpenOptions::new().write(true).create_new(true).mode(0o600).open(&temporary);
            let Ok(mut file) = file else {
                let error = file.unwrap_err();
                if error.kind() == io::ErrorKind::AlreadyExists { continue; }
                return Err(error);
            };
            let result = (|| { file.write_all(data.as_bytes())?; file.sync_all()?; fs::rename(&temporary, &self.path) })();
            if result.is_err() { let _ = fs::remove_file(&temporary); }
            return result;
        }
        Err(io::Error::new(io::ErrorKind::AlreadyExists, "no temporary history path available"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn test_path() -> PathBuf {
        let stamp = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("iblueconnect-history-{}-{stamp}", std::process::id())).join("history")
    }

    #[test]
    fn history_survives_reopen_and_clear() {
        let path = test_path();
        let dir = path.parent().unwrap().to_path_buf();
        fs::create_dir_all(&dir).unwrap();
        let mut store = HistoryStore::open(&path).unwrap();
        assert!(store.observe(ObservedCall {
            id: "call-1".into(),
            number: Some("123".into()),
            name_snapshot: None,
            direction: Direction::Incoming,
            started_at: SystemTime::UNIX_EPOCH,
        }).unwrap());
        assert!(store.finish("call-1", Outcome::Missed, SystemTime::UNIX_EPOCH + Duration::from_secs(3), Duration::ZERO).unwrap());
        drop(store);
        let mut store = HistoryStore::open(&path).unwrap();
        let records: Vec<_> = store.recent().collect();
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].outcome, Outcome::Missed);
        assert_eq!(records[0].number.as_deref(), Some("123"));
        store.clear().unwrap();
        drop(store);
        assert_eq!(HistoryStore::open(&path).unwrap().recent().count(), 0);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn startup_call_with_unknown_direction_survives_reopen() {
        let path = test_path();
        let mut store = HistoryStore::open(&path).unwrap();
        assert!(store.observe(ObservedCall {
            id: "startup-call".into(),
            number: None,
            name_snapshot: None,
            direction: Direction::Unknown,
            started_at: SystemTime::UNIX_EPOCH,
        }).unwrap());
        drop(store);
        let store = HistoryStore::open(&path).unwrap();
        let record = store.recent().next().unwrap();
        assert_eq!(record.direction, Direction::Unknown);
        assert_eq!(record.display_name(), "Unknown caller");
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn private_identity_round_trips_without_exposing_file_permissions() {
        let path = test_path();
        let mut store = HistoryStore::open(&path).unwrap();
        assert!(store.observe(ObservedCall {
            id: "id\t1".into(),
            number: Some("+12\n34".into()),
            name_snapshot: Some("Name ☎".into()),
            direction: Direction::Outgoing,
            started_at: SystemTime::UNIX_EPOCH + Duration::from_nanos(123),
        }).unwrap());
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        drop(store);
        let store = HistoryStore::open(&path).unwrap();
        let record = store.recent().next().unwrap();
        assert_eq!(record.id, "id\t1");
        assert_eq!(record.number.as_deref(), Some("+12\n34"));
        assert_eq!(record.name_snapshot.as_deref(), Some("Name ☎"));
        assert_eq!(record.started_at, SystemTime::UNIX_EPOCH + Duration::from_nanos(123));
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn malformed_history_is_reported_without_erasing_it() {
        let path = test_path();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, b"broken").unwrap();
        assert_eq!(HistoryStore::open(&path).err().unwrap().kind(), io::ErrorKind::InvalidData);
        assert_eq!(fs::read(&path).unwrap(), b"broken");
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn impossible_timestamp_is_rejected_without_panicking() {
        let path = test_path();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        let impossible = u128::from(u64::MAX) * 1_000_000_000;
        fs::write(&path, format!("IBC1\n61\t-\t-\ti\t{impossible}\t-\t0\tu\n")).unwrap();
        assert_eq!(HistoryStore::open(&path).err().unwrap().kind(), io::ErrorKind::InvalidData);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn only_latest_five_hundred_survive_reopen() {
        let path = test_path();
        let mut store = HistoryStore::open(&path).unwrap();
        for index in 0..=500 {
            assert!(store.observe(ObservedCall {
                id: index.to_string(),
                number: None,
                name_snapshot: None,
                direction: Direction::Incoming,
                started_at: SystemTime::UNIX_EPOCH + Duration::from_secs(index),
            }).unwrap());
        }
        drop(store);
        let store = HistoryStore::open(&path).unwrap();
        assert_eq!(store.recent().count(), 500);
        assert_eq!(store.recent().next().unwrap().id, "500");
        assert_eq!(store.recent().last().unwrap().id, "1");
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }
}
