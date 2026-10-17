use std::fs::File;
use std::io::{self, Read};
use std::path::Path;
use std::time::{Duration, SystemTime};

use crate::call::{CallAction, CallPhase, Observation};
use crate::history::{CallRecord, Direction, ObservedCall, Outcome};
use crate::history_store::HistoryStore;
use crate::ofono::OfonoCall;

pub struct HistoryTracker {
    store: HistoryStore,
    current: Option<Tracked>,
    /// Waiting calls beside the current one, followed by oFono path until they end or become current.
    waiting: Vec<Tracked>,
    session_changed: bool,
}

struct Tracked {
    path: String,
    id: String,
    last_phase: CallPhase,
    active_since: Option<SystemTime>,
    active_duration: Duration,
    finish_as: Outcome,
}

impl HistoryTracker {
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        let mut store = HistoryStore::open(path)?;
        let orphaned: Vec<_> = store.recent()
            .filter(|record| record.ended_at.is_none())
            .map(|record| (record.id.clone(), record.active_duration))
            .collect();
        for (id, duration) in orphaned {
            store.finish(&id, Outcome::Unknown, SystemTime::now(), duration)?;
        }
        Ok(Self { store, current: None, waiting: Vec::new(), session_changed: false })
    }

    pub fn recent(&self) -> impl Iterator<Item = &CallRecord> {
        self.store.recent()
    }

    pub fn backend_lost(&mut self, now: SystemTime) {
        self.session_changed = true;
        for waiting in std::mem::take(&mut self.waiting) {
            let duration = waiting.active_duration
                + waiting.active_since.and_then(|at| now.duration_since(at).ok()).unwrap_or_default();
            if self.store.finish(&waiting.id, Outcome::Unknown, now, duration).is_err() {
                eprintln!("Call history could not be saved");
            }
        }
        if let Some(current) = &mut self.current {
            if let Some(at) = current.active_since.take() {
                current.active_duration += now.duration_since(at).unwrap_or_default();
            }
        }
    }

    pub fn action_succeeded(&mut self, path: &str, action: CallAction) {
        if let Some(current) = &mut self.current {
            if current.path == path {
                current.finish_as = match (action, current.last_phase) {
                    (CallAction::Decline, CallPhase::Incoming) => Outcome::Declined,
                    (CallAction::HangUp, CallPhase::Active) => Outcome::Completed,
                    _ => current.finish_as,
                };
            }
        }
    }

    fn finish_current(&mut self, now: SystemTime) -> io::Result<()> {
        if let Some(current) = &self.current {
            let duration = current.active_duration
                + current.active_since.and_then(|at| now.duration_since(at).ok()).unwrap_or_default();
            self.store.finish(&current.id, current.finish_as, now, duration)?;
            self.current = None;
        }
        Ok(())
    }

    /// Records each waiting call: missed if it ends unanswered, received once it was active.
    fn reconcile_waiting(&mut self, calls: &[OfonoCall], now: SystemTime) -> io::Result<()> {
        for mut waiting in std::mem::take(&mut self.waiting) {
            let Some(call) = calls.iter().find(|call| call.path == waiting.path && call.state != "disconnected") else {
                let duration = waiting.active_duration
                    + waiting.active_since.and_then(|at| now.duration_since(at).ok()).unwrap_or_default();
                self.store.finish(&waiting.id, waiting.finish_as, now, duration)?;
                continue;
            };
            if call.state == "active" {
                waiting.active_since.get_or_insert(now);
                waiting.last_phase = CallPhase::Active;
                waiting.finish_as = Outcome::Unknown;
            } else if let Some(at) = waiting.active_since.take() {
                waiting.active_duration += now.duration_since(at).unwrap_or_default();
            }
            self.waiting.push(waiting);
        }
        for call in calls.iter().filter(|call| call.state == "waiting") {
            if self.waiting.iter().any(|waiting| waiting.path == call.path) { continue; }
            let id = self.observe(call, Direction::Incoming, now)?;
            self.waiting.push(Tracked {
                path: call.path.clone(), id, last_phase: CallPhase::Incoming,
                active_since: None, active_duration: Duration::ZERO, finish_as: Outcome::Missed,
            });
        }
        Ok(())
    }

    fn observe(&mut self, call: &OfonoCall, direction: Direction, now: SystemTime) -> io::Result<String> {
        let mut bytes = [0; 16];
        File::open("/dev/urandom")?.read_exact(&mut bytes)?;
        let id = format!("{:032x}", u128::from_ne_bytes(bytes));
        self.store.observe(ObservedCall {
            id: id.clone(), number: call.number.clone(), name_snapshot: call.name.clone(),
            direction, started_at: now,
        })?;
        Ok(id)
    }

    pub fn reconcile(&mut self, observed: &Observation, calls: &[OfonoCall], now: SystemTime) -> io::Result<()> {
        self.reconcile_waiting(calls, now)?;
        if matches!(observed, Observation::Idle) {
            return self.finish_current(now);
        }
        let Observation::Call(observed_call) = observed else { return Ok(()) };
        // A formerly waiting call that is now the only call continues its own record.
        if !self.session_changed && self.current.as_ref().is_none_or(|current| current.path != observed_call.id) {
            if let Some(index) = self.waiting.iter().position(|waiting| waiting.path == observed_call.id) {
                self.finish_current(now)?;
                self.current = Some(self.waiting.remove(index));
            }
        }
        let reused = self.current.as_ref().is_some_and(|current| {
            matches!(current.last_phase, CallPhase::Active | CallPhase::Ending)
                && matches!(observed_call.phase, CallPhase::Incoming | CallPhase::OutgoingDialing | CallPhase::OutgoingAlerting)
        });
        if !self.session_changed && !reused && self.current.as_ref().is_some_and(|current| current.path == observed_call.id) {
            let current = self.current.as_mut().unwrap();
            if observed_call.phase == CallPhase::Active {
                current.active_since.get_or_insert(now);
            } else if let Some(at) = current.active_since.take() {
                current.active_duration += now.duration_since(at).unwrap_or_default();
            }
            if observed_call.phase == CallPhase::Active { current.finish_as = Outcome::Unknown; }
            current.last_phase = observed_call.phase;
            return Ok(());
        }
        self.finish_current(now)?;
        self.session_changed = false;
        let Some(call) = calls.iter().find(|call| call.path == observed_call.id) else { return Ok(()) };
        let direction = match observed_call.phase {
            CallPhase::Incoming => Direction::Incoming,
            CallPhase::OutgoingDialing | CallPhase::OutgoingAlerting => Direction::Outgoing,
            _ => Direction::Unknown,
        };
        let id = self.observe(call, direction, now)?;
        self.current = Some(Tracked {
            path: call.path.clone(), id, last_phase: observed_call.phase,
            active_since: (observed_call.phase == CallPhase::Active).then_some(now),
            active_duration: Duration::ZERO,
            finish_as: Outcome::Unknown,
        });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::call::{AudioLocation, Call, CallAction, CallPhase, Observation};
    use crate::history::{Direction, Outcome};
    use crate::ofono::OfonoCall;
    use std::fs;
    use std::time::{Duration, SystemTime};

    #[test]
    fn call_already_active_at_startup_is_recorded_without_inventing_direction() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/private-call".into(), state: "active".into(), multiparty: false, number: Some("123".into()), name: None };
        let observed = Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown));
        tracker.reconcile(&observed, &[call], SystemTime::UNIX_EPOCH).unwrap();
        let recent: Vec<_> = tracker.recent().collect();
        assert_eq!(recent.len(), 1);
        assert_eq!(recent[0].direction, Direction::Unknown);
        assert_eq!(recent[0].number.as_deref(), Some("123"));
        assert!(!recent[0].id.contains("modem"));
        drop(tracker);
        assert_eq!(HistoryTracker::open(&path).unwrap().recent().count(), 1);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn disappearance_finishes_a_call_once_with_observed_active_duration() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "active".into(), multiparty: false, number: None, name: None };
        let observed = Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown));
        let start = SystemTime::UNIX_EPOCH;
        tracker.reconcile(&observed, &[call.clone()], start).unwrap();
        tracker.reconcile(&observed, &[call], start + Duration::from_secs(2)).unwrap();
        tracker.reconcile(&Observation::Idle, &[], start + Duration::from_secs(5)).unwrap();
        tracker.reconcile(&Observation::Idle, &[], start + Duration::from_secs(6)).unwrap();
        let records: Vec<_> = tracker.recent().collect();
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].outcome, Outcome::Unknown);
        assert_eq!(records[0].active_duration, Duration::from_secs(5));
        assert_eq!(records[0].ended_at, Some(start + Duration::from_secs(5)));
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn lost_visibility_does_not_claim_end_but_a_replacement_call_finishes_the_old_record() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let old = OfonoCall { path: "/modem/old".into(), state: "active".into(), multiparty: false, number: None, name: None };
        let new = OfonoCall { path: "/modem/new".into(), state: "incoming".into(), multiparty: false, number: None, name: None };
        tracker.reconcile(&Observation::Call(Call::new(&old.path, CallPhase::Active, AudioLocation::Unknown)), &[old], SystemTime::UNIX_EPOCH).unwrap();
        tracker.reconcile(&Observation::Unavailable, &[], SystemTime::UNIX_EPOCH + Duration::from_secs(1)).unwrap();
        assert_eq!(tracker.recent().next().unwrap().ended_at, None);
        tracker.reconcile(&Observation::Call(Call::new(&new.path, CallPhase::Incoming, AudioLocation::Unknown)), &[new], SystemTime::UNIX_EPOCH + Duration::from_secs(3)).unwrap();
        let records: Vec<_> = tracker.recent().collect();
        assert_eq!(records.len(), 2);
        assert_eq!(records[0].direction, Direction::Incoming);
        assert_eq!(records[1].ended_at, Some(SystemTime::UNIX_EPOCH + Duration::from_secs(3)));
        assert_eq!(records[1].outcome, Outcome::Unknown);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn backend_restart_with_reused_path_starts_a_new_observed_call() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "active".into(), multiparty: false, number: None, name: None };
        let observed = Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown));
        tracker.reconcile(&observed, &[call.clone()], SystemTime::UNIX_EPOCH).unwrap();
        tracker.backend_lost(SystemTime::UNIX_EPOCH + Duration::from_secs(1));
        assert_eq!(tracker.recent().next().unwrap().ended_at, None);
        tracker.reconcile(&observed, &[call], SystemTime::UNIX_EPOCH + Duration::from_secs(10)).unwrap();
        let records: Vec<_> = tracker.recent().collect();
        assert_eq!(records.len(), 2);
        assert_ne!(records[0].id, records[1].id);
        assert_eq!(records[1].outcome, Outcome::Unknown);
        assert_eq!(records[1].active_duration, Duration::from_secs(1));
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn reused_path_after_active_call_does_not_merge_a_new_incoming_call() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "active".into(), multiparty: false, number: None, name: None };
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown)), &[call.clone()], SystemTime::UNIX_EPOCH).unwrap();
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Incoming, AudioLocation::Unknown)), &[call], SystemTime::UNIX_EPOCH + Duration::from_secs(2)).unwrap();
        let records: Vec<_> = tracker.recent().collect();
        assert_eq!(records.len(), 2);
        assert_eq!(records[0].direction, Direction::Incoming);
        assert_ne!(records[0].id, records[1].id);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn ending_time_is_not_counted_as_active_duration() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "active".into(), multiparty: false, number: None, name: None };
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown)), &[call.clone()], SystemTime::UNIX_EPOCH).unwrap();
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Ending, AudioLocation::Unknown)), &[call], SystemTime::UNIX_EPOCH + Duration::from_secs(3)).unwrap();
        tracker.reconcile(&Observation::Idle, &[], SystemTime::UNIX_EPOCH + Duration::from_secs(8)).unwrap();
        assert_eq!(tracker.recent().next().unwrap().active_duration, Duration::from_secs(3));
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn daemon_restart_closes_orphaned_open_records_as_unknown() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "active".into(), multiparty: false, number: None, name: None };
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Active, AudioLocation::Unknown)), &[call], SystemTime::UNIX_EPOCH).unwrap();
        drop(tracker);
        let tracker = HistoryTracker::open(&path).unwrap();
        let record = tracker.recent().next().unwrap();
        assert_eq!(record.outcome, Outcome::Unknown);
        assert!(record.ended_at.is_some());
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn waiting_call_is_missed_unless_answered_and_an_answered_one_is_not_duplicated() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = |path: &str, state: &str| OfonoCall { path: path.into(), state: state.into(), multiparty: false, number: None, name: None };
        let at = |seconds| SystemTime::UNIX_EPOCH + Duration::from_secs(seconds);
        let active = |path: &str| Observation::Call(Call::new(path, CallPhase::Active, AudioLocation::Unknown));
        tracker.reconcile(&active("/m/a"), &[call("/m/a", "active")], at(0)).unwrap();
        // A waiting call that goes away unanswered is missed.
        tracker.reconcile(&Observation::Unavailable, &[call("/m/a", "active"), call("/m/b", "waiting")], at(1)).unwrap();
        tracker.reconcile(&active("/m/a"), &[call("/m/a", "active")], at(2)).unwrap();
        // An answered waiting call stays one received record after the first call ends.
        tracker.reconcile(&Observation::Unavailable, &[call("/m/a", "active"), call("/m/c", "waiting")], at(3)).unwrap();
        tracker.reconcile(&Observation::Unavailable, &[call("/m/a", "held"), call("/m/c", "active")], at(4)).unwrap();
        tracker.reconcile(&active("/m/c"), &[call("/m/c", "active")], at(6)).unwrap();
        tracker.reconcile(&Observation::Idle, &[], at(10)).unwrap();
        let records: Vec<_> = tracker.recent().collect();
        assert_eq!(records.len(), 3);
        let (first, missed, received) = (records[2], records[1], records[0]);
        assert_eq!(first.ended_at, Some(at(6)));
        assert_eq!((missed.direction, missed.outcome, missed.ended_at), (Direction::Incoming, Outcome::Missed, Some(at(2))));
        assert_eq!((received.direction, received.outcome, received.ended_at), (Direction::Incoming, Outcome::Unknown, Some(at(10))));
        assert_eq!(received.active_duration, Duration::from_secs(6));
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }

    #[test]
    fn confirmed_laptop_decline_is_recorded_as_declined() {
        let path = std::env::temp_dir().join(format!("iblueconnect-tracker-{}-{}", std::process::id(), SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos())).join("history");
        let mut tracker = HistoryTracker::open(&path).unwrap();
        let call = OfonoCall { path: "/modem/call1".into(), state: "incoming".into(), multiparty: false, number: None, name: None };
        tracker.reconcile(&Observation::Call(Call::new(&call.path, CallPhase::Incoming, AudioLocation::Unknown)), &[call], SystemTime::UNIX_EPOCH).unwrap();
        tracker.action_succeeded("/modem/call1", CallAction::Decline);
        tracker.reconcile(&Observation::Idle, &[], SystemTime::UNIX_EPOCH + Duration::from_secs(1)).unwrap();
        assert_eq!(tracker.recent().next().unwrap().outcome, Outcome::Declined);
        fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }
}
