# IBlueConnect — Product & Technical Specification

Status: product decisions agreed, including active-call exit behavior; three independent specification audits completed. The user reports an existing working phone-initiated audio route to the ASUS laptop and connected headphones. Agent-run hardware and desktop verification have not run.

**2026-10-23 voice indicators, privacy, and exit controls (installed 2026-10-23, live acceptance pending):** The user requests a smooth waveform for their outgoing voice using the current Ubuntu accent, flat while muted, and a small filling bar for the other person's voice. The waveform follows the source actually linked into outgoing HFP (including RNNoise); no link or an ambiguous link leaves it flat. It indicates the local outgoing signal, not confirmation of remote audibility. The incoming bar and caller speaking highlight remain independent of outgoing mute. Both indicators appear in compact mode. Read-only audio meters stop while locked; no audio is saved. A fail-closed GNOME lock watcher hides the main window, controller, and application dialogs; unlocking waits for fresh call state before restoring the controller. It never alters the actual call's audio or acts as Ignore.

Safe Quit, Disconnect, and confirmed Forget are exposed in the device menu. Fresh known-idle call state, no HFP audio nodes, no pending call/sync action, and a confirmed unlocked session are required. Disconnect targets only the phone already selected by oFono and verifies BlueZ disconnection; Forget then clears app-owned contact/history caches, preserving system Bluetooth pairing. Monitoring stays paused until explicitly resumed; resuming never initiates Bluetooth. Quit stops the daemon normally; closing the main window still preserves background monitoring. **Active-call automatic audio handoff remains unimplemented:** the available backend has no exposed, verified phone-route transfer operation. Active or unknown calls block these actions with an explanation instead of risking interruption. This is a guarded partial implementation of the original handoff-and-exit requirement, not full V1 acceptance.

**2026-10-23 mid-call connect:** If the phone connects while a call is already in progress, the floating controller opens once, silently, as soon as the screen is unlocked. A call answered on the iPhone after ringing here still stays on the phone.

**2026-10-23 appearance choice (UI installed):** The phone menu offers System / Light / Dark / Black (Black = GNOME Dark with near-black surfaces). The choice is saved per user and applies to the main window and the call controller together; System follows GNOME and is the default. This refines "follows the system light/dark appearance" into the default of a user choice.

**2026-10-23 controller style (UI installed):** The whole call controller follows the voice strip: soft rounded tiles (Mute, Audio, Keypad with captions under icons; Mute keeps the crossed-microphone icon and reads "Muted"), a wide solid-red End pill, equal-width Ignore/Decline/Answer pills, bold timer. This supersedes the 2026-10-04 icon-only Mute.

**2026-10-23 Apple-inspired voice strip (UI installed):** One pill-shaped bar strip in the call controller: your voice in the system accent enters from the left, the caller's voice in green from the right, quiet bars are grey dots, your side stays flat while muted, and the avatar ring takes the current speaker's colour. "You" and the caller's name label the two sides. Not a copy of Apple's design; Apple publishes no specification.

**2026-10-23 list performance and layout (UI installed):** Lists rebuild only when their data, search text or call availability changes; searches filter after a short typing pause; Recents shows 50 calls with "Show more" (50 at a time). Separators span 90% of the list width and scale with the window. Siri is a large "Ask Siri" accent pill in its own row under the connection card. The incoming controller has no empty space below its buttons. **Deferred (user will return):** virtualized `GtkListView` lists for Contacts and Recents. **Pending decisions:** FaceTime identifier probe; Apple-style two-direction, two-color waveform.

**2026-10-23 iPhone-style Recents (UI installed):** Recents rows have no Call button; tapping a row calls that person back. Thin separator lines span the middle 80% of the list between rows. Siri is its own accent button (microphone icon) on the connection card, visible on all tabs. **Planned next:** an "!" info button on each recent row listing all recent calls with that person; list performance work (rebuild only on data/search change, debounce search, virtualized lists).

**2026-10-23 Siri button and Recents layout (installed, live acceptance pending):** User-approved: a Siri button (now on the connection card) starts Siri on the iPhone over HFP voice recognition (oFono `Handsfree.VoiceRecognition`), so the user speaks and hears Siri through the laptop and can say e.g. "FaceTime audio <name>". Enabled only with a ready link, no calls, and a confirmed unlocked screen. Recents rows show the date under the name and align every Call button directly beside the names.

**2026-10-23 FaceTime detection and dual-SIM lines (researched, decision pending):** The user asked to mark received/missed calls as FaceTime Audio and to call from either of two eSIM lines. HFP/oFono/PBAP carry no call-type or line field and HFP has no line-select command, so neither can be built reliably over this link. FaceTime presence over HFP needs a user-approved live probe (identifier form only, never its value). Dual SIM: the iPhone picks the line (default or the contact's preferred/last-used line). No implementation until the user decides.

**2026-10-17 full multi-call controls (built offline, not installed):** User-approved: call waiting (Ignore, Decline, Hold & Accept, End & Accept; controller plus one soft `message-new-instant` beep per waiting call, none under DND, never auto-answer), Swap with a per-call list, Add call from the controller, and conference management (participant list, End one, Private, add callers up to five then Merge, End conference). Daemon methods: `Swap()`, `PrivateChat(s)`, `HoldAndAnswer(s)`, `ReleaseAndAnswer(s)` plus widened `AddCall`, `Merge`, `EndCall`, `EndConference`; each checks a fresh oFono call list. This supersedes the earlier exclusion of conference calls and call waiting. Live acceptance pending.

**2026-10-04 call-control appearance:** Answer stays green and Decline stays red regardless of GNOME's accent color or light/dark appearance. Mute is an icon-only microphone button in both expanded and compact controllers, switching to the standard crossed-out microphone when muted; retain Mute/Unmute tooltips, accessible names, and the keyboard shortcut. These UI-only changes are installed and pass test-first offline checks on Ubuntu 24.04 and 26.04. Live visual acceptance remains pending.

**2026-09-30 preview delivery:** The user requests separate native amd64 installers for Ubuntu 24.04 and 26.04 from one shared source branch. Both packages build, pass offline checks, and install in isolated native Ubuntu containers. First-run setup requests combined background/audio/sync consent, preserves the physical default input, installs version-appropriate oFono/call-only RNNoise configuration, and retains backups/Undo. Separate saved sync opt-out and other persisted preferences remain missing. Fresh-desktop user-operated Bluetooth acceptance, including native 26.04 oFono 2.x, remains pending. Installed-package isolated main-window measurements are 100–115 MiB PSS, above the under-80 MB target; this preview does not meet full V1 acceptance. See [README.md](README.md) for installation, measurements, and remaining limitations. GitHub publication is pending.

**2026-09-30 Ubuntu upgrade:** The current machine reports Ubuntu 26.04.1 LTS, WirePlumber 0.5.13, PipeWire 1.6.2, and BlueZ 5.85. This supersedes the older installed-version assumptions below. The permanent oFono backend requires a SPA-JSON fragment under `wireplumber.conf.d` on WirePlumber 0.5 and later; the old `bluetooth.lua.d` override is ignored. Retain the Lua format for WirePlumber 0.4 and keep rollback compatible with both formats. The user reported a silent call after upgrading and confirmed working two-way audio after the approved migration. See [Feasibility.md](Feasibility.md).

**2026-09-30 microphone-filter decision:** After the backend migration, the user confirmed a working two-way test call and requested RNNoise only for IBlueConnect's phone calls. Preserve Ubuntu's chosen default input for other apps. Route only the outgoing HFP microphone stream through the existing RNNoise source, whose input follows the selected physical microphone; incoming audio stays unchanged. If RNNoise is absent, retain the ordinary Ubuntu input route. The installed rule matches the HFP profile and outgoing SCO factory at node creation; the earlier `media.class` match missed that stage and bypassed the filter. Eleven offline tests pass, and the corrected rule was applied with user approval and fresh idle checks. A user-operated active call now verifies physical mic → RNNoise → outgoing HFP, with zero direct microphone bypass and the physical default unchanged. The user confirmed working duplex sound and reduced background noise. Call-only filtering is verified for this test call; filter-absent fallback and physical-device switching remain unverified.

**2026-09-28 call-controls update (in progress):** The user requests a movable floating call controller, access to the main window while the controller is visible, and the actual calling number when a contact has several numbers. The user also requests mute, hold, adding another caller, and merging calls into a conference. These requests supersede the older "conference calls and complex call waiting" exclusion below. Implement each control only when the connected oFono modem exposes a safe operation and the UI can identify the affected call; retain phone-side management when it cannot. No automatic call placement or answer is authorized for verification. Offline tests precede each implemented behavior; real-call validation remains user-operated.

**2026-09-29 number-display step:** The floating controller uses the number reported by oFono for the current call, including a contact's secondary number. A withheld number stays blank; the UI must not substitute the contact's first number. `SnapshotDetails` carries status, call id, phase, audio route, caller label, and raw number in one reply. The old five-field `Snapshot` remains for rollback compatibility. Offline tests cover a secondary number and an update within the same call.

**2026-09-29 controller step:** Dragging the caller header moves the floating window. X11 corner placement happens when the window first appears and later refreshes preserve the chosen position. Incoming and active views have **Open app**, which opens the main window alongside the controller. The controller exposes Hold, Resume, and Merge only in states the daemon permits. A multiparty call shows its count and uses **End conference**. These paths pass offline GTK tests; real GNOME placement and live calls remain to be verified.

**2026-09-29 mute and voice step:** Live **Mute** mutes only the call's outgoing HFP PipeWire stream (never the system or iPhone microphone). It is shown only while the daemon's `CallAudio()` finds this call's audio on the laptop, and `SetMuted(b)` changes it. A mute restored from an earlier call is cleared once per call; within a call, mute survives a phone/laptop handoff through WirePlumber's stream restore. The controller's waveform follows the measured level of the other person's voice, tapped read-only from the call's incoming stream; no looping animation. While they speak, the caller's avatar and name light in the GNOME accent color, in both the expanded and compact controller. The whole controller can be dragged. On X11 it no longer counts as the app's window for GNOME Shell, so the app icon opens the main window during a call. Offline tests pass; live calls are unverified.

**2026-09-29 multi-call step:** The daemon exposes `Calls()` with each call's id, phase, audio location, caller label, raw number, and multiparty flag. Guarded `Hold(id)`, `Resume(id)`, `AddCall(number)`, `Merge()`, `EndCall(id)`, and `EndConference()` require a fresh oFono call list and the exact eligible state. The main dialer says **Add call** only for one active non-conference call; a second call can be managed and merged once one is active and the other held. A held call remains available for Resume, with its timer paused and keypad hidden. The old `Snapshot()` behavior and single-call rollback UI remain unchanged. Offline Rust and GTK tests pass. iPhone/carrier execution and any conference beyond two callers remain unverified. Live **Mute** stays hidden: the disconnected modem does not establish a working HFP `Muted` control or a safe per-call PipeWire capture path. A second waiting call is visible through `Calls()` in an open app; its alert policy still needs a separate decision.

**2026-09-25 update:** The user replaced the application-owned incoming-call popup requirement with a native GNOME notification-center entry. It has Answer, Decline, and Ignore actions; Ignore removes only the entry, while the call continues ringing on the iPhone. The main window still opens only from its app icon. Earlier popup language below is historical and superseded by this decision. Live GNOME call acceptance remains to be tested.

**Visual decision:** The user selected the native connected-device layout: an iPhone-to-laptop link graphic with calling status and battery. The same compact illustrated card appears on Recents, Keypad, and Contacts. The link is red with an X while calling is unavailable and blue when ready. The phone keeps a compact shape in maximized windows, the connection card has a 600 px maximum width, and the full keypad fits the default window. It follows the current system light or dark appearance through libadwaita. The other two comparison mockups were discarded.

This replaces the initial specification. **Required** means a V1 release criterion. Proposed technology is not evidence of compatibility. There is no implementation yet.

The interview decisions govern product behavior. Details such as the 500-entry history bound, startup defaults, and daemon/UI split are implementation defaults carried forward or selected for this design, not separate user approvals. They may be refined without weakening agreed behavior. See [review findings](IBlueConnect_Design_Review.md) for evidence and remaining risks.

## 1. Product and target

IBlueConnect makes the user's Linux laptop an optional hands-free endpoint for their iPhone's cellular calls: native, lightweight, local-first, and available without leaving the main window open.

| Component | Target / observed baseline |
|---|---|
| Phone | iPhone 17, iOS 27 as reported by the user; capture exact build during testing |
| Computer | This x86_64 Ubuntu 24.04 laptop |
| Desktop | Ubuntu GNOME on X11 |
| Bluetooth | BlueZ 5.72 |
| Audio | PipeWire 1.0.5; WirePlumber 0.4.17 |
| Native UI | GTK 4.14.5; libadwaita 1.5.0 |
| Telephony candidate | oFono; absent at initial inspection |
| Required headphones | User's 3.5 mm wired headphones/headset |

Use devices exposed by Ubuntu, with no headset-model allowlist. Generic wired/USB audio support is intended; only tested hardware may be described as verified. Bluetooth headphones and other operating systems, desktops, and phone combinations are outside initial guaranteed support.

Delivery proceeds from spec agreement and multi-agent audit to implementation and real-device verification. A hardware feasibility gate precedes full interface implementation.

### Existing working behavior — user-reported baseline

On the current setup, with the iPhone connected, the user can:

1. Answer a cellular call on the iPhone.
2. Use the iPhone's audio selector to route the ongoing call to the ASUS laptop.
3. Use the laptop's connected headphones for call audio.

This already works without IBlueConnect and is evidence from the user, not an agent-run acceptance test. Preserve it as the baseline when investigating call control and audio integration. Inspect and reuse the existing working route before installing another telephony backend or changing Bluetooth/audio configuration; oFono is a candidate, not a mandatory replacement.

The report does not separately establish which microphone was used, laptop-originated answer/decline/dialing, laptop-initiated transfers in either direction, DTMF, or automatic route changes during an active call. Verify those individually without treating existing headphone playback as proof of the entire V1 path.

## 2. Agreed V1 scope

### Required

- Receive cellular calls; answer, decline, and hang up from the laptop.
- Manually enter and dial numbers.
- Carry two-way call audio through the laptop's selected headphones/output and microphone.
- Follow Ubuntu's output and input selections, including changes during calls.
- Leave calls started or answered on the iPhone there until the user requests otherwise.
- Select laptop audio from the iPhone's call-audio choices.
- Transfer an ongoing phone call using **Use laptop**, and return it using **Use iPhone**, without ending it.
- Mute laptop call audio and send in-call keypad tones (DTMF).
- Provide the incoming-call popup, DND/lock behavior, and missed-call notifications defined below.
- Maintain bounded local recent-call history.
- Reconnect to one primary trusted phone and remain available after closing the main window.
- Provide login autostart, connection status, guided first run, and minimal settings.

### Desired, not release-blocking

PBAP contact sync, cached contact search, caller-name matching, optional phone battery display, and tray integration where supported.

Unavailable, refused, or failed contact sync must never block calling. Show raw caller numbers; withheld or unavailable numbers display **Unknown caller**, with no invented number or callback action.

### Outside V1

Bluetooth headphones, multiple phones, contact editing, SMS/iMessage, FaceTime integration, general phone notifications, photos/files, iCloud, mirroring, recording, conference calls, and complex call waiting.

For unsupported multiple-call situations, preserve the existing call and leave management to the iPhone. Do not silently decline another call or guess which call a command targets.

## 3. Audio ownership and handoff

Call state and audio location are separate. An active call does not imply laptop audio ownership.

| Action | Required result |
|---|---|
| Answer/dial through IBlueConnect | Audio uses laptop system-selected output and input |
| Answer/dial directly on iPhone | Audio stays on iPhone |
| Select laptop in iPhone audio selector | Transfer to laptop without ending call |
| Click **Use laptop** for a phone-owned call | Request and verify transfer to laptop |
| Click **Use iPhone** for a laptop-owned call | Request and verify transfer to phone |
| Change route directly on iPhone | Laptop UI reflects actual audio location |

Show **Switching audio…** while pending. Announce success only after observing it. Transfer failure must show an error and preserve the call and last working route where possible. Never hang up and redial to simulate handoff.

Pairing, automatic reconnection, and restarting the UI must not seize an ongoing phone call. Both transfer directions, particularly laptop-initiated transfer of phone-owned audio, are required feasibility tests; basic HFP connectivity is not proof.

## 4. Follow Ubuntu audio

Ubuntu owns device selection. Follow its current default output and input, including moving existing call streams when those defaults change. Verify actual movement rather than assuming it.

- No independent device-priority policy or persistent application device overrides.
- Follow Ubuntu on wired/USB connection and removal.
- Use a headset microphone when Ubuntu selects it. Headphones without a mic use the system-selected input, normally the built-in mic.
- If unplugging makes Ubuntu select built-in speakers, follow that selection without ending the call.
- Display actual output and input separately; offer **Open Sound settings**.
- Mute only this application's outgoing laptop call audio, not the global system microphone. Preserve laptop mute across device switches and a phone/laptop handoff round trip within the same call; reset it for a new call. Clearly indicate that laptop mute does not mute the iPhone microphone when audio is on the phone.
- If an input/output is unavailable, identify it, offer **Use iPhone**, and do not falsely report working laptop audio.

Bluetooth headphone chaining is unnecessary; Bluetooth between the iPhone and laptop remains required.

## 5. Incoming calls and notification center

While unlocked, show a compact, application-owned incoming-call popup even with the main window closed. The daemon must be able to activate that small UI surface.

The popup:
- Appears prominently above ordinary windows without stealing keyboard focus.
- Shows name when available, otherwise number or **Unknown caller**.
- Provides labeled **Answer** and **Decline** buttons.
- Remains visible without timeout while ringing.
- Plays one brief alert sound when first presented; no repeating ringtone.
- Remains visible during Do Not Disturb. DND suppresses sound only.

Do not relay an iPhone's in-band ringtone through a prematurely opened call-audio bridge; that would bypass the single-sound and DND rules. Do not transmit microphone audio before answer or explicit transfer of an active call.

While ringing, omit close/minimize controls and prevent Escape/Alt+F4 from accidentally declining or hiding the persistent popup. It remains deliberately keyboard-focusable so Answer/Decline are accessible. After answering, closing call controls only hides the UI and never ends the call.

An application popup is distinct from a native notification banner. Do not rely solely on a desktop banner to meet persistence or DND visibility. Verify behavior in this GNOME/X11 session.

| Event | Behavior |
|---|---|
| Answer on laptop | Convert popup to active-call controls |
| Answer on phone | Remove incoming popup; ongoing call available in main UI with **Use laptop** |
| Decline | Remove popup; record declined, not missed |
| Caller hangs up unanswered | Remove popup immediately; create one missed history entry and a notification-center entry |
| Ringing ends for another known reason | Remove stale Answer/Decline controls; reflect actual outcome |

A missed-call notification stays in the notification center with **Call back** and **Dismiss**, until dismissed, subject to desktop/session retention limits. Omit Call back when the number is unavailable. Dismiss removes the notification, not history. Post the missed-call entry silently and without a new banner; never replace the incoming popup with another missed-call popup.

Verify that GNOME retains the notification and exposes both actions; requesting notification hints is not proof. If this server cannot provide that behavior, raise the conflict for a product decision instead of silently substituting a permanent window.

Test at least four consecutive missed calls: desktop per-application retention limits may evict older notification entries before dismissal. Record the observed limit and keep all locally recorded calls in Recent; do not claim unlimited notification retention. If this prevents the required experience, resolve notification grouping with the user before implementation sign-off.

Deduplicate events, sounds, history, and notifications. Recheck call state before actions. Stale actions cannot answer ended calls or cause duplicate dialing. A disconnected callback reports that connection is required; never silently queue a future call.

## 6. Lock screen

New calls while locked remain answerable on the phone. No caller details, incoming-call popup, or laptop answering are permitted until unlock. This takes precedence over DND popup visibility.

- Hide call surfaces and remove/redact caller-bearing notifications on lock; cached notifications must not expose identity.
- On unlock, show the popup if the call is still ringing. If this is its first presentation and DND is off, play its one alert; never replay an alert already presented before lock. Restored missed-call entries are always silent.
- Record calls missed while locked; post identifying missed-call notifications after unlock.
- On unlock, restore previously undismissed missed-call entries removed for privacy, without duplication or replaying sounds. Track user dismissal separately from application removal for lock privacy.
- An existing laptop call continues uninterrupted on lock; locking does not transfer audio or hang up.
- Revalidate lock state when executing UI/notification actions.
- Do not play an incoming alert sound for newly arriving calls while locked.

Verify both application and GNOME notification surfaces on the target session.

## 7. Interface and visual direction

GTK 4/libadwaita, native widgets, minimalist and clean, with more personality than the original ASCII wireframes:

- Restrained blue accents, generous spacing, softly rounded native surfaces, crisp symbolic icons.
- Follow system light/dark preference.
- Green for answer/call; red for decline/end, always with labels or accessible names.
- No gradients, decorative animations, large illustrations, or complex navigation.
- Keyboard access, visible focus, screen-reader labels, text scaling, contrast, and reduced-motion support.

Implement accents using facilities available in installed libadwaita 1.5; do not assume newer accent-color APIs are available.

Main window: **Recent** by default, number entry, and easy access to **Dial pad**. If contact sync is implemented, extend the entry to **Search contacts or enter number** and provide **Contacts** navigation. Otherwise omit contact-only navigation; explain a sync failure when applicable without blocking dialing. Empty views use helpful prompts, not fake records.

Header: phone name and explicit status—Bluetooth unavailable, Disconnected, Connecting, Connected but calling unavailable, or Ready for calls. Bluetooth connectivity alone is not readiness. Status details offer connect/disconnect, actionable errors, and optional battery.

### Dialing and active calls

Offer **Call [number]** for valid number input and, if contacts are implemented, search cached names/numbers. Include 0–9, *, # and international + entry. Validate input without assuming fixed number length or requiring a contact.

Before calling, keypad input edits the number. During an active call it sends DTMF to that call. Focus/navigation must not transmit tones or initiate another call.

Show identity, call state, duration from active state, and audio location. Laptop-owned calls expose **Mute**, **Keypad**, **Use iPhone**, **Hang up**, and actual input/output. Phone-owned calls prominently offer **Use laptop**; never imply laptop mute controls the phone microphone. Unsupported controls are disabled with a reason.

## 8. State and failures

The daemon owns authoritative call state, audio location, and pending actions. UI clients receive an initial snapshot and subsequent updates. Reopening UI never starts a second call.

Distinguish idle, incoming ringing, outgoing dialing/alerting, active, and ending; retain terminal outcomes completed, missed, declined, busy, failed, or interrupted. Audio location is independently phone, laptop, switching, or unknown.

- Use backend call identity/state; reconcile after startup or reconnection.
- Serialize conflicting actions and disable repeated Answer/Dial/transfer while pending.
- Show supplied failure reasons; do not infer busy from every unanswered call.
- Start duration from active, not ringing.
- Bluetooth loss is not proof the cellular call ended. Show lost visibility and remove misleading controls; reconcile without seizing phone audio.
- Daemon/audio failures must not look successful. Verify phone behavior during failures rather than promising untested automatic recovery.
- Closing or crashing the GUI must not terminate the call or its audio path.

## 9. Optional contacts and local history

### Contacts

Ask for sync consent separately from pairing. Denial leaves calling intact. If available, cache PBAP names, numbers, and number labels in SQLite under the XDG user data directory (default ~/.local/share/iblueconnect/).

Allowed alternative (user decision, 2026-09-24): a user-only JSON file replaced atomically (write temporary file, then rename) meets the same last-good-cache rule for a phone-sized contact list and needs no extra library. SQLite remains the current implementation; switching to JSON is permitted if the SQLite dependency becomes a burden.

Support multiple numbers per contact and explicit number selection. Sync on suitable reconnection events and **Sync now**; use change metadata only when available. Avoid continuous polling and repeated permission prompts. Show last successful sync and errors. Replace data transactionally only after successful sync; interruptions preserve the last good cache.

Match caller numbers conservatively. Ambiguous matches show the number. Photos, editing, cloud sync, and writing contacts back to the phone are outside V1.

### History

Keep the latest 500 locally observed calls: number when available, name snapshot when known, direction, timestamps, active duration, outcome. This is not the complete iPhone history; do not invent disconnected-period calls.

Deduplicate events. Preserve unknown/interrupted outcomes rather than falsely claiming completion. Notification dismissal does not remove history. Settings provide **Clear call history**.

### Next step: sync missed calls and recents from the iPhone

User request (2026-09-24): after contacts, also sync the iPhone's own call history so **Recent** and missed calls include calls the laptop did not observe (for example, calls while disconnected or while the laptop was off).

- Source: Bluetooth PBAP call-history phonebooks over the already-connected phone (`mch` missed, `ich` incoming, `och` outgoing, or `cch` combined), through the same obexd path as contacts. The app still never initiates the Bluetooth link. Whether this iPhone/iOS build exposes call history over PBAP, and with what timestamps and limits, is unverified; test it first and report the result before building UI on it.
- Sync on the same triggers as contacts (phone becomes ready, **Sync now**), bounded to the same 500-entry history limit, stored privately alongside local history.
- Merge with locally observed history without duplicates: match on number, direction, and time within a small tolerance; prefer the locally observed record (it has accurate outcome/duration) and mark phone-synced entries as such. Never invent outcomes the phone did not report.
- Missed calls learned only from sync appear in **Recent** as missed, but must not raise an alert sound or incoming popup, and must not post missed-call notifications for calls that ended before the sync (avoid a burst of stale notifications).
- Caller names come from the synced contacts cache when matched; otherwise show the number or **Unknown caller**.
- Refused or failed history sync never blocks calling or contacts; show the reason (for example, the iPhone's **Sync Contacts** permission) and keep the last good history.

## 10. First run, background, and settings

First run checks dependencies and guides pairing/selection of one phone through native Bluetooth facilities. Explain permissions and actionable errors without routine terminal use. Ready means initialized call capabilities. Offer optional contact sync afterward.

Use a small background daemon (iblueconnectd) and separately activated GTK UI (iblueconnect) over session D-Bus. The daemon owns connection/calls, audio orchestration, storage, and notification lifecycle. Do not keep a full main window alive for incoming calls.

At login, and after the laptop resumes, the app runs silently in the background and monitors the trusted iPhone when Bluetooth is connected. An incoming call opens only the persistent call popup with Answer and Decline and plays its single alert sound when lock and DND rules permit. The main dialer and contact-sync window never opens automatically for a call or at login; it opens only when the user launches IBlueConnect from its normal application icon. Closing that main window leaves background call monitoring and the incoming popup available.

Start at user login via systemd user service. Expose **Start automatically** and **Connect automatically**, enabled after setup. Closing the main window leaves the daemon available. Provide explicit **Quit IBlueConnect**.

Manual **Disconnect** suspends automatic reconnection until explicit reconnect or a new session. Unexpected loss retries with bounded backoff, for example 1, 2, 5, 10, 30 seconds capped at 30, while auto-connect is enabled. No busy loops with disabled Bluetooth or during suspend.

Tray is optional; the desktop launcher always reopens controls. UI failure must not kill the daemon. Daemon restart reconciles state without auto-dialing or auto-answering.

Settings:
- Start automatically; connect automatically.
- Primary phone, connection action, **Forget phone**.
- Actual output/input, audio follows Ubuntu, **Open Sound settings**.
- Optional contact permission, last successful sync, **Sync now**.
- **Clear call history**.

No independent device priority or generic notification switch that silently defeats the required popup. Alert sound follows DND and system volume.

**Hang up / End call** terminates the cellular call on the iPhone too. It is distinct from closing a window, transferring audio, quitting the application, or disconnecting Bluetooth.

For **Quit** or **Disconnect** during a laptop-owned call, first transfer audio to the iPhone. Complete the requested exit/disconnection only after verified handoff. If the transfer fails or its result is unknown, remain running and connected, preserve the call, and show an actionable error. Never silently hang up to complete an exit action. If audio is already confirmed on the phone, preserve that call and proceed without reacquiring laptop audio. If the call ends naturally during handoff, proceed after confirming it ended. Recheck state before completing the action.

Apply the same call-preserving sequence before **Forget phone** removes the connection or app data. Forget clears app-owned phone identity, contacts, and history only after safe disconnection; distinguish it from system-wide Bluetooth unpairing.

## 11. Proposed architecture and feasibility gate

| Concern | Proposed owner |
|---|---|
| Pairing, trust, Bluetooth transport | BlueZ |
| HFP hands-free call control | oFono candidate; real-device validation required |
| Audio streams | PipeWire |
| System device policy | WirePlumber |
| Optional contacts | BlueZ OBEX/PBAP where supported |
| State, lifecycle, privacy, UX | IBlueConnect daemon and GTK UI |
| Local cache/history | SQLite |
| Login startup | systemd user service |

Prefer Rust, GTK 4/libadwaita, and D-Bus. Avoid Electron, embedded Chromium, Node.js backends, and a web shell as a substitute for native integration.

**Python environment requirement:** if any Python application code, prototype, test, or project tooling is needed, create a project virtual environment first and run it through that environment's interpreter. Install Python packages only inside that environment; do not run project Python with the global interpreter or install into system Python. Creating the virtual environment itself is the bootstrap exception. Do not introduce Python or create an unused environment if the Rust/native toolchain suffices.

The original BlueZ/oFono/PipeWire diagram was a candidate, not validated wiring. Establish HFP ownership, installed PipeWire telephony-backend compatibility, audio-transport acquisition/release, and service configuration together. Avoid competing profile owners and unreviewed system-wide audio changes. Use configuration appropriate to the installed WirePlumber 0.4 series, rather than assuming newer configuration examples apply.

Prove the actual duplex audio bridge: phone audio to Ubuntu's selected output, and selected microphone to the phone. Keep the Bluetooth sides bound to the selected phone and local sides following system defaults; prevent feedback and accidental capture of desktop-monitor audio. Reuse existing PipeWire stream/loopback facilities where sufficient. Call-control state, Bluetooth control connection, and SCO audio transport are distinct; none alone proves the iPhone's exact internal output selection. If only phone-versus-laptop location is observable, show that distinction without inventing earpiece/speaker details. An ambiguous route remains **Unknown**.

For handoff, explicitly test acquiring/releasing the audio transport without hanging up or losing call-control visibility. Verify that normal graph/policy activity does not automatically reacquire audio after **Use iPhone**. No telephony API should be assumed to expose a complete audio-selector control.

Record the chosen oFono version and hands-free plugin, installed PipeWire backend, profile/audio-agent owner, D-Bus permissions, reversible WirePlumber configuration, and startup/restart ordering. Also record the iPhone's exact iOS build and Call Audio Routing preference without automatically changing it. A telephony call-transfer API is not an audio-route transfer and must not be used as one.

Before full interface work, demonstrate:
1. Pairing and incoming detection.
2. Answer, decline, hang up, and outgoing dialing.
3. Two-way laptop/headphone audio.
4. Default input/output changes and unplug/replug during calls.
5. Phone-answered/originated calls staying on the phone.
6. iPhone audio selector plus both laptop-initiated handoff directions.
7. Real DTMF reception by an automated menu.

Start by documenting and reproducing the user-reported phone-answer → iPhone audio selector → ASUS → headphones path on the existing configuration. Use it as a regression baseline after each audio/backend change; do not discard a working path merely to match the proposed architecture.

Also run an early desktop feasibility check before full UI work: a GTK4 popup above ordinary windows without focus theft, visible during DND, hidden while locked, plus silent missed-call notification-center entries with working actions and lock/unlock restoration. Do not assume older GTK window APIs exist in GTK4. If the target desktop cannot meet a requirement, document the limitation and return the tradeoff to the user.

If the backend cannot meet a required case, record evidence and evaluate the smallest viable alternative. Do not silently weaken requirements or present mocks as a working product. Contact feasibility is independent and cannot delay this gate.

Only create modules/binaries justified by the proven integration; the initial seven-crate proposal is not mandatory. Subscribe to events, query initial state, and reconcile after recovery instead of continuously polling.

## 12. Privacy, security, and performance

- Accept telephony only from the one selected paired/trusted phone; validate identity and command parameters.
- No accounts, cloud backend, remote contact storage, analytics, or telemetry.
- User-only data directory and restrictive permissions on the database and related files.
- Never log audio, DTMF digits, contact names, full phone numbers, or persistent device identifiers. Debug logs also remain redacted.
- Session IPC must not expose privileged system operations or arbitrary-device control; use platform authorization.
- Forget removes app-owned identifying data without claiming secure erasure of SSDs/backups.

Targets to measure, not guarantees:
- Background, main UI closed: under 40 MB application memory; effectively zero idle CPU.
- Main UI open: under 80 MB total application memory.
- Event-driven idle; minimal wakeups.

Measure all app processes together and state memory method, observation interval, and environment. Report incremental external-service costs separately rather than hiding newly required dependencies.

## 13. Acceptance matrix

Every required row needs observed target-device evidence before claiming working V1. Mark hardware checks **Not tested** until run; mocks cannot establish Bluetooth/audio support.

Evidence recorded so far: the user's existing workflow supports the phone-initiated laptop audio selection part of V10 and headphone playback part of V07. Mark these **User-reported baseline; full acceptance pending**, since application-state accuracy and microphone direction have not been separately demonstrated. All other rows remain **Not tested**.

| ID | Scenario | Expected behavior/evidence |
|---|---|---|
| V01 | First run | Guided pairing or actionable dependency error; no false ready |
| V02 | Login, window closed | Daemon available, trusted phone reconnects, call arrives |
| V03 | Incoming unlocked | Persistent prominent popup, one sound, controls, no focus theft |
| V04 | Incoming during DND | Popup visible, sound suppressed |
| V05 | Answer/decline/hang up | Real phone transitions; no duplicate actions |
| V06 | Manual dialing | Dial once; accurate busy/failure handling |
| V07 | Wired headphones | Both parties hear selected output/microphone correctly |
| V08 | Defaults change; unplug/replug | Existing streams follow Ubuntu; call survives; labels accurate |
| V09 | Phone-originated/answered call | Stays on phone; laptop observes without taking audio |
| V10 | iPhone audio selector | Laptop selectable; actual route matches laptop UI |
| V11 | Use laptop / Use iPhone | Both directions preserve ongoing call; failure does not end it |
| V12 | DTMF and mute | Remote IVR accepts tones; laptop mute survives device changes and same-call handoff round trip; phone/global mic unaffected; new call resets mute |
| V13 | Caller hangs up unanswered | Popup disappears; one missed record and silent, banner-free notification-center entry |
| V14 | Missed-call actions | Callback once; Dismiss removes only notification; no callback for unknown number |
| V15 | Lock during ringing / new call locked / missed entry already present | Identity hidden, answering disabled; ringing restored on unlock with at most its first permitted alert; missed entries restored silently without duplication; callback/dismiss still work |
| V16 | Lock during active call | Call/audio continue uninterrupted |
| V17 | Answer on phone / stale actions | Remove incoming popup; no false missed call or stale command |
| V18 | GUI closes/crashes/reopens | Daemon/call persist; UI restores authoritative state |
| V19 | Link loss / daemon restart | Honest unknown/unavailable state; no auto-answer or audio seizure |
| V20 | Manual disconnect / reconnect | User action respected; unexpected loss retries without busy loop |
| V21 | Contacts absent/refused/failed | Core calling, numbers, dialing, and history fully usable |
| V22 | Visual/accessibility review | Native blue accents; light/dark, keyboard, labels, scaling, contrast |
| V23 | Privacy/resources | Private data, redacted logs, bounded history, measured idle/UI usage |
| V24 | Missing device / failed transfer | Truthful error, preserve call where possible, offer phone route |
| V25 | Unsupported second call | Existing call preserved; no automatic decline or command against an ambiguous call |
| V26 | Forget phone | App-owned identity, contacts, and history removed; no unintended system-wide unpairing |
| V27 | Incoming in-band ringtone; DND/locked | No repeated or relayed ringtone; no pre-answer microphone transmission; one permitted app sound only when unlocked and DND off |
| V28 | Popup keyboard/window interaction | Typing stays in prior app; normal window switching does not hide ringing popup; intentional keyboard access works; Escape/Alt+F4 cannot decline/hide it |
| V29 | Four missed calls; notification retention | Document actual center retention/actions; all calls remain in Recent; any retention conflict is resolved rather than claimed to pass |
| V30 | Quit/Disconnect/Forget during a call | Successful handoff precedes exit/disconnection/data removal; failed or unknown handoff leaves app connected; phone-owned calls stay on phone; natural call end during handoff is reconciled |
| V31 | End call versus exit | Hang up ends the real cellular call; closing UI preserves it; Quit/Disconnect preserves it through verified phone handoff |

If contacts are implemented, additionally check denied permission, multiple numbers, ambiguous matches, interrupted sync, and last-good-cache retention.

Independent reviewers assess technical feasibility, UI consistency, and requirement/test coverage. Their purpose is conformance, not feature expansion. Distinguish specification review from implementation and hardware evidence.

## 14. Delivery sequence

1. **Agreement and audit:** update this spec, independently review it with multiple agents, return unresolved product tradeoffs to the user.
2. **Hardware feasibility:** prove calls/audio/handoff before backend commitment and full UI; record versions, configuration, commands, and outcomes. Real calls and listening tests need user participation.
3. **Working core:** daemon, trusted connection, state, audio, handoff, DTMF, reconnection, and history.
4. **Native interface:** setup, recents/dialer, popup/controls, DND/lock behavior, notifications, agreed visual direction.
5. **Desktop delivery:** startup, launcher, minimal settings, recovery, accessibility, privacy, resource measurements.
6. **Optional contacts:** add when feasible without delaying required calling.
7. **Final independent review and hardware acceptance:** evaluate the implemented app against the matrix; report failures and untested cases.
8. **Next step — phone call-history sync:** after contacts, verify PBAP call-history support on the iPhone, then sync missed calls and recents into **Recent** as described in §9.

V1 succeeds when this user's real phone, laptop, and headphones work together, with explicit control over where call audio lives.
