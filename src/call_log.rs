use std::collections::HashMap;
use std::time::UNIX_EPOCH;

use crate::history::{CallRecord, Direction, Outcome};
use crate::vcard::{Entry, decode, structured_name, tel_number, unfold};

const MAX_RECENTS: usize = 500;
const SAME_CALL_SECONDS: i64 = 120;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Kind {
    Missed,
    Received,
    Dialed,
}

impl Kind {
    pub fn from_str(kind: &str) -> Option<Self> {
        [Kind::Missed, Kind::Received, Kind::Dialed].into_iter().find(|known| known.as_str() == kind)
    }

    pub fn as_str(self) -> &'static str {
        match self { Kind::Missed => "missed", Kind::Received => "received", Kind::Dialed => "dialed" }
    }
}

/// One call from the phone's PBAP call history. `started_at` is "YYYY-MM-DD HH:MM:SS",
/// local time unless `utc`; SQLite converts it to unix seconds (see contacts_store).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PhoneCall {
    pub number: Option<String>,
    pub name: Option<String>,
    pub kind: Kind,
    pub started_at: String,
    pub utc: bool,
}

/// A phone call-history entry as cached, with `started_at` in unix seconds.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StoredCall {
    pub number: Option<String>,
    pub name: Option<String>,
    pub kind: Kind,
    pub started_at: i64,
}

/// Parses PBAP call-history vCards (`cch`/`mch`/`ich`/`och`); cards without a valid
/// `X-IRMC-CALL-DATETIME` are skipped.
pub fn parse(text: &str) -> Vec<PhoneCall> {
    let mut calls = Vec::new();
    let (mut name, mut structured, mut number) = (String::new(), String::new(), String::new());
    let mut stamp: Option<(Kind, String, bool)> = None;
    for line in unfold(text) {
        let Some((head, value)) = line.split_once(':') else { continue };
        let mut params = head.split(';');
        let property = params.next().unwrap_or("");
        let property = property.rsplit('.').next().unwrap_or(property).to_ascii_uppercase();
        let params: Vec<String> = params.map(str::to_ascii_uppercase).collect();
        match property.as_str() {
            "BEGIN" => { name.clear(); structured.clear(); number.clear(); stamp = None; }
            "FN" => name = decode(value, &params).trim().to_owned(),
            "N" => structured = structured_name(&decode(value, &params)),
            "TEL" if number.is_empty() => number = tel_number(value),
            "X-IRMC-CALL-DATETIME" => stamp = call_stamp(&params, value.trim()),
            "END" => {
                if let Some((kind, started_at, utc)) = stamp.take() {
                    let name = [&name, &structured].into_iter().find(|name| !name.is_empty()).cloned();
                    let number = Some(number.clone()).filter(|number| !number.is_empty());
                    calls.push(PhoneCall { number, name, kind, started_at, utc });
                }
            }
            _ => {}
        }
    }
    calls
}

fn call_stamp(params: &[String], value: &str) -> Option<(Kind, String, bool)> {
    let kind = params.iter().map(|param| param.strip_prefix("TYPE=").unwrap_or(param)).find_map(|kind| match kind {
        "MISSED" => Some(Kind::Missed),
        "RECEIVED" => Some(Kind::Received),
        "DIALED" => Some(Kind::Dialed),
        _ => None,
    })?;
    let (digits, utc) = match value.strip_suffix('Z') { Some(rest) => (rest, true), None => (value, false) };
    let b = digits.as_bytes();
    let valid = b.len() == 15 && b[8] == b'T' && b.iter().enumerate().all(|(i, c)| i == 8 || c.is_ascii_digit());
    valid.then(|| (kind, format!("{}-{}-{} {}:{}:{}", &digits[0..4], &digits[4..6], &digits[6..8],
                                 &digits[9..11], &digits[11..13], &digits[13..15]), utc))
}

/// One row of the Recent list: name ("" if unknown), number ("" if withheld), display kind, unix start.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Recent {
    pub name: String,
    pub number: String,
    pub kind: &'static str,
    pub started_at: i64,
}

/// Merges locally observed calls with the phone's call history, newest first. A phone entry for the
/// same number and direction within two minutes of a local record is the same call; the local record
/// (accurate outcome) wins.
pub fn merge(local: &[CallRecord], phone: &[StoredCall], contacts: &[Entry]) -> Vec<Recent> {
    let mut by_number: HashMap<String, Option<&str>> = HashMap::new();
    for entry in contacts {
        by_number.entry(number_key(&entry.number))
            .and_modify(|name| if *name != Some(entry.name.as_str()) { *name = None })
            .or_insert(Some(&entry.name));
    }
    let contact_name = |number: Option<&str>| number.and_then(|number| by_number.get(&number_key(number)).copied().flatten());
    let unix = |record: &CallRecord| record.started_at.duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let mut recents: Vec<Recent> = local.iter().map(|record| Recent {
        name: record.name_snapshot.as_deref().or(contact_name(record.number.as_deref())).unwrap_or("").to_owned(),
        number: record.number.clone().unwrap_or_default(),
        kind: local_kind(record),
        started_at: unix(record),
    }).collect();
    for call in phone {
        let duplicate = local.iter().any(|record| record.number.as_deref().map(number_key) == call.number.as_deref().map(number_key)
            && same_direction(record.direction, call.kind)
            && (unix(record) - call.started_at).abs() <= SAME_CALL_SECONDS);
        if duplicate { continue; }
        recents.push(Recent {
            name: call.name.as_deref().or(contact_name(call.number.as_deref())).unwrap_or("").to_owned(),
            number: call.number.clone().unwrap_or_default(),
            kind: call.kind.as_str(),
            started_at: call.started_at,
        });
    }
    recents.sort_by(|a, b| b.started_at.cmp(&a.started_at));
    recents.truncate(MAX_RECENTS);
    recents
}

/// Digits only, keeping the last nine, so "+20 100…" and "0100…" forms of one number compare equal.
// ponytail: last-nine-digit match; a country-aware normalizer would be needed for short or foreign numbers.
fn number_key(number: &str) -> String {
    let digits: String = number.chars().filter(char::is_ascii_digit).collect();
    digits[digits.len().saturating_sub(9)..].to_owned()
}

/// Returns a name only when every contact matching this number agrees on it.
pub fn contact_name<'a>(number: &str, contacts: &'a [Entry]) -> Option<&'a str> {
    let key = number_key(number);
    if key.is_empty() { return None; }
    let mut matches = contacts.iter().filter(|entry| number_key(&entry.number) == key);
    let name = matches.next()?.name.as_str();
    matches.all(|entry| entry.name == name).then_some(name)
}

fn same_direction(direction: Direction, kind: Kind) -> bool {
    match direction {
        Direction::Incoming => kind != Kind::Dialed,
        Direction::Outgoing => kind == Kind::Dialed,
        Direction::Unknown => true,
    }
}

fn local_kind(record: &CallRecord) -> &'static str {
    match (record.outcome, record.direction) {
        (Outcome::Missed, _) => "missed",
        (Outcome::Declined, _) => "declined",
        (Outcome::Busy | Outcome::Failed, _) => "failed",
        (_, Direction::Incoming) => "received",
        (_, Direction::Outgoing) => "dialed",
        (_, Direction::Unknown) => "unknown",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn call(number: Option<&str>, name: Option<&str>, kind: Kind, at: &str, utc: bool) -> PhoneCall {
        PhoneCall { number: number.map(Into::into), name: name.map(Into::into), kind, started_at: at.into(), utc }
    }

    use std::time::Duration;

    fn local(number: Option<&str>, name: Option<&str>, direction: Direction, outcome: Outcome, at: u64) -> CallRecord {
        CallRecord {
            id: format!("local-{at}"), number: number.map(Into::into), name_snapshot: name.map(Into::into), direction,
            started_at: UNIX_EPOCH + Duration::from_secs(at), ended_at: None, active_duration: Duration::ZERO, outcome,
        }
    }

    fn stored(number: Option<&str>, name: Option<&str>, kind: Kind, at: i64) -> StoredCall {
        StoredCall { number: number.map(Into::into), name: name.map(Into::into), kind, started_at: at }
    }

    fn contact(name: &str, number: &str) -> Entry {
        Entry { name: name.into(), label: "Mobile".into(), number: number.into() }
    }

    fn shown(recents: &[Recent]) -> Vec<(&str, &str, &str, i64)> {
        recents.iter().map(|recent| (recent.name.as_str(), recent.number.as_str(), recent.kind, recent.started_at)).collect()
    }

    #[test]
    fn local_record_wins_over_matching_phone_entry_within_two_minutes() {
        let merged = merge(
            &[local(Some("1"), None, Direction::Incoming, Outcome::Declined, 1000)],
            &[stored(Some("1"), None, Kind::Missed, 1100), stored(Some("1"), None, Kind::Missed, 1200),
              stored(Some("1"), None, Kind::Dialed, 1010), stored(None, None, Kind::Missed, 900)],
            &[]);
        assert_eq!(shown(&merged), vec![
            ("", "1", "missed", 1200), ("", "1", "dialed", 1010), ("", "1", "declined", 1000), ("", "", "missed", 900),
        ]);
    }

    #[test]
    fn international_and_national_forms_of_one_number_are_the_same_call_and_contact() {
        let merged = merge(
            &[local(Some("+201001234567"), None, Direction::Incoming, Outcome::Completed, 1000)],
            &[stored(Some("01001234567"), None, Kind::Received, 1050), stored(Some("01009999999"), None, Kind::Received, 900)],
            &[contact("Alice", "+20 100 123 4567")]);
        assert_eq!(shown(&merged), vec![
            ("Alice", "+201001234567", "received", 1000), ("", "01009999999", "received", 900),
        ]);
    }

    #[test]
    fn local_outcomes_map_to_display_kinds() {
        let merged = merge(&[
            local(Some("1"), None, Direction::Incoming, Outcome::Completed, 10),
            local(Some("2"), None, Direction::Outgoing, Outcome::Completed, 9),
            local(Some("3"), None, Direction::Incoming, Outcome::Missed, 8),
            local(Some("4"), None, Direction::Outgoing, Outcome::Busy, 7),
            local(Some("5"), None, Direction::Incoming, Outcome::Interrupted, 6),
            local(Some("6"), None, Direction::Unknown, Outcome::Unknown, 5),
        ], &[], &[]);
        assert_eq!(merged.iter().map(|recent| recent.kind).collect::<Vec<_>>(),
                   ["received", "dialed", "missed", "failed", "received", "unknown"]);
    }

    #[test]
    fn names_come_from_local_then_phone_then_unique_contact() {
        let merged = merge(
            &[local(Some("1"), Some("Snap"), Direction::Incoming, Outcome::Completed, 50)],
            &[stored(Some("2"), Some("Card"), Kind::Received, 40), stored(Some("3"), None, Kind::Received, 30),
              stored(Some("4"), None, Kind::Received, 20)],
            &[contact("Alice", "3"), contact("Bob", "4"), contact("Bobby", "4"), contact("Other", "1")]);
        assert_eq!(merged.iter().map(|recent| recent.name.as_str()).collect::<Vec<_>>(), ["Snap", "Card", "Alice", ""]);
    }

    #[test]
    fn merged_list_is_capped_at_five_hundred() {
        let phone: Vec<_> = (0..600).map(|at| stored(Some("1"), None, Kind::Missed, at * 1000)).collect();
        let merged = merge(&[], &phone, &[]);
        assert_eq!(merged.len(), 500);
        assert_eq!(merged[0].started_at, 599_000);
    }

    #[test]
    fn parses_ios_call_history_cards_and_skips_undated_ones() {
        let text = concat!(
            "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice\r\nTEL;TYPE=CELL:+20 100 1\r\nX-IRMC-CALL-DATETIME;MISSED:20260924T153000\r\nEND:VCARD\r\n",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:\r\nN:;;;;\r\nTEL:0102\r\nX-IRMC-CALL-DATETIME;TYPE=DIALED:20260101T000001Z\r\nEND:VCARD\r\n",
            "BEGIN:VCARD\r\nVERSION:2.1\r\nN:Smith;Bob\r\nX-IRMC-CALL-DATETIME;RECEIVED:20251231T235959\r\nEND:VCARD\r\n",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:No date\r\nTEL:5\r\nEND:VCARD\r\n",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL:6\r\nX-IRMC-CALL-DATETIME;MISSED:garbage\r\nEND:VCARD\r\n",
        );
        assert_eq!(parse(text), vec![
            call(Some("+201001"), Some("Alice"), Kind::Missed, "2026-09-24 15:30:00", false),
            call(Some("0102"), None, Kind::Dialed, "2026-01-01 00:00:01", true),
            call(None, Some("Bob Smith"), Kind::Received, "2025-12-31 23:59:59", false),
        ]);
    }
}
