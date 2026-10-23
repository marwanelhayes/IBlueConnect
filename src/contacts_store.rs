use std::fs::{self, OpenOptions};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::Path;

use rusqlite::{Connection, OptionalExtension, params};

pub use crate::vcard::Entry;
use crate::call_log::{Kind, PhoneCall, StoredCall};

/// Private SQLite cache of the phone's contacts; replaced only after a complete sync.
pub struct ContactStore(Connection);

impl ContactStore {
    /// Clears only app-owned cached data; Bluetooth pairing is untouched.
    pub fn clear(&mut self) -> rusqlite::Result<()> {
        let transaction = self.0.transaction()?;
        transaction.execute_batch("DELETE FROM contacts; DELETE FROM phone_calls; DELETE FROM meta;")?;
        transaction.commit()
    }

    pub fn open(path: impl AsRef<Path>) -> rusqlite::Result<Self> {
        let path = path.as_ref();
        if let Some(parent) = path.parent() {
            fs::DirBuilder::new().recursive(true).mode(0o700).create(parent).map_err(io_error)?;
        }
        // Create the file ourselves so SQLite never makes it world-readable.
        OpenOptions::new().write(true).create(true).truncate(false).mode(0o600).open(path).map_err(io_error)?;
        let connection = Connection::open(path)?;
        connection.execute_batch(
            "CREATE TABLE IF NOT EXISTS contacts (name TEXT NOT NULL, label TEXT NOT NULL, number TEXT NOT NULL CHECK (number <> ''));
             CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
             CREATE TABLE IF NOT EXISTS phone_calls (number TEXT, name TEXT, kind TEXT NOT NULL, started_at INTEGER NOT NULL);")?;
        Ok(Self(connection))
    }

    /// Replaces every contact in one transaction; any failure keeps the last good cache.
    pub fn replace_all(&mut self, entries: &[Entry], synced_at: i64) -> rusqlite::Result<()> {
        let transaction = self.0.transaction()?;
        transaction.execute("DELETE FROM contacts", [])?;
        {
            let mut insert = transaction.prepare("INSERT INTO contacts (name, label, number) VALUES (?1, ?2, ?3)")?;
            for entry in entries {
                insert.execute(params![entry.name, entry.label, entry.number])?;
            }
        }
        transaction.execute("INSERT OR REPLACE INTO meta (key, value) VALUES ('last_sync', ?1)", [synced_at])?;
        transaction.commit()
    }

    /// Replaces the phone's call history in one transaction; SQLite turns the phone's local
    /// "YYYY-MM-DD HH:MM:SS" into unix seconds, and an unparsable time fails the whole replace.
    pub fn replace_calls(&mut self, calls: &[PhoneCall]) -> rusqlite::Result<()> {
        let transaction = self.0.transaction()?;
        transaction.execute("DELETE FROM phone_calls", [])?;
        {
            let mut insert = transaction.prepare(
                "INSERT INTO phone_calls (number, name, kind, started_at)
                 VALUES (?1, ?2, ?3, CAST(strftime('%s', ?4, CASE WHEN ?5 THEN '+0 seconds' ELSE 'utc' END) AS INTEGER))")?;
            for call in calls {
                insert.execute(params![call.number, call.name, call.kind.as_str(), call.started_at, call.utc])?;
            }
        }
        transaction.commit()
    }

    pub fn phone_calls(&self) -> rusqlite::Result<Vec<StoredCall>> {
        let mut query = self.0.prepare("SELECT number, name, kind, started_at FROM phone_calls ORDER BY started_at DESC")?;
        query.query_map([], |row| {
            let kind: String = row.get(2)?;
            Ok(StoredCall { number: row.get(0)?, name: row.get(1)?, kind: Kind::from_str(&kind).unwrap_or(Kind::Received), started_at: row.get(3)? })
        })?.collect()
    }

    pub fn list(&self) -> rusqlite::Result<Vec<Entry>> {
        let mut query = self.0.prepare("SELECT name, label, number FROM contacts ORDER BY name COLLATE NOCASE, label, number")?;
        query.query_map([], |row| Ok(Entry { name: row.get(0)?, label: row.get(1)?, number: row.get(2)? }))?.collect()
    }

    /// Last successful sync as local "YYYY-MM-DD HH:MM" (SQLite does the time-zone work).
    pub fn last_sync_label(&self) -> rusqlite::Result<Option<String>> {
        self.0.query_row(
            "SELECT strftime('%Y-%m-%d %H:%M', value, 'unixepoch', 'localtime') FROM meta WHERE key = 'last_sync'",
            [], |row| row.get(0)).optional()
    }

    pub fn last_sync(&self) -> rusqlite::Result<Option<i64>> {
        self.0.query_row("SELECT value FROM meta WHERE key = 'last_sync'", [], |row| row.get(0)).optional()
    }
}

fn io_error(error: std::io::Error) -> rusqlite::Error {
    rusqlite::Error::ToSqlConversionFailure(Box::new(error))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn entry(name: &str, label: &str, number: &str) -> Entry {
        Entry { name: name.into(), label: label.into(), number: number.into() }
    }

    fn test_path() -> PathBuf {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("iblueconnect-contacts-{}-{stamp}", std::process::id())).join("contacts.sqlite3")
    }

    #[test]
    fn new_store_is_empty_and_private() {
        let path = test_path();
        let store = ContactStore::open(&path).unwrap();
        assert_eq!(store.last_sync().unwrap(), None);
        assert!(store.list().unwrap().is_empty());
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        assert_eq!(fs::metadata(path.parent().unwrap()).unwrap().permissions().mode() & 0o777, 0o700);
    }

    #[test]
    fn forget_clears_contacts_phone_history_and_sync_metadata() {
        let path = test_path();
        let mut store = ContactStore::open(&path).unwrap();
        store.replace_all(&[entry("Test", "Mobile", "1")], 100).unwrap();
        store.replace_calls(&[PhoneCall { number: Some("1".into()), name: None, kind: Kind::Missed,
            started_at: "2020-01-01 00:00:00".into(), utc: true }]).unwrap();
        store.clear().unwrap();
        drop(store);
        let store = ContactStore::open(&path).unwrap();
        assert!(store.list().unwrap().is_empty());
        assert!(store.phone_calls().unwrap().is_empty());
        assert_eq!(store.last_sync().unwrap(), None);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn replace_all_survives_reopen_sorted_by_name_then_label() {
        let path = test_path();
        let mut store = ContactStore::open(&path).unwrap();
        store.replace_all(&[entry("bob", "Mobile", "2"), entry("Alice", "Work", "1"), entry("Alice", "Home", "3")], 100).unwrap();
        let store = ContactStore::open(&path).unwrap();
        assert_eq!(store.last_sync().unwrap(), Some(100));
        assert_eq!(store.list().unwrap(), vec![entry("Alice", "Home", "3"), entry("Alice", "Work", "1"), entry("bob", "Mobile", "2")]);
    }

    #[test]
    fn last_sync_label_is_local_minute_precision() {
        let mut store = ContactStore::open(test_path()).unwrap();
        assert_eq!(store.last_sync_label().unwrap(), None);
        store.replace_all(&[], 1_700_000_000).unwrap();
        let label = store.last_sync_label().unwrap().unwrap();
        assert_eq!(label.len(), "2023-11-14 22:13".len());
        assert!(label.starts_with("2023-11-1") && label.as_bytes()[10] == b' ' && label.as_bytes()[13] == b':');
    }

    fn phone(number: Option<&str>, kind: Kind, at: &str, utc: bool) -> PhoneCall {
        PhoneCall { number: number.map(Into::into), name: None, kind, started_at: at.into(), utc }
    }

    #[test]
    fn phone_calls_store_unix_times_newest_first_and_replace_atomically() {
        let mut store = ContactStore::open(test_path()).unwrap();
        store.replace_calls(&[
            phone(Some("1"), Kind::Missed, "2026-01-01 00:00:01", true),
            phone(None, Kind::Dialed, "2026-01-01 00:00:05", true),
        ]).unwrap();
        let calls = store.phone_calls().unwrap();
        assert_eq!(calls.iter().map(|call| (call.number.clone(), call.kind, call.started_at)).collect::<Vec<_>>(),
                   vec![(None, Kind::Dialed, 1_767_225_605), (Some("1".into()), Kind::Missed, 1_767_225_601)]);

        store.replace_calls(&[phone(Some("2"), Kind::Received, "2026-06-01 12:00:00", false)]).unwrap();
        let local: i64 = store.0.query_row("SELECT CAST(strftime('%s', '2026-06-01 12:00:00', 'utc') AS INTEGER)", [], |row| row.get(0)).unwrap();
        assert_eq!(store.phone_calls().unwrap()[0].started_at, local);

        assert!(store.replace_calls(&[phone(Some("3"), Kind::Missed, "not a time", false)]).is_err());
        assert_eq!(store.phone_calls().unwrap().len(), 1);
    }

    #[test]
    fn failed_replace_keeps_last_good_cache() {
        let mut store = ContactStore::open(test_path()).unwrap();
        store.replace_all(&[entry("Alice", "Mobile", "1")], 100).unwrap();
        assert!(store.replace_all(&[entry("Bob", "Mobile", "2"), entry("Broken", "Mobile", "")], 200).is_err());
        assert_eq!(store.last_sync().unwrap(), Some(100));
        assert_eq!(store.list().unwrap(), vec![entry("Alice", "Mobile", "1")]);
    }
}
