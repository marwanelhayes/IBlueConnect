use std::collections::HashSet;

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Effect {
    ShowIncoming(String, String),
    HideIncoming(String),
    Alert(String, u64),
    PostMissedSilently(String, String, Option<String>),
    RemoveMissed(String),
}

struct Ringing {
    id: String,
    label: String,
    presented: bool,
}

struct Missed {
    id: String,
    label: String,
    number: Option<String>,
    posted: bool,
    dismissed: bool,
}

pub struct PresentationPolicy {
    locked: bool,
    dnd: bool,
    ringing: Option<Ringing>,
    missed: Vec<Missed>,
    seen: HashSet<String>,
    lock_epoch: u64,
}

impl PresentationPolicy {
    pub fn new(locked: bool, dnd: bool) -> Self {
        Self { locked, dnd, ringing: None, missed: Vec::new(), seen: HashSet::new(), lock_epoch: 0 }
    }

    pub fn call_list_cleared(&mut self) { self.seen.clear(); }

    pub fn ring(&mut self, id: &str, label: &str) -> Vec<Effect> {
        if self.ringing.is_some() || !self.seen.insert(id.into()) {
            return vec![];
        }
        self.ringing = Some(Ringing { id: id.into(), label: label.into(), presented: false });
        self.present()
    }

    pub fn set_locked(&mut self, locked: bool) -> Vec<Effect> {
        if self.locked == locked {
            return vec![];
        }
        self.locked = locked;
        if locked {
            self.lock_epoch = self.lock_epoch.wrapping_add(1);
        }
        let mut effects = if locked {
            if self.ringing.as_ref().is_some_and(|ringing| ringing.presented) {
                vec![Effect::HideIncoming(self.ringing.as_ref().unwrap().id.clone())]
            } else {
                vec![]
            }
        } else {
            self.present()
        };
        for missed in &mut self.missed {
            if missed.dismissed || missed.posted != locked {
                continue;
            }
            missed.posted = !locked;
            if locked {
                effects.push(Effect::RemoveMissed(missed.id.clone()));
            } else {
                effects.push(Effect::PostMissedSilently(missed.id.clone(), missed.label.clone(), missed.number.clone()));
            }
        }
        effects
    }

    pub fn set_dnd(&mut self, dnd: bool) {
        self.dnd = dnd;
    }

    pub fn can_answer(&self, id: &str) -> bool {
        !self.locked && self.ringing.as_ref().is_some_and(|ringing| ringing.id == id)
    }

    pub fn end_ringing(&mut self, id: &str) -> Vec<Effect> {
        if !self.ringing.as_ref().is_some_and(|ringing| ringing.id == id) {
            return vec![];
        }
        self.ringing = None;
        if self.locked { vec![] } else { vec![Effect::HideIncoming(id.into())] }
    }

    pub fn missed(&mut self, id: &str, label: &str, number: Option<&str>) -> Vec<Effect> {
        if self.missed.iter().any(|missed| missed.id == id) {
            return vec![];
        }
        self.seen.insert(id.into());
        let mut effects = Vec::new();
        if self.ringing.as_ref().is_some_and(|ringing| ringing.id == id) {
            self.ringing = None;
            if !self.locked {
                effects.push(Effect::HideIncoming(id.into()));
            }
        }
        self.missed.push(Missed {
            id: id.into(), label: label.into(), number: number.map(str::to_owned),
            posted: !self.locked, dismissed: false,
        });
        if !self.locked {
            effects.push(Effect::PostMissedSilently(id.into(), label.into(), number.map(str::to_owned)));
        }
        effects
    }

    pub fn dismiss_missed(&mut self, id: &str) -> Vec<Effect> {
        let Some(missed) = self.missed.iter_mut().find(|missed| missed.id == id && !missed.dismissed) else {
            return vec![];
        };
        missed.dismissed = true;
        if missed.posted {
            missed.posted = false;
            vec![Effect::RemoveMissed(id.into())]
        } else {
            vec![]
        }
    }

    pub fn callback_number(&self, id: &str) -> Option<&str> {
        if self.locked {
            return None;
        }
        self.missed.iter().find(|missed| missed.id == id && missed.posted && !missed.dismissed)
            .and_then(|missed| missed.number.as_deref())
    }

    pub fn effect_is_current(&self, effect: &Effect) -> bool {
        match effect {
            Effect::ShowIncoming(id, label) => !self.locked && self.ringing.as_ref()
                .is_some_and(|ringing| ringing.id == *id && ringing.label == *label),
            Effect::HideIncoming(_) => self.locked || self.ringing.is_none(),
            Effect::Alert(id, epoch) => !self.locked && !self.dnd && *epoch == self.lock_epoch && self.ringing.as_ref()
                .is_some_and(|ringing| ringing.id == *id),
            Effect::PostMissedSilently(id, label, number) => !self.locked && self.missed.iter()
                .any(|missed| missed.id == *id && missed.label == *label && missed.number == *number
                    && missed.posted && !missed.dismissed),
            Effect::RemoveMissed(id) => self.missed.iter()
                .any(|missed| missed.id == *id && !missed.posted),
        }
    }

    fn present(&mut self) -> Vec<Effect> {
        if self.locked {
            return vec![];
        }
        let Some(ringing) = self.ringing.as_mut() else { return vec![] };
        let first = !ringing.presented;
        ringing.presented = true;
        let mut effects = vec![Effect::ShowIncoming(ringing.id.clone(), ringing.label.clone())];
        if first && !self.dnd {
            effects.push(Effect::Alert(ringing.id.clone(), self.lock_epoch));
        }
        effects
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn locked_ringing_is_hidden_until_unlock_and_alerts_once() {
        let mut policy = PresentationPolicy::new(true, false);
        assert!(policy.ring("call-1", "Alice").is_empty());
        assert_eq!(
            policy.set_locked(false),
            vec![Effect::ShowIncoming("call-1".into(), "Alice".into()), Effect::Alert("call-1".into(), 0)]
        );
        assert_eq!(policy.set_locked(true), vec![Effect::HideIncoming("call-1".into())]);
        assert_eq!(
            policy.set_locked(false),
            vec![Effect::ShowIncoming("call-1".into(), "Alice".into())]
        );
    }

    #[test]
    fn dnd_keeps_popup_but_suppresses_its_only_alert() {
        let mut policy = PresentationPolicy::new(false, true);
        assert_eq!(
            policy.ring("call-1", "Alice"),
            vec![Effect::ShowIncoming("call-1".into(), "Alice".into())]
        );
        assert!(policy.ring("call-1", "Alice").is_empty());
        policy.set_locked(true);
        policy.set_dnd(false);
        assert_eq!(
            policy.set_locked(false),
            vec![Effect::ShowIncoming("call-1".into(), "Alice".into())]
        );
    }

    #[test]
    fn missed_while_locked_restores_silently_until_user_dismisses() {
        let mut policy = PresentationPolicy::new(true, false);
        assert!(policy.ring("call-1", "Alice").is_empty());
        assert!(policy.missed("call-1", "Alice", Some("123")).is_empty());
        assert_eq!(
            policy.set_locked(false),
            vec![Effect::PostMissedSilently("call-1".into(), "Alice".into(), Some("123".into()))]
        );
        assert_eq!(policy.set_locked(true), vec![Effect::RemoveMissed("call-1".into())]);
        assert_eq!(
            policy.set_locked(false),
            vec![Effect::PostMissedSilently("call-1".into(), "Alice".into(), Some("123".into()))]
        );
        assert_eq!(policy.dismiss_missed("call-1"), vec![Effect::RemoveMissed("call-1".into())]);
        policy.set_locked(true);
        assert!(policy.set_locked(false).is_empty());
        assert!(policy.missed("call-1", "Alice", Some("123")).is_empty());
    }

    #[test]
    fn answered_or_declined_ringing_cannot_reappear_or_answer_while_locked() {
        let mut policy = PresentationPolicy::new(false, false);
        policy.ring("call-1", "Alice");
        assert!(policy.can_answer("call-1"));
        assert_eq!(policy.end_ringing("call-1"), vec![Effect::HideIncoming("call-1".into())]);
        assert!(!policy.can_answer("call-1"));
        policy.set_locked(true);
        assert!(policy.set_locked(false).is_empty());
        assert!(policy.end_ringing("call-1").is_empty());
        assert!(policy.ring("call-1", "Alice").is_empty());

        policy.ring("call-2", "Bob");
        policy.set_locked(true);
        assert!(!policy.can_answer("call-2"));
        assert!(!policy.can_answer("call-1"));
    }

    #[test]
    fn callback_requires_visible_undismissed_notification_with_number() {
        let mut policy = PresentationPolicy::new(false, false);
        assert_eq!(
            policy.missed("private", "Unknown caller", None),
            vec![Effect::PostMissedSilently("private".into(), "Unknown caller".into(), None)]
        );
        assert_eq!(policy.callback_number("private"), None);
        policy.missed("known", "Alice", Some("123"));
        assert_eq!(policy.callback_number("known"), Some("123"));
        policy.set_locked(true);
        assert_eq!(policy.callback_number("known"), None);
        policy.set_locked(false);
        assert_eq!(policy.callback_number("known"), Some("123"));
        policy.dismiss_missed("known");
        assert_eq!(policy.callback_number("known"), None);
        assert!(policy.missed("known", "Alice", Some("123")).is_empty());
    }

    #[test]
    fn queued_identity_effects_must_be_rechecked_at_execution() {
        let mut policy = PresentationPolicy::new(false, false);
        let incoming = policy.ring("call-1", "Alice");
        assert!(policy.effect_is_current(&incoming[0]));
        policy.set_locked(true);
        assert!(!policy.effect_is_current(&incoming[0]));
        assert!(!policy.effect_is_current(&incoming[1]));
        policy.set_locked(false);
        assert!(!policy.effect_is_current(&incoming[1]));
        policy.set_locked(true);

        policy.missed("call-1", "Alice", Some("123"));
        let restored = policy.set_locked(false);
        assert!(policy.effect_is_current(&restored[0]));
        policy.dismiss_missed("call-1");
        assert!(!policy.effect_is_current(&restored[0]));
    }
}
