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
        Ok(Self { store, current: None, session_changed: false })
    }

    pub fn recent(&self) -> impl Iterator<Item = &CallRecord> {
        self.store.recent()
    }

    pub fn backend_lost(&mut self, now: SystemTime) {
        self.session_changed = true;
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

    pub fn reconcile(&mut self, observed: &Observation, calls: &[OfonoCall], now: SystemTime) -> io::Result<()> {
        if matches!(observed, Observation::Idle) {
            return self.finish_current(now);
        }
        let Observation::Call(observed_call) = observed else { return Ok(()) };
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
        let mut bytes = [0; 16];
        File::open("/dev/urandom")?.read_exact(&mut bytes)?;
        let id = format!("{:032x}", u128::from_ne_bytes(bytes));
        self.store.observe(ObservedCall {
            id: id.clone(), number: call.number.clone(), name_snapshot: call.name.clone(),
            direction, started_at: now,
        })?;
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
