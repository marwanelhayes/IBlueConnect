# Agent instructions

Before working, read `HANDOFF.md` and `IBlueConnect_Spec.md`. Read `Feasibility.md` for the hardware findings. The 2026-09-25 updates at the top of the handoff and spec supersede their older popup instructions.

Continue the Rust daemon and C/GTK UI with test-first changes. Use subagents for independent code work. Do not change Bluetooth/audio services, place calls, or run other risky commands without the user's approval. oFono is the permanent, user-approved HFP backend (`packaging/ofono-enable.sh`, undo with `packaging/ofono-rollback.sh`); keep the rollback working. Restart the installed daemon only when no call is active.

Before restarting the daemon, check a fresh `Snapshot` and confirm its call phase is `idle`. Capture and parse the reply without printing it: it contains private caller details. A UI-only update does not need a daemon restart.

Never make the app initiate the laptop-to-iPhone Bluetooth link; the user connects the phone manually. Syncing contacts and call history over an already-connected phone is allowed. FaceTime Audio is not possible over this link; do not attempt it without a new user decision.

The app listens silently in the background. Since 2026-09-28 (user decision), incoming calls use the floating GNOME call controller (`iblueconnect --background`, launched by the daemon) with Ignore, Decline, and Answer, plus one permitted alert sound. Ignore only hides the controller and leaves the phone ringing. The controller hides when ringing ends; after Answer, the same window becomes the active-call controller. Synced missed calls never raise a popup, sound, or notification. The main window (Recents, Keypad, Contacts) opens only from the app icon. Do not auto-answer calls.

The installed UI is built from `src/mock-ui/` (`packaging/build-mock-ui.sh`; tests `packaging/test-mock-ui.sh`); it follows the daemon by default, and `--mock`/`--developer`/`--preview=…` use mock data. `src/iblueconnect-ui.c` is the old UI, kept only as a rollback until a live call is verified. The main window uses the same iPhone-to-laptop connection card on all three tabs and follows the system light/dark appearance. A ready link is blue; an unavailable link is red with an X. Keep the full keypad visible at the default window size. The daemon must notify an already-open window when the phone disconnects; its oFono modem-removal watcher and five-second fallback are covered by offline tests, but a live user-operated disconnect has not yet been verified.

Never print phone numbers, contact names, call times, or Bluetooth addresses in tool output; summarize real data as counts. The user handles Git.
