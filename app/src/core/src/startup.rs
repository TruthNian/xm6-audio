//! Resume the saved profile inside the resident core. The existing sampler
//! drives this policy; it adds neither a process nor a timer. After a first
//! successful connection, the service owns reconnection as before.

use std::time::Duration;

use ipc::StreamState;

pub(crate) struct StartupResume {
    profile: Option<String>,
    attempts: usize,
    next: Duration,
}

impl StartupResume {
    pub fn new(profile: Option<String>) -> Self {
        Self { profile, attempts: 0, next: Duration::ZERO }
    }

    pub fn poll(
        &mut self,
        now: Duration,
        stream: StreamState,
        active: Option<&str>,
        exists: bool,
        enabled: bool,
    ) -> Option<String> {
        let name = self.profile.as_deref()?;
        if !enabled || !exists || active != Some(name) {
            self.profile = None;
            return None;
        }
        match stream {
            // A successful stream hands over to the existing reconnection
            // lifecycle. Idle after an attempt means the user disconnected.
            StreamState::Streaming | StreamState::Reconnecting => {
                self.profile = None;
                return None;
            }
            StreamState::Idle if self.attempts > 0 => {
                self.profile = None;
                return None;
            }
            StreamState::Connecting => return None,
            _ => {}
        }
        if now < self.next {
            return None;
        }
        let name = name.to_owned();
        let seconds = [2, 5, 10, 30][self.attempts.min(3)];
        self.attempts = self.attempts.saturating_add(1);
        self.next = now.saturating_add(Duration::from_secs(seconds));
        Some(name)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn poll(p: &mut StartupResume, seconds: u64, stream: StreamState) -> Option<String> {
        p.poll(Duration::from_secs(seconds), stream, Some("headphones"), true, true)
    }

    #[test]
    fn startup_retries_a_device_that_is_not_ready_without_overlapping_connections() {
        let mut p = StartupResume::new(Some("headphones".into()));
        assert_eq!(poll(&mut p, 0, StreamState::Idle).as_deref(), Some("headphones"));
        assert_eq!(poll(&mut p, 1, StreamState::Error), None);
        assert_eq!(poll(&mut p, 2, StreamState::Error).as_deref(), Some("headphones"));
        assert_eq!(poll(&mut p, 30, StreamState::Connecting), None);
        assert_eq!(poll(&mut p, 31, StreamState::Error).as_deref(), Some("headphones"));
    }

    #[test]
    fn startup_respects_disconnect_and_hands_success_to_the_existing_service() {
        for final_state in [StreamState::Idle, StreamState::Streaming, StreamState::Reconnecting] {
            let mut p = StartupResume::new(Some("headphones".into()));
            assert!(poll(&mut p, 0, StreamState::Idle).is_some());
            assert_eq!(poll(&mut p, 2, final_state), None);
            assert_eq!(poll(&mut p, 100, StreamState::Error), None);
        }
    }

    #[test]
    fn startup_does_not_resurrect_a_removed_or_disabled_profile() {
        for (active, exists, enabled) in [(None, true, true), (Some("headphones"), false, true), (Some("headphones"), true, false)] {
            let mut p = StartupResume::new(Some("headphones".into()));
            assert_eq!(p.poll(Duration::ZERO, StreamState::Idle, active, exists, enabled), None);
            assert_eq!(poll(&mut p, 100, StreamState::Error), None);
        }
    }
}
