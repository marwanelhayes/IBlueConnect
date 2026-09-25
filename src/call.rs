#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CallPhase {
    Incoming,
    OutgoingDialing,
    OutgoingAlerting,
    Active,
    Ending,
    Terminal(CallOutcome),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CallOutcome {
    Completed,
    Missed,
    Declined,
    Busy,
    Failed,
    Interrupted,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AudioLocation {
    Phone,
    Laptop,
    Switching,
    Unknown,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Call {
    pub id: String,
    pub phase: CallPhase,
    pub audio: AudioLocation,
}

impl Call {
    pub fn new(id: impl Into<String>, phase: CallPhase, audio: AudioLocation) -> Self {
        Self {
            id: id.into(),
            phase,
            audio,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Observation {
    Idle,
    Unavailable,
    Call(Call),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExitAction {
    Quit,
    Disconnect,
    Forget,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExitDecision {
    Proceed,
    RequestPhoneAudio,
    Wait,
    Blocked,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CallAction {
    Answer,
    Decline,
    HangUp,
}

pub fn can_apply(action: CallAction, call_id: &str, observed: &Observation) -> bool {
    let Observation::Call(call) = observed else {
        return false;
    };
    call.id == call_id
        && match action {
            CallAction::Answer | CallAction::Decline => call.phase == CallPhase::Incoming,
            CallAction::HangUp => matches!(
                call.phase,
                CallPhase::OutgoingDialing | CallPhase::OutgoingAlerting | CallPhase::Active
            ),
        }
}

pub struct ExitGuard {
    pub action: ExitAction,
    call_id: Option<String>,
    transfer_requested: bool,
    transfer_failed: bool,
}

impl ExitGuard {
    pub fn new(action: ExitAction, observed: &Observation) -> Self {
        Self {
            action,
            call_id: match observed {
                Observation::Call(call) => Some(call.id.clone()),
                _ => None,
            },
            transfer_requested: false,
            transfer_failed: false,
        }
    }

    pub fn transfer_failed(&mut self) {
        self.transfer_failed = true;
    }

    pub fn evaluate(&mut self, observed: &Observation) -> ExitDecision {
        let call = match observed {
            Observation::Idle => return ExitDecision::Proceed,
            Observation::Call(call) if self.call_id.as_deref() == Some(call.id.as_str()) => call,
            _ => return ExitDecision::Blocked,
        };
        if matches!(call.phase, CallPhase::Terminal(_)) {
            return ExitDecision::Proceed;
        }
        if self.transfer_failed {
            return ExitDecision::Blocked;
        }
        match (call.phase, call.audio) {
            (_, AudioLocation::Phone) => ExitDecision::Proceed,
            (CallPhase::Ending, _) => ExitDecision::Wait,
            (_, AudioLocation::Laptop) if !self.transfer_requested => {
                self.transfer_requested = true;
                ExitDecision::RequestPhoneAudio
            }
            (_, AudioLocation::Laptop | AudioLocation::Switching) if self.transfer_requested => {
                ExitDecision::Wait
            }
            _ => ExitDecision::Blocked,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn active_call_can_keep_phone_audio() {
        let call = Call::new("call-1", CallPhase::Active, AudioLocation::Phone);

        assert_eq!(call.phase, CallPhase::Active);
        assert_eq!(call.audio, AudioLocation::Phone);
    }

    #[test]
    fn quit_can_proceed_when_phone_owns_call_audio() {
        let observed = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Phone));
        let mut exit = ExitGuard::new(ExitAction::Quit, &observed);

        assert_eq!(exit.evaluate(&observed), ExitDecision::Proceed);
    }

    #[test]
    fn disconnect_requests_phone_audio_before_disconnecting() {
        let observed = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let mut exit = ExitGuard::new(ExitAction::Disconnect, &observed);

        assert_eq!(exit.evaluate(&observed), ExitDecision::RequestPhoneAudio);
    }

    #[test]
    fn quit_waits_for_observed_phone_route_after_handoff_request() {
        let laptop = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let switching = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Switching));
        let phone = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Phone));
        let mut exit = ExitGuard::new(ExitAction::Quit, &laptop);

        assert_eq!(exit.evaluate(&laptop), ExitDecision::RequestPhoneAudio);
        assert_eq!(exit.evaluate(&switching), ExitDecision::Wait);
        assert_eq!(exit.evaluate(&phone), ExitDecision::Proceed);
    }

    #[test]
    fn forget_can_proceed_after_authoritative_call_end_during_handoff() {
        let laptop = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let mut exit = ExitGuard::new(ExitAction::Forget, &laptop);

        assert_eq!(exit.evaluate(&laptop), ExitDecision::RequestPhoneAudio);
        assert_eq!(exit.evaluate(&Observation::Idle), ExitDecision::Proceed);
    }

    #[test]
    fn failed_handoff_blocks_quit_and_keeps_call_available() {
        let laptop = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let mut exit = ExitGuard::new(ExitAction::Quit, &laptop);

        assert_eq!(exit.evaluate(&laptop), ExitDecision::RequestPhoneAudio);
        exit.transfer_failed();
        assert_eq!(exit.evaluate(&laptop), ExitDecision::Blocked);
    }

    #[test]
    fn stale_answer_does_not_target_a_call_answered_on_phone() {
        let ringing = Observation::Call(Call::new("call-1", CallPhase::Incoming, AudioLocation::Phone));
        let answered = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Phone));

        assert!(can_apply(CallAction::Answer, "call-1", &ringing));
        assert!(!can_apply(CallAction::Answer, "call-1", &answered));
    }

    #[test]
    fn hang_up_only_targets_the_current_active_call() {
        let active = Observation::Call(Call::new("call-2", CallPhase::Active, AudioLocation::Phone));

        assert!(!can_apply(CallAction::HangUp, "call-1", &active));
        assert!(can_apply(CallAction::HangUp, "call-2", &active));
        assert!(!can_apply(CallAction::HangUp, "call-2", &Observation::Unavailable));
    }

    #[test]
    fn confirmed_call_end_allows_exit_even_after_transfer_error() {
        let laptop = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let mut exit = ExitGuard::new(ExitAction::Quit, &laptop);

        assert_eq!(exit.evaluate(&laptop), ExitDecision::RequestPhoneAudio);
        exit.transfer_failed();
        assert_eq!(exit.evaluate(&Observation::Idle), ExitDecision::Proceed);
    }

    #[test]
    fn terminal_outcome_confirms_end_and_disables_hang_up() {
        let active = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let completed = Observation::Call(Call::new("call-1", CallPhase::Terminal(CallOutcome::Completed), AudioLocation::Unknown));
        let mut exit = ExitGuard::new(ExitAction::Quit, &active);

        assert_eq!(exit.evaluate(&active), ExitDecision::RequestPhoneAudio);
        assert_eq!(exit.evaluate(&completed), ExitDecision::Proceed);
        assert!(!can_apply(CallAction::HangUp, "call-1", &completed));
    }

    #[test]
    fn ending_call_waits_for_confirmed_end_without_starting_handoff() {
        let ending = Observation::Call(Call::new("call-1", CallPhase::Ending, AudioLocation::Laptop));
        let mut exit = ExitGuard::new(ExitAction::Quit, &ending);

        assert_eq!(exit.evaluate(&ending), ExitDecision::Wait);
    }

    #[test]
    fn outgoing_call_can_be_hung_up_until_ending_begins() {
        let dialing = Observation::Call(Call::new("call-1", CallPhase::OutgoingDialing, AudioLocation::Laptop));
        let alerting = Observation::Call(Call::new("call-1", CallPhase::OutgoingAlerting, AudioLocation::Laptop));
        let ending = Observation::Call(Call::new("call-1", CallPhase::Ending, AudioLocation::Laptop));

        assert!(can_apply(CallAction::HangUp, "call-1", &dialing));
        assert!(can_apply(CallAction::HangUp, "call-1", &alerting));
        assert!(!can_apply(CallAction::HangUp, "call-1", &ending));
    }

    #[test]
    fn stale_decline_cannot_reject_a_call_answered_on_phone() {
        let ringing = Observation::Call(Call::new("call-1", CallPhase::Incoming, AudioLocation::Phone));
        let answered = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Phone));

        assert!(can_apply(CallAction::Decline, "call-1", &ringing));
        assert!(!can_apply(CallAction::Decline, "call-1", &answered));
    }

    #[test]
    fn unverified_route_or_lost_visibility_blocks_exit() {
        let laptop = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let unknown = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Unknown));
        let mut exit = ExitGuard::new(ExitAction::Quit, &laptop);

        assert_eq!(exit.evaluate(&laptop), ExitDecision::RequestPhoneAudio);
        assert_eq!(exit.evaluate(&unknown), ExitDecision::Blocked);
        assert_eq!(exit.evaluate(&Observation::Unavailable), ExitDecision::Blocked);
    }

    #[test]
    fn replacement_call_blocks_stale_exit_attempt() {
        let old = Observation::Call(Call::new("call-1", CallPhase::Active, AudioLocation::Laptop));
        let new = Observation::Call(Call::new("call-2", CallPhase::Active, AudioLocation::Phone));
        let mut exit = ExitGuard::new(ExitAction::Disconnect, &old);

        assert_eq!(exit.evaluate(&old), ExitDecision::RequestPhoneAudio);
        assert_eq!(exit.evaluate(&new), ExitDecision::Blocked);
    }
}
