# IBlueConnect handoff — calling GUI, oFono, keypad, contacts, recents

Date: 2026-09-24. Workspace: `/home/marwan-ahmed/Work/IBlueConnect`. Supersedes the older /tmp handoffs.

## 2026-09-25 update

The user replaced the incoming GTK popup with a native GNOME notification-center entry carrying Answer, Decline, and Ignore. Ignore removes only the entry. The daemon now posts and closes that notification directly; the older popup details below are historical. Caller names resolve from the synced contact cache, the main UI shows approximate iPhone battery percentage from oFono's 0–5 level, and Contacts rows load when the Contacts tab opens. Charging state is unavailable from the observed phone interfaces. Rust and C offline checks pass; incoming-call behavior still needs a user-coordinated live test.

The user chose the native connected-device main-window layout after comparing three visual options. It draws an iPhone-to-laptop link that turns blue only when calling is ready and red with an X when unavailable, centers empty Recents, and follows libadwaita's live system light/dark preference. The phone illustration keeps its proportions in tall windows, the connection card is capped at 600 px in wide windows, and the same compact illustrated card appears on Recents, Keypad, and Contacts while the full keypad fits in the default window. The unused comparison files were removed.

The daemon now observes oFono's `ModemRemoved` event for its tracked modem and checks the connected backend every five seconds as a fallback. This lets an already-open main window receive a `Changed` signal after a manual Bluetooth disconnect. A private D-Bus regression and the offline Rust suite pass; no live link was disconnected for verification. The new daemon was installed and restarted only after Snapshot reported an idle call.

## Read first

- [AGENTS.md](AGENTS.md), [IBlueConnect_Spec.md](IBlueConnect_Spec.md) (product contract; §9 allows a JSON contacts cache as an alternative, SQLite is current; §9 "Next step: sync missed calls and recents" and §14 step 8 are now implemented), [Feasibility.md](Feasibility.md) (hardware findings, including the permanent oFono switch and PBAP contact/call-history results).
- Project is untracked in Git; user handles Git — never run git.

## Hard user rules

- **Never initiate the Bluetooth link** laptop↔iPhone (no BlueZ Connect/Pair/Powered, no oFono Powered/Online writes). The user connects manually; overrides spec "Connect automatically". PBAP/OBEX sessions over an already-connected phone (contacts + call history) are requested and allowed.
- No auto-answer. Main window opens only from the app icon. Incoming call → popup only (+ one alert sound, unless DND/locked).
- Popup has **Answer / Decline / Ignore**. Ignore only hides the popup (call keeps ringing on the iPhone); the hidden popup process lives until ringing ends so an unlock re-show stays hidden.
- After answering, the popup must NOT stay in the user's face: normal (not keep-above) closable window, findable in dock/Alt+Tab when minimized, closes itself when the call ends. Closing it never ends the call.
- Never print phone numbers, names, MACs, or call times in tool output. Summarize real data as counts only (a slip happened once via the Snapshot caller field — redact that field).
- FaceTime Audio is not possible (see Open decisions); don't attempt it.

## Current state (all installed and running)

- **Permanent oFono backend, user-approved.** [packaging/ofono-enable.sh](packaging/ofono-enable.sh); undo [packaging/ofono-rollback.sh](packaging/ofono-rollback.sh). oFono 1.31 enabled, dundee disabled, WirePlumber override `~/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua`. Side effect: Bluetooth headset mics (HFP) likely unusable while oFono owns HFP.
- **Daemon** [src/bin/iblueconnectd.rs](src/bin/iblueconnectd.rs), session D-Bus `org.iblueconnect.Daemon` at `/org/iblueconnect/Daemon`:
  - `Snapshot() -> (sssss)` status, id, phase, audio, caller (name > number > "Unknown caller"; "" idle).
  - `Answer/Decline/HangUp(id)`, `Dial(number)`, `SendTone(id, key)`.
  - `Contacts() -> (s, a(sss))` status + (name, label, number); `Recents() -> (s, a(sssx))` status + (name, number, kind, unix start), newest first, ≤500, kind ∈ missed/received/dialed/declined/failed/unknown; `SyncContacts()` syncs both.
  - Signals: `Changed`, `ContactsChanged`, `RecentsChanged`.
  - `Presenter` wires [src/presentation.rs](src/presentation.rs): incoming + unlocked → spawns `iblueconnect --incoming` + one `canberra-gtk-play -i phone-incoming-call` (skipped if DND). Unknown lock ⇒ locked; unknown DND ⇒ silent. Rechecked on the 5 s tick.
- **Sync** (one thread, triggered when the backend becomes ready and by `SyncContacts()`): [src/pbap.rs](src/pbap.rs) `pull_phonebook(bus, address, book, timeout)` pulls `pb` then `cch`, each failing independently. [src/vcard.rs](src/vcard.rs) parses contacts; [src/call_log.rs](src/call_log.rs) parses call history and merges with local history (dedupe: last-9-digit number key, compatible direction, ±120 s, local record wins; names local > vCard > unique contact match). [src/contacts_store.rs](src/contacts_store.rs): rusqlite 0.40 on system libsqlite3, `~/.local/share/iblueconnect/contacts.sqlite3` (0700/0600), one-transaction replace per table; empty/failed pull keeps the last good data. Synced missed calls never alert/popup/notify.
- **Real results:** contacts 904 numbers, 0 malformed (iPhone "Sync Contacts" must be on; otherwise OBEX Connect fails 0x41 Unauthorized). Call history: iOS shares only the latest 100 (`cch`, all with X-IRMC-CALL-DATETIME); merged Recents 102 entries, 0 duplicates, 2 unnamed.
- **GUI** [src/iblueconnect-ui.c](src/iblueconnect-ui.c) (C, GTK4/libadwaita 1.5), default 500×680:
  - Shared compact illustrated connection card (battery and calling status), caller, Answer/Decline/Hang up, error label (empty rows hidden).
  - AdwViewStack tabs: **Recents** (first; kind icons, missed in red, "Missed · 09:05 / Yesterday / weekday / date", call-back button, Sync now), **Keypad** (entry + Call + 0–9 * # + ⌫; DTMF during active call), **Contacts** (search name/digits, per-number call button, Sync now, last-sync status).
  - `--incoming` popup: separate app ID `org.iblueconnect.IBlueConnect.Incoming`, unmapped until Snapshot confirms ringing, X11 `_NET_WM_STATE_ABOVE` + user_time 0 only while ringing, close blocked only while ringing, keeps its taskbar entry.
  - Pure helpers are tested in the `IBLUECONNECT_UI_TEST` self-check at the bottom of the file.
- Tests: Rust 74 lib (+2 ignored live probes) + 18 daemon pass; C self-check passes; release build warning-free.
- User-confirmed on hardware: outgoing call from the app, contact sync, call-history sync. Popup-after-answer and minimize/Alt+Tab fixes are installed but **not yet observed on a real call**.

## Commands

- Rust tests: `T=.tools/rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin && RUSTC="$PWD/$T/rustc" RUSTDOC="$PWD/$T/rustdoc" CARGO_TARGET_DIR=/tmp/iblueconnect-target-root $T/cargo test --offline`
- Live probes (opt-in, counts only): `... cargo test --offline --lib live_call_history_probe -- --ignored --nocapture`; `audio::tests::live_route_has_playback_and_capture` needs an active call.
- C tests: `cc -Wall -Wextra -DIBLUECONNECT_UI_TEST src/iblueconnect-ui.c -o <scratch>/ui-test $(pkg-config --cflags --libs gtk4 libadwaita-1) && <scratch>/ui-test`
- Install: `sh packaging/install-user.sh` (user-level only). Restart the daemon only when Snapshot phase is `idle`; a restart triggers a sync.
- Headless screenshot: `xvfb-run -a dbus-run-session -- sh -c "GSK_RENDERER=cairo GDK_BACKEND=x11 timeout 45 ./iblueconnect & sleep 32; ffmpeg -loglevel error -f x11grab -video_size 520x720 -i \$DISPLAY -frames:v 1 out.png"` (~30 s portal stall; no xdotool, so only the first tab is capturable).
- obexd drops a PBAP session when the D-Bus client that created it disconnects: probe from one long-lived process (Rust test), never from separate `gdbus` calls.
- Sudo is non-interactive here, but still get user approval for system changes.

## Open decisions / next work

1. **FaceTime Audio: not feasible** — HFP dials only cellular calls, Apple exposes no API to start FaceTime from another device, no Linux FaceTime client; spec lists it outside V1. Offered alternative awaiting user decision: a "Join FaceTime link" button opening a facetime.apple.com link in the browser (laptop mic/internet, not the iPhone). Do not build without approval.
2. User-coordinated verification on a real incoming call: popup Answer/Decline/Ignore, un-pinning after answer, minimized popup findable, both audio directions, Hang up, Recents updating after the call. If one-way audio recurs, capture a sound-free PipeWire graph during the call.
3. Known gaps: popup doesn't self-hide on lock (GNOME shield covers it; answer is guarded); missed-call notifications not built; daemon notices a newly connected phone only on its retry tick (≤30 s); sync timeout leaves a lingering reader thread (`ponytail:` in pbap.rs); last-9-digit number key can over-match short/foreign numbers (`ponytail:` in call_log.rs); Recents relative times refresh only on reload; Use laptop/Use iPhone handoff, mute, lock/DND acceptance still open per spec §13.

## Suggested skills

- `tdd` for any new behavior (one test → code, per cycle), `diagnose` for runtime call/audio failures, `ponytail:ponytail` to keep diffs minimal, `verification-before-completion` before claiming hardware behavior.
