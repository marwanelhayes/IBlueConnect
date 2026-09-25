# Agent instructions

Before working, read `HANDOFF.md` and `IBlueConnect_Spec.md`. Read `Feasibility.md` for the hardware findings.

Continue the Rust daemon and C/GTK UI with test-first changes. Use subagents for independent code work. Do not change Bluetooth/audio services, place calls, or run other risky commands without the user's approval. oFono is the permanent, user-approved HFP backend (`packaging/ofono-enable.sh`, undo with `packaging/ofono-rollback.sh`); keep the rollback working. Restart the installed daemon only when no call is active.

Never make the app initiate the laptop-to-iPhone Bluetooth link; the user connects the phone manually. Syncing contacts and call history over an already-connected phone is allowed. FaceTime Audio is not possible over this link; do not attempt it without a new user decision.

The app listens silently in the background. Incoming calls show only a popup with Answer, Decline, and Ignore plus one permitted alert sound; Ignore only hides the popup. After answering, the popup becomes a normal closable window that stays findable when minimized and closes when the call ends. Synced missed calls never raise a popup, sound, or notification. The main window (Recents, Keypad, Contacts) opens only from the app icon. Do not auto-answer calls.

Never print phone numbers, contact names, call times, or Bluetooth addresses in tool output; summarize real data as counts. The user handles Git.
