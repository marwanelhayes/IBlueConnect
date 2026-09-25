use std::collections::VecDeque;
use std::time::{Duration, SystemTime};

const MAX_CALLS: usize = 500;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Direction {
    Incoming,
    Outgoing,
    Unknown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Outcome {
    Completed,
    Missed,
    Declined,
    Busy,
    Failed,
    Interrupted,
    Unknown,
}

#[derive(Clone, Eq, PartialEq)]
pub struct ObservedCall {
    pub id: String,
    pub number: Option<String>,
    pub name_snapshot: Option<String>,
    pub direction: Direction,
    pub started_at: SystemTime,
}

#[derive(Clone, Eq, PartialEq)]
pub struct CallRecord {
    pub id: String,
    pub number: Option<String>,
    pub name_snapshot: Option<String>,
    pub direction: Direction,
    pub started_at: SystemTime,
    pub ended_at: Option<SystemTime>,
    pub active_duration: Duration,
    pub outcome: Outcome,
}

impl CallRecord {
    pub fn display_name(&self) -> &str {
        self.name_snapshot
            .as_deref()
            .or(self.number.as_deref())
            .unwrap_or("Unknown caller")
    }
}

#[derive(Clone, Default)]
pub struct History {
    calls: VecDeque<CallRecord>,
}

impl History {
    pub(crate) fn from_records(records: Vec<CallRecord>) -> Self {
        let mut calls = VecDeque::from(records);
        calls.truncate(MAX_CALLS);
        Self { calls }
    }

    pub fn observe(&mut self, call: ObservedCall) -> bool {
        if self.calls.iter().any(|existing| existing.id == call.id) {
            return false;
        }
        self.calls.push_front(CallRecord {
            id: call.id,
            number: call.number,
            name_snapshot: call.name_snapshot,
            direction: call.direction,
            started_at: call.started_at,
            ended_at: None,
            active_duration: Duration::ZERO,
            outcome: Outcome::Unknown,
        });
        self.calls.truncate(MAX_CALLS);
        true
    }

    pub fn finish(
        &mut self,
        id: &str,
        outcome: Outcome,
        ended_at: SystemTime,
        active_duration: Duration,
    ) -> bool {
        let Some(call) = self.calls.iter_mut().find(|call| call.id == id) else {
            return false;
        };
        if call.ended_at.is_some() {
            return false;
        }
        call.ended_at = Some(ended_at);
        call.active_duration = active_duration;
        call.outcome = outcome;
        true
    }

    pub fn recent(&self) -> impl Iterator<Item = &CallRecord> {
        self.calls.iter()
    }

    pub fn clear(&mut self) {
        self.calls.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, SystemTime};

    #[test]
    fn unanswered_call_is_recorded_once_as_missed() {
        let started_at = SystemTime::UNIX_EPOCH;
        let mut history = History::default();
        let call = ObservedCall {
            id: "call-1".into(),
            number: Some("123".into()),
            name_snapshot: None,
            direction: Direction::Incoming,
            started_at,
        };
        assert!(history.observe(call.clone()));
        assert!(!history.observe(call));
        assert_eq!(history.recent().next().unwrap().outcome, Outcome::Unknown);
        let ended_at = started_at + Duration::from_secs(3);
        assert!(history.finish("call-1", Outcome::Missed, ended_at, Duration::ZERO));
        assert!(!history.finish("call-1", Outcome::Missed, ended_at, Duration::ZERO));
        let recent: Vec<_> = history.recent().collect();
        assert_eq!(recent.len(), 1);
        assert_eq!(recent[0].outcome, Outcome::Missed);
    }

    #[test]
    fn decline_is_distinct_from_missed_and_unknown_number_is_not_callable() {
        let started_at = SystemTime::UNIX_EPOCH;
        let mut history = History::default();
        history.observe(ObservedCall {
            id: "private-call".into(),
            number: None,
            name_snapshot: None,
            direction: Direction::Incoming,
            started_at,
        });
        assert!(history.finish(
            "private-call",
            Outcome::Declined,
            started_at,
            Duration::ZERO
        ));
        let call = history.recent().next().unwrap();
        assert_eq!(call.outcome, Outcome::Declined);
        assert_eq!(call.display_name(), "Unknown caller");
        assert_eq!(call.number.as_deref(), None);
    }

    #[test]
    fn recent_calls_are_newest_first_bounded_and_clearable() {
        let mut history = History::default();
        for index in 0..=500 {
            history.observe(ObservedCall {
                id: index.to_string(),
                number: None,
                name_snapshot: None,
                direction: Direction::Outgoing,
                started_at: SystemTime::UNIX_EPOCH + Duration::from_secs(index),
            });
        }
        let recent: Vec<_> = history.recent().collect();
        assert_eq!(recent.len(), 500);
        assert_eq!(recent.first().unwrap().id, "500");
        assert_eq!(recent.last().unwrap().id, "1");
        history.clear();
        assert_eq!(history.recent().count(), 0);
    }
}
