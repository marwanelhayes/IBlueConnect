use crate::call::{AudioLocation, Call, CallAction, CallPhase, Observation, can_apply};
use crate::ofono::{phase_for_state, valid_number, valid_tone};

pub struct RawCall<'a> {
    // oFono object path identifies a call only within the current service session.
    pub path: &'a str,
    pub state: &'a str,
    pub multiparty: bool,
}

pub enum Command {
    Dial { serial: u64, number: String },
    Answer { serial: u64, path: String },
    HangUp { serial: u64, path: String, action: CallAction },
    SendTone { serial: u64, path: String, tone: char },
}

impl Command {
    pub fn serial(&self) -> u64 {
        match self {
            Self::Dial { serial, .. }
            | Self::Answer { serial, .. }
            | Self::HangUp { serial, .. }
            | Self::SendTone { serial, .. } => *serial,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CommandError {
    NotReady,
    UnsupportedCalls,
    Busy,
    StaleCall,
    InvalidNumber,
    InvalidTone,
}

pub struct Backend {
    observation: Observation,
    ready: bool,
    serial: u64,
    pending: Option<Pending>,
}

enum Pending {
    Dial(u64),
    Action { serial: u64, path: String, phase: CallPhase },
    Tone { serial: u64, path: String },
}

impl Pending {
    fn serial(&self) -> u64 {
        match self {
            Self::Dial(serial) | Self::Action { serial, .. } | Self::Tone { serial, .. } => *serial,
        }
    }
}

impl Backend {
    pub fn new() -> Self {
        Self { observation: Observation::Unavailable, ready: false, serial: 0, pending: None }
    }

    pub fn observation(&self) -> &Observation {
        &self.observation
    }

    pub fn has_pending_command(&self) -> bool { self.pending.is_some() }

    pub fn update_audio(&mut self, audio: AudioLocation) -> bool {
        let Observation::Call(call) = &mut self.observation else { return false };
        if call.audio == audio { return false; }
        call.audio = audio;
        true
    }

    pub fn reconcile(&mut self, ready: bool, calls: &[RawCall<'_>], audio: AudioLocation) {
        self.ready = ready;
        self.observation = if !ready {
            Observation::Unavailable
        } else if calls.is_empty() {
            Observation::Idle
        } else {
            let mut live = calls.iter().filter(|call| call.state != "disconnected");
            let selected = match (live.next(), live.next()) {
                (Some(call), None) => Some(call),
                (None, None) if calls.len() == 1 => calls.first(),
                _ => None,
            };
            selected
                .filter(|call| !call.multiparty)
                .and_then(|call| phase_for_state(call.state).map(|phase| (call, phase)))
                .map(|(call, phase)| Observation::Call(Call::new(call.path, phase, audio)))
                .unwrap_or(Observation::Unavailable)
        };
        if self.pending.as_ref().is_some_and(|pending| match (pending, &self.observation) {
            (Pending::Dial(_), Observation::Idle) => false,
            (Pending::Action { path, phase, .. }, Observation::Call(call))
                if path == &call.id && phase == &call.phase => false,
            (Pending::Tone { path, .. }, Observation::Call(call))
                if path == &call.id && call.phase == CallPhase::Active => false,
            _ => true,
        }) {
            self.pending = None;
        }
    }

    fn gate(&self) -> Result<(), CommandError> {
        if !self.ready {
            Err(CommandError::NotReady)
        } else if matches!(self.observation, Observation::Unavailable) {
            Err(CommandError::UnsupportedCalls)
        } else if self.pending.is_some() {
            Err(CommandError::Busy)
        } else {
            Ok(())
        }
    }

    fn next_serial(&mut self) -> u64 {
        self.serial = self.serial.wrapping_add(1);
        self.serial
    }

    pub fn plan_action(&mut self, action: CallAction, call_id: &str) -> Result<Command, CommandError> {
        self.gate()?;
        if !can_apply(action, call_id, &self.observation) {
            return Err(CommandError::StaleCall);
        }
        let Observation::Call(call) = &self.observation else { unreachable!() };
        let path = call.id.clone();
        let phase = call.phase;
        let serial = self.next_serial();
        self.pending = Some(Pending::Action { serial, path: path.clone(), phase });
        Ok(match action {
            CallAction::Answer => Command::Answer { serial, path },
            CallAction::Decline | CallAction::HangUp => Command::HangUp { serial, path, action },
        })
    }

    pub fn plan_dial(&mut self, number: &str) -> Result<Command, CommandError> {
        self.gate()?;
        if !matches!(self.observation, Observation::Idle) {
            return Err(CommandError::Busy);
        }
        if !valid_number(number) {
            return Err(CommandError::InvalidNumber);
        }
        let serial = self.next_serial();
        self.pending = Some(Pending::Dial(serial));
        Ok(Command::Dial { serial, number: number.to_owned() })
    }

    pub fn plan_tone(&mut self, call_id: &str, tone: char) -> Result<Command, CommandError> {
        self.gate()?;
        if !valid_tone(tone) {
            return Err(CommandError::InvalidTone);
        }
        let Observation::Call(call) = &self.observation else {
            return Err(CommandError::StaleCall);
        };
        if call.id != call_id || call.phase != CallPhase::Active {
            return Err(CommandError::StaleCall);
        }
        let path = call.id.clone();
        let serial = self.next_serial();
        self.pending = Some(Pending::Tone { serial, path: path.clone() });
        Ok(Command::SendTone { serial, path, tone })
    }

    pub fn command_result(&mut self, command: &Command, succeeded: bool) {
        if self.pending.as_ref().is_some_and(|pending| pending.serial() == command.serial()
            && (!succeeded || matches!(pending, Pending::Tone { .. }))) {
            self.pending = None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw<'a>(path: &'a str, state: &'a str) -> RawCall<'a> {
        RawCall { path, state, multiparty: false }
    }

    #[test]
    fn reconciles_one_call_and_keeps_audio_location_independent() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/modem0/voicecall01", "incoming")], AudioLocation::Phone);
        assert_eq!(backend.observation(), &Observation::Call(Call::new("/modem0/voicecall01", CallPhase::Incoming, AudioLocation::Phone)));
        backend.reconcile(true, &[raw("/modem0/voicecall01", "active")], AudioLocation::Laptop);
        assert_eq!(backend.observation(), &Observation::Call(Call::new("/modem0/voicecall01", CallPhase::Active, AudioLocation::Laptop)));
    }

    #[test]
    fn route_changes_update_an_existing_call_without_changing_its_identity_or_phase() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "active")], AudioLocation::Unknown);
        assert!(backend.update_audio(AudioLocation::Laptop));
        assert_eq!(backend.observation(), &Observation::Call(Call::new("/call1", CallPhase::Active, AudioLocation::Laptop)));
        assert!(backend.update_audio(AudioLocation::Unknown));
        assert!(!backend.update_audio(AudioLocation::Unknown));
        backend.reconcile(true, &[], AudioLocation::Unknown);
        assert!(!backend.update_audio(AudioLocation::Laptop));
    }

    #[test]
    fn unsupported_call_states_and_multiple_calls_block_commands() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "active"), raw("/call2", "waiting")], AudioLocation::Phone);
        assert!(matches!(backend.observation(), Observation::Unavailable));
        assert_eq!(backend.plan_action(CallAction::HangUp, "/call1").err(), Some(CommandError::UnsupportedCalls));
        backend.reconcile(true, &[raw("/call1", "held")], AudioLocation::Phone);
        assert_eq!(backend.plan_dial("123").err(), Some(CommandError::UnsupportedCalls));
        backend.reconcile(true, &[RawCall { path: "/call1", state: "active", multiparty: true }], AudioLocation::Phone);
        assert_eq!(backend.plan_action(CallAction::HangUp, "/call1").err(), Some(CommandError::UnsupportedCalls));
    }

    #[test]
    fn stale_actions_and_repeated_commands_are_rejected_until_state_changes() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "incoming")], AudioLocation::Phone);
        let answer = backend.plan_action(CallAction::Answer, "/call1").unwrap();
        assert!(matches!(answer, Command::Answer { .. }));
        assert_eq!(backend.plan_action(CallAction::Answer, "/call1").err(), Some(CommandError::Busy));
        backend.command_result(&answer, true);
        assert_eq!(backend.plan_action(CallAction::Answer, "/call1").err(), Some(CommandError::Busy));
        backend.reconcile(true, &[raw("/call1", "active")], AudioLocation::Phone);
        assert_eq!(backend.plan_action(CallAction::Answer, "/call1").err(), Some(CommandError::StaleCall));
        assert_eq!(backend.plan_action(CallAction::HangUp, "/call0").err(), Some(CommandError::StaleCall));
        assert!(matches!(backend.plan_action(CallAction::HangUp, "/call1"), Ok(Command::HangUp { .. })));
    }

    #[test]
    fn dialing_is_single_shot_and_requires_idle_ready_state() {
        let mut backend = Backend::new();
        assert_eq!(backend.plan_dial("123").err(), Some(CommandError::NotReady));
        backend.reconcile(true, &[], AudioLocation::Unknown);
        assert_eq!(backend.plan_dial("+12 3").err(), Some(CommandError::InvalidNumber));
        let dial = backend.plan_dial("+12*3#").unwrap();
        assert!(matches!(dial, Command::Dial { .. }));
        backend.command_result(&dial, true);
        assert_eq!(backend.plan_dial("123").err(), Some(CommandError::Busy));
        backend.reconcile(true, &[raw("/call1", "dialing")], AudioLocation::Unknown);
        assert_eq!(backend.plan_dial("123").err(), Some(CommandError::Busy));
        backend.reconcile(true, &[], AudioLocation::Unknown);
        assert!(matches!(backend.plan_dial("123"), Ok(Command::Dial { .. })));
    }

    #[test]
    fn tone_requires_current_active_call_and_valid_key() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "active")], AudioLocation::Phone);
        assert_eq!(backend.plan_tone("/call1", 'x').err(), Some(CommandError::InvalidTone));
        assert_eq!(backend.plan_tone("/call0", '2').err(), Some(CommandError::StaleCall));
        let tone = backend.plan_tone("/call1", '#').unwrap();
        assert!(matches!(tone, Command::SendTone { tone: '#', .. }));
        assert_eq!(backend.plan_tone("/call1", '3').err(), Some(CommandError::Busy));
        backend.command_result(&tone, true);
        assert!(matches!(backend.plan_tone("/call1", '3'), Ok(Command::SendTone { tone: '3', .. })));
    }

    #[test]
    fn disconnect_clears_pending_without_claiming_call_ended() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "incoming")], AudioLocation::Phone);
        backend.plan_action(CallAction::Answer, "/call1").unwrap();
        backend.reconcile(false, &[], AudioLocation::Unknown);
        assert!(matches!(backend.observation(), Observation::Unavailable));
        assert_eq!(backend.plan_dial("123").err(), Some(CommandError::NotReady));
    }

    #[test]
    fn disconnected_call_is_ending_until_removed() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "disconnected")], AudioLocation::Phone);
        assert_eq!(backend.observation(), &Observation::Call(Call::new("/call1", CallPhase::Ending, AudioLocation::Phone)));
        assert_eq!(backend.plan_action(CallAction::HangUp, "/call1").err(), Some(CommandError::StaleCall));
        backend.reconcile(true, &[], AudioLocation::Unknown);
        assert_eq!(backend.observation(), &Observation::Idle);
    }

    #[test]
    fn failed_command_can_retry_and_old_result_cannot_clear_new_pending() {
        let mut backend = Backend::new();
        backend.reconcile(true, &[raw("/call1", "incoming")], AudioLocation::Phone);
        let first = backend.plan_action(CallAction::Answer, "/call1").unwrap();
        backend.command_result(&first, false);
        let second = backend.plan_action(CallAction::Answer, "/call1").unwrap();
        backend.command_result(&first, false);
        assert_eq!(backend.plan_action(CallAction::Answer, "/call1").err(), Some(CommandError::Busy));
        backend.command_result(&second, false);
        assert!(backend.plan_action(CallAction::Answer, "/call1").is_ok());
    }
}
