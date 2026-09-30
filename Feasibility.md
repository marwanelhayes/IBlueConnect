# Feasibility record

## 2026-09-30 Ubuntu 26.04.1 silent-call diagnosis

The user reported no audio in either direction during a received call after moving to Ubuntu 26.04.1 LTS. Read-only inspection found WirePlumber 0.5.13, PipeWire 1.6.2, and BlueZ 5.85. The installed app-owned oFono override was still `~/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua`; no modern fragment was present. WirePlumber's current-boot journal contained one warning that Lua configuration files are unsupported and two duplicate-profile-registration errors. This establishes a configuration migration defect and suggests competing backend ownership, but does not independently establish the exact failed-call transport.

[WirePlumber's migration guide](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/migration.html) requires SPA-JSON fragments under `~/.config/wireplumber/wireplumber.conf.d/` from version 0.5. The equivalent setting is `monitor.bluez.properties = { bluez5.hfphsp-backend = "ofono" }`. Keep oFono as the approved backend and avoid the previously failed mSBC override.

Private, parsed read-only checks found `ready/idle`, zero HFP streams, one oFono modem, one oFono audio card, one physical microphone source, and the RNNoise nodes. PipeWire, WirePlumber, filter-chain, oFono, Bluetooth, and the app daemon were active; dundee was inactive. No service, connection, microphone selection, or installed configuration was changed. No call was placed or answered. Applying the migration and testing live duplex audio require user participation and approval for service changes.

The packaging migration now passes nine offline regression tests, including both configuration formats and guarded rollback. A fully mocked installer using the actual installed WirePlumber version output generated a fragment accepted by the installed `spa-json-dump` parser. The retained oFono package is 1.31-3ubuntu3.24.04.2; the repair skips reinstalling it.

The user approved applying the fix and confirmed no active call. The old settings were backed up durably (path in `HANDOFF.md`), the modern fragment installed, the obsolete app-managed Lua file removed, and WirePlumber, oFono, and the app daemon restarted with fresh private idle/zero-HFP checks. Guards stopped on transient state during the restart sequence; subsequent fresh idle checks allowed completion without repeating earlier restarts. The installed fragment selects oFono; all core services are active, dundee inactive, Snapshot `ready/idle`, and RNNoise remains present. Post-restart journals have zero old-Lua warnings, duplicate-profile errors, or disconnected-transport errors. No Bluetooth connection was initiated and no agent operated a call. A user-operated live duplex check is still required to establish that the silent-call symptom is resolved.

The user then arranged a test call and reported working two-way audio, with noise cancellation failing. The next graph was already free of HFP streams, so its live routes were not captured. The selected default input was the physical mic; the RNNoise source and capture existed but were suspended. A follow-up probe distinguished paused from active links and found two paused physical-mic links and a shared filter link group. This is a routing/selection finding, not evidence for changing the filter strength or codec.

The user requested RNNoise only for IBlueConnect calls rather than setting the filter as the global input. No global input change was made. The native WirePlumber rule targets only HFP `Stream/Input/Audio` nodes, routing them to `echo-cancel-source` with normal default-source fallback when the filter is missing. [WirePlumber's linking policy](https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html) documents `target.object` and the default fallback. Eleven oFono tests pass, including the actual installed native rule matcher and Lua evaluation. The RNNoise installer readiness check now accepts an unchanged physical default while still requiring upstream microphone links; its regression passes.

With a separate explicit approval and no-active-call confirmation, the working modern fragment was backed up and the call-only rule applied after a fresh private idle/zero-HFP check; only WirePlumber was restarted. Settled checks confirm the exact previous physical default input is preserved, RNNoise exists, services are healthy, and the daemon reports `ready/idle`. No filter strength, volume, codec, or binary was changed. Live noise-reduction verification, filter-absent fallback, and physical-device switching are pending.

The next user-operated routing test failed. The subsequent capture already reported `ready/idle`, zero HFP streams, and two active physical→RNNoise links. Recent WirePlumber logs contained one SCO failure, `Transport endpoint is not connected`, without a missing-target, invalid-rule, duplicate-profile, or filter-cycle error. A direct oFono call-list query returned zero calls. The precise transport-failure trigger and live rule matching remain unproven because the graph was not captured during the active call. The user clarified that they had manually selected RNNoise as the global microphone; this explains the changed default after the settled post-install check.

The user approved restoring the saved physical default and restarting oFono once, confirming the call ended. Separate fresh private `ready/idle` and zero-HFP checks preceded both actions. Post-checks confirm the physical default restored, services healthy, and daemon `ready/idle`; the call-only rule remains installed. A read-only graph watcher was started before requesting another user-operated call so the actual active routing can be captured. Live noise-reduction verification is pending.

The watcher then captured an active call with one HFP stream in each direction, two active physical→outgoing-HFP links, two incoming-HFP→output links, zero RNNoise→HFP links, and the physical global default retained. The outgoing HFP node lacked `target.object` and identified its factory as `api.bluez5.sco.sink`; incoming used `api.bluez5.sco.source`. This establishes an actual filter bypass in addition to the earlier separate SCO failure. The user reported noise filtering still failed and was told they could end the call after capture.

The rule used `media.class`, which becomes available after the node-creation rule stage. The corrected selector uses HFP profile plus outgoing SCO `factory.name`, assigned before matching in the installed WirePlumber `bluez.lua`. A native matcher regression with creation-time properties and no `media.class` fails with the old selector and passes with the correction; all eleven tests pass. The user approved applying the corrected test. The corrected fragment is installed with a durable backup; WirePlumber followed by oFono was restarted behind separate fresh private idle/zero-HFP checks. Both services are active, the daemon is `ready/idle`, and the original physical global default is preserved. During the next user-operated active call, the read-only graph confirmed two physical→RNNoise links, one RNNoise→outgoing-HFP link, zero direct physical→HFP links, two incoming-HFP→output links, the correct outgoing target, and a physical global default. The corrected call-only route is now verified on this machine. The user confirmed both directions work and noise is reduced. This verifies audible call-only filtering for this test call; no audio was recorded. Filter-absent fallback and physical-device switching remain unverified.

## 2026-09-28 UI-only prototype boundary

The `gnome-frontend-update` branch adds a standalone GTK 4/libadwaita mock frontend under `src/mock-ui/`. Its connection, contact, call, audio-device, and popup states are simulated. Passing its UI tests demonstrates only interface behavior; it adds no hardware evidence and does not change the installed daemon, oFono, BlueZ, or PipeWire route. Real backend wiring and live call acceptance remain deferred.

The mock popup requests top-right, above-window placement on X11. Wayland falls back to an ordinary GTK window; neither path has yet been checked under the user's live GNOME Shell. The virtual-display interaction checks validate widget/state behavior only.

Observed on 2026-09-24 through read-only system inspection during a user-operated call.

## Existing route

The user reports: answer on iPhone → select ASUS in the iPhone audio menu → hear the call through laptop-connected headphones. On the live call below, the user confirmed the other person could hear the microphone selected by Ubuntu. This is user-reported two-way audio, supported by the observed graph links; it is not an agent listening test. Preserve this route while testing; no Bluetooth or audio configuration has been changed here.

## Live route snapshot

During a user-routed call on 2026-09-24, read-only inspection observed:

- The iPhone connected to BlueZ and its PipeWire Bluetooth card present.
- PipeWire Bluetooth card profile `audio-gateway` (`Audio Gateway (A2DP Source & HSP/HFP AG)`); a live Bluetooth stream identified itself as `headset-audio-gateway` using the `msbc` codec.
- Active links from the selected C-Media capture source to the Bluetooth output stream, and from the Bluetooth input stream to the selected C-Media playback sink. These links establish the two graph directions at that moment; they do not independently prove audibility at either end.
- No `org.ofono` service on the system bus during the call.

The Bluetooth call streams disappeared before the next graph snapshot while the iPhone remained Bluetooth-connected. The user says they manually switched call audio from the iPhone; the exact destination of that later switch is being confirmed. No call was initiated or controlled by the agent, and no configuration was changed. Default-device movement and laptop-initiated handoff in both directions remain untested. The observed native PipeWire route should be investigated before considering oFono.

The [PipeWire 1.0.5 native backend source](https://raw.githubusercontent.com/PipeWire/pipewire/1.0.5/spa/plugins/bluez5/backend-native.c) registers HFP profiles with BlueZ and manages RFCOMM/SCO internally. Combined with the live profile and absent `org.ofono`, this suggests the native backend currently owns HFP; ownership has not been independently traced on D-Bus. Source inspection found call indicators handled inside that backend but no application-facing call-control method there. This is a source-based inference, not a runtime proof that another control API is unavailable. Enabling a second HFP backend before resolving ownership could disrupt the working route.

## Setup before the live call

- Ubuntu 24.04, GNOME/X11 per the specification; BlueZ 5.72, PipeWire 1.0.5, WirePlumber 0.4.17.
- Bluetooth adapter powered; iPhone paired; no device listed as connected by `bluetoothctl devices Connected` during the initial preflight. The iPhone connected for the later live call and remained connected after its call streams disappeared.
- `org.bluez` is running. No `org.ofono` service was present on the system bus. oFono is not installed.
- The installed WirePlumber 0.4 Bluetooth configuration shows `bluez5.hfphsp-backend = "native"` only as a commented example; no explicit backend override was found under `/etc/wireplumber` or `/usr/share/wireplumber`.
- PipeWire and WirePlumber user services active. The selected sink and source were C-Media(R) Audio Analog Stereo. No Bluetooth audio device or call stream appeared in the initial `wpctl status`; call streams appeared during the live snapshot above.
- A project-local Rust 1.98.1 toolchain exists under `.tools/`. GTK4 4.14.5, libadwaita 1.5.0, PipeWire 1.0.5, and SQLite 3.45.1 development metadata are available.

## Feasibility gate still open

No agent has placed or received a call, operated an iPhone audio selector, initiated a laptop-side transfer, or checked DTMF. No desktop popup, DND, lock-screen, or notification-center behavior has been tested. V07/V10 now have user-reported two-way audio and phone-side routing plus the live graph snapshot above; application acceptance remains pending. Other corresponding acceptance cases remain **Not tested**.

The temporary experiment below exposed oFono's call-control API but could not test a real call. At the next coordinated call-free window, repeat the controlled setup and first retest the original phone-controlled ASUS route under oFono. Then test both laptop-initiated handoff directions and the remaining gate in section 11 of [the specification](IBlueConnect_Spec.md). Any renewed profile-owner or system audio change must remain reversible because it can disrupt the working route.

## Controlled backend experiment

The package index offers oFono 1.31, and the installed WirePlumber 0.4 configuration documents `ofono` as an HFP backend option. [PipeWire 1.0.5's oFono backend](https://raw.githubusercontent.com/PipeWire/pipewire/1.0.5/spa/plugins/bluez5/backend-ofono.c) has audio acquire/release support. These are compatibility clues, not proof of the required iPhone handoffs.

The approved live plan used a call-free window: install oFono, apply a reversible user WirePlumber 0.4 override selecting `ofono`, restart the affected service, and inspect modem and audio state. A user-operated call would first retest the phone-controlled ASUS route, followed by call control and handoff only if the baseline survived.

### Sandbox preparation completed

With the user's approval for a sandboxed experiment, Ubuntu's `ofono_1.31-3ubuntu3.24.04.2_amd64.deb` was downloaded and extracted under `/tmp/iblueconnect-ofono/` first. The extracted binary contains `plugins/hfp_hf_bluez5.c` and oFono call interface names. Its package `postinst` enables and starts both `ofono.service` and `dundee.service` during a normal installation, so a live test could not remain workspace-only. The user then explicitly permitted a temporary live session.

### Temporary live session and rollback

- An `apt-get` simulation showed one new package, `ofono`, and no other package changes. It was installed temporarily after the user confirmed no active call. The unrelated `dundee.service` was stopped.
- oFono initially logged `RegisterProfile() ... UUID already registered`, because the existing native backend still owned HFP when the package auto-started. A temporary user WirePlumber Lua override selected `ofono`; WirePlumber was restarted, then oFono was restarted to retry registration.
- After this ordering, the connected iPhone appeared as an online, powered oFono HFP modem exposing `org.ofono.VoiceCallManager`, with a hands-free audio card visible to `HandsfreeAudioManager`. `GetCalls()` returned an empty list. The selected Ubuntu sink and source remained C-Media.
- No user-operated test call became available during the temporary session. Phone-side ASUS routing, two-way audio under oFono, laptop Answer/Decline/Dial, DTMF, and both laptop-initiated handoffs remain **Not tested** under this backend.
- Rollback removed only the temporary Lua override and its empty directories, stopped oFono, restarted WirePlumber, and purged only the added `ofono` package. Verification found PipeWire/WirePlumber active, oFono/dundee services and `org.ofono` absent, the iPhone Bluetooth-connected, and the C-Media sink/source still selected.

### Native route after rollback

On a subsequent user-operated call, read-only inspection again found both HFP stream directions linked to C-Media capture/playback, with the `audio-gateway` card profile, `headset-audio-gateway` stream profile, and `msbc` codec. The user confirmed both sides could hear and that they ended this call manually on the iPhone. The observed graph route and user-reported two-way audibility survived the temporary oFono experiment and rollback.

During another user-reported active call, the new Rust PipeWire snapshot parser passed its ignored live route test: it found one HFP playback stream linked to an output and one HFP capture stream linked from an input. This is a read-only graph check, not an oFono or laptop call-control test; the native HFP backend remained in place.

### Second temporary oFono setup and rollback

On 2026-09-24, after the user confirmed the previous call had ended and approved another attempt, preflight found no active HFP call streams, one Bluetooth-connected device, and the C-Media sink/source still selected. `apt-get -s install ofono` again listed only oFono. A private pre-change `wpctl` snapshot and rollback script were saved under `/tmp`.

oFono 1.31-3ubuntu3.24.04.2 was installed; the unrelated `dundee.service` started by its package was stopped. A temporary user WirePlumber 0.4 Lua override selected the `ofono` HFP backend, then WirePlumber and oFono were restarted. The iPhone briefly disconnected during the switch. After a controlled reconnect of the already paired iPhone, oFono exposed one modem with `org.ofono.VoiceCallManager` and one `HandsfreeAudioManager` card. The Rust daemon's session D-Bus status returned `ready`. PipeWire and WirePlumber were active, both C-Media defaults stayed selected, and no HFP call stream opened before a call.

No user-operated test call appeared in oFono during this session. Phone-side ASUS selection, two-way audio under oFono, call control, DTMF, and handoff therefore remain **not tested**. To avoid leaving the known working route on an unverified backend, the daemon and oFono were stopped, the temporary override removed, oFono purged, and WirePlumber restarted. Post-rollback checks found PipeWire/WirePlumber and Bluetooth active, oFono/dundee and the daemon inactive, one iPhone Bluetooth-connected, and the same two C-Media defaults selected. A new live call was not used to recheck the native route after this rollback.

### Coordinated oFono call-audio test

Later on 2026-09-24, the user confirmed another call had ended and permitted the temporary oFono setup again. With no active HFP streams, oFono 1.31-3ubuntu3.24.04.2 was installed, `dundee.service` stopped, the same user-only WirePlumber 0.4 override applied, and WirePlumber/oFono restarted. The paired iPhone needed one controlled reconnect after the backend switch. oFono then exposed a ready call-capable modem, the Rust daemon returned `ready`, and the two C-Media defaults remained selected.

During the user-operated test call, the iPhone's audio selector offered ASUS. The user selected it and reported that both sides could hear each other. The daemon observed an **active** call with **laptop** audio, and the Rust read-only `audio::tests::live_route_has_playback_and_capture` check passed under oFono, confirming active PipeWire playback and capture links from the same Bluetooth device. After the user ended the call, the daemon returned to idle. This validates the phone-selected ASUS route and two-way audio under this oFono setup. Laptop Answer/Decline/Dial, DTMF, default-device changes, and laptop-initiated handoff remain untested.

### Incoming laptop Answer attempt

With oFono still active and the screen confirmed unlocked, the user arranged a separate incoming call. A bounded one-shot client saw the fresh incoming call but its daemon `Answer` request failed; a following `Snapshot` timed out. The user answered on the iPhone, selected ASUS, and could hear the caller through the laptop, but the caller could not hear the user. The user ended the call. No PipeWire graph was saved during this second call, so its missing microphone direction cannot yet be attributed to a particular stream, source, or backend transition. The earlier successful call's ten saved graph snapshots all show an active unmuted capture path; they do not establish the path during this failure.

After the call ended, the temporary oFono override was removed, oFono purged, and WirePlumber restarted. PipeWire/WirePlumber were active, oFono/dundee and the override absent, and the C-Media sink/source stayed selected. The known native route is restored at the configuration level; a fresh native call has not been made after this rollback.

A private D-Bus regression test reproduced the Answer timeout with the daemon serving the request and checking GNOME lock state through the same session connection. A separate connection for the lock query made the test pass. The one-way audio failure needs a graph snapshot during a future call if it recurs before its cause can be established.

### Incoming Answer retest after the D-Bus fix

After the user approved another temporary session, the same oFono 1.31 and WirePlumber 0.4 setup was enabled with no active HFP stream. The patched Rust daemon was installed, ready and idle; GNOME reported the screen unlocked and both C-Media defaults stayed selected. The user arranged an incoming call. The one-shot test watcher immediately sent laptop `Answer` on the first fresh incoming call, that request succeeded, and the daemon observed `active/laptop`. The user reported that ASUS became the iPhone audio route automatically and both sides could hear each other. A live PipeWire snapshot showed one running HFP playback stream linked to the selected output and one running HFP capture stream linked from the selected input; source and HFP streams were unmuted. The automatic answer came from this temporary test watcher, not from the installed app's normal behavior.

After the user confirmed the call ended, the temporary override was removed, oFono purged, and WirePlumber restarted. PipeWire/WirePlumber were active, oFono/dundee and the override absent, both C-Media defaults still selected, and the already paired iPhone was reconnected after it dropped during rollback. The installed patched daemon remains enabled for login but stopped without oFono. This validates laptop Answer and duplex audio for this test call; the earlier one-way call remains unexplained because no graph was captured during that failure. A permanent oFono backend choice, handoff in either direction, DTMF, and the remaining acceptance cases are still open.

## Permanent oFono backend (2026-09-24, later)

The user approved making oFono the permanent HFP backend. Preflight found no active HFP stream, oFono not installed, no WirePlumber override, and DISPLAY/XAUTHORITY present in the systemd user environment (needed for the daemon-spawned popup). `apt-get -s install ofono` again listed only `ofono` 1.31-3ubuntu3.24.04.2.

[packaging/ofono-enable.sh](packaging/ofono-enable.sh) then installed oFono, disabled and stopped `dundee.service`, wrote the user override `~/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua` (`bluez5.hfphsp-backend = "ofono"`), restarted WirePlumber, enabled and restarted oFono, and restarted the daemon. The script refuses to run while an HFP call stream is active and never reconnects the phone. The package's first auto-start logged one "UUID already registered" line (native HFP still owned the profile, as in the earlier temporary sessions); the restart after WirePlumber released HFP logged no errors. Afterwards oFono was active and enabled, dundee inactive and disabled, PipeWire/WirePlumber/daemon active, and both C-Media defaults still selected. The iPhone was not connected at that moment; the user reconnected it manually. [packaging/ofono-rollback.sh](packaging/ofono-rollback.sh) restores the native backend; it has not been run since the permanent switch.

Expected side effect, not tested: while oFono owns HFP, Bluetooth headset microphones (HFP/HSP headset role) are likely unusable; A2DP playback and wired audio are unaffected.

## Application call control under the permanent backend

With the user connecting the iPhone manually, the daemon reported `ready`. The user placed an outgoing call from the IBlueConnect main window; the daemon observed it (`alerting`, laptop audio) and the user confirmed the call worked. Incoming-call popup behavior (Answer/Decline/Ignore, un-pinning after answer, finding a minimized popup), both audio directions under this setup, DTMF, and handoff remain **not tested** on a real call. No agent placed or answered a call.

## Contact sync over PBAP

PBAP runs through BlueZ's obexd (user service `obex.service`, D-Bus activatable as `org.bluez.obex`) over the already-connected phone; the application never initiates the Bluetooth link. The phone address is derived from the oFono HFP modem path (`/hfp/org/bluez/hci0/dev_XX_…`), whose format was confirmed on the live modem.

- First attempt: `CreateSession(PBAP)` failed with `OBEX Connect failed with 0x41` (Unauthorized) — the iPhone refused because **Sync Contacts** was off for this laptop.
- After the user enabled Settings → Bluetooth → ⓘ → **Sync Contacts** on the iPhone, `Select("int","pb")` + `PullAll` (vCard 3.0) succeeded: 904 phone numbers cached, 0 malformed (378 Mobile, 512 Phone, 11 Home, 3 Work; 3 nameless entries shown by number).
- obexd removes a session as soon as the D-Bus client that created it disconnects, so separate `gdbus` commands cannot probe PBAP; probes must run inside one long-lived process.

## Call-history sync over PBAP

An opt-in counts-only probe (`pbap::tests::live_call_history_probe`) pulled `cch` from the iPhone: 100 vCards, all carrying `X-IRMC-CALL-DATETIME` with a MISSED/RECEIVED/DIALED tag (25/44/31). iOS appears to cap PBAP call history at the latest 100 calls. Per-book `mch`/`ich`/`och` were not probed separately.

The daemon now pulls `cch` after `pb` in the same sync; each fails independently. Merged with the daemon's locally observed history, Recents showed 102 entries covering the last 5 days (31 dialed, 25 missed, 46 received). The first merge showed two local/phone duplicates because the two sources wrote the same numbers in different forms (`+20…` vs `0…`); matching on the last nine digits removed them and let 4 more entries pick up contact names (2 remain unnamed). The two remaining same-number pairs within five minutes were verified to be distinct calls (different kinds, 90 s and 267 s apart). Phone timestamps are assumed to be in the laptop's local time zone; this was not independently verified.

## Still open

Real incoming-call acceptance of the popup, two-way audio and DTMF under the permanent backend, default-device changes, both laptop-initiated handoff directions, lock/DND popup behavior, missed-call notifications, and the rest of the specification's acceptance matrix. FaceTime Audio cannot be started over HFP (cellular only) and is outside V1.

## Audio quality baseline on the permanent oFono backend

During a user-operated cellular call routed to the laptop, the user described the incoming voice as muffled and pixelated. The user's own voice was clear to the other person, but keyboard and fan noise were conspicuous compared with Discord using the same C-Media input and output. The incoming voice was clearer when the user manually moved that call's audio to the iPhone. No agent placed or controlled the call.

A read-only PipeWire probe found one running HFP playback stream and one running HFP capture stream, both unmuted and using `cvsd`. Three consecutive graph snapshots retained active links in both directions, and the existing live route test passed. The selected C-Media input was at 92% and output at 40%, neither muted. These observations do not exclude intermittent transport errors or explain the full voice-quality difference, but they make an absent capture link or simple mute unlikely during this test.

At baseline, the WirePlumber 0.4 oFono override set only `bluez5.hfphsp-backend = "ofono"`; it did not set `bluez5.enable-msbc`. PipeWire 1.0.5's oFono backend defaults its mSBC support flag to false when that property is absent and registers only CVSD with oFono. Earlier native-backend calls used `msbc`.

The user approved a reversible mSBC trial. After a fresh private daemon Snapshot reported `idle` and the graph had zero HFP call streams, the previous override was backed up under `/tmp`, `bluez5.enable-msbc = true` was added to the active override, and WirePlumber then oFono were restarted. The application daemon was not restarted and no Bluetooth connection was initiated. PipeWire, WirePlumber, and oFono were active afterward; the daemon reported `ready/idle` with at least one connected Bluetooth device.

On the next user-operated call, the iPhone's audio selector stayed on ASUS but the laptop had no sound. The daemon initially reported `ready/active/unknown`; PipeWire had one Bluetooth audio-gateway card but **zero HFP audio streams**, despite all three audio services being active. The user moved audio to the iPhone and confirmed that audio worked there. This mSBC trial did not establish a usable laptop route.

The daemon later showed phase `idle` with status `manage calls on phone` while the user confirmed the cellular call was still connected. In the old installed code, unsupported/multiple oFono call states became `Observation::Unavailable`, which `public_snapshot` mapped to phase `idle`; therefore `idle` alone did not prove the cellular call had ended. A separate user message said the call had ended, and the original override was restored after another `idle` Snapshot and zero HFP streams; a later reply clarified that the call was still connected. Thus WirePlumber and oFono were restarted during that phone call. The user reported that iPhone audio still worked afterward. No further service changes were made during the call. Future service changes must check both status and phase and resolve any conflict with the user's report before restarting. The original override exactly matches its pre-trial backup, and the repo enable script no longer sets mSBC.

The user later confirmed the cellular call ended. The Rust/GTK fix now reports phase `unavailable` instead of `idle` for this unsupported state; 21 daemon tests and GTK checks passed. It was installed, and the application daemon was restarted only after a new private Snapshot reported `ready/idle` and PipeWire had zero HFP streams. The daemon remained active and reported `ready/idle` afterward. A subsequent user-operated call routed to ASUS produced two running CVSD HFP streams with active links in both directions, and the user confirmed that both sides could hear each other. This verifies the restored laptop audio route; the installed unavailable-state display has not yet been tested on a real unsupported call state. Background fan and keyboard noise on the outgoing microphone remains to address.

The user approved a temporary WebRTC microphone-filter trial after confirming the call had ended; a fresh private Snapshot was `ready/idle` with zero HFP streams. The installed PipeWire echo-cancel module was loaded in a short-lived `pw-cli` client with `monitor.mode=true`, its capture stream targeted to the selected C-Media source, and its WebRTC noise suppression enabled with gain control disabled. WirePlumber selected the virtual `echo-cancel-source` as the default input without a manual default change or service restart. During a user-operated ASUS-routed call, both CVSD HFP streams were running; two active links connected C-Media to the filter and two connected the filter to the outgoing HFP stream. The user reported **no clear improvement** in fan/keyboard noise. After the user confirmed the call ended and a fresh Snapshot was `ready/idle` with zero HFP streams, the temporary client was stopped. The filter source disappeared and the default input returned to C-Media. No permanent filter configuration was installed.

The user then approved an RNNoise trial. RNNoise v1.10's LADSPA plugin was built under `/tmp` without installing packages or changing services. A short-lived PipeWire filter-chain client exposed one mono/48 kHz virtual source and took input from C-Media. On a user-operated ASUS-routed call, two running CVSD HFP streams were present, with two active C-Media → filter links, one active filter → outgoing HFP link, and two HFP → playback links. The user reported less keyboard/fan noise and clear outgoing speech; the incoming voice remained muffled. The inline microphone was on the headset cable. The user chose to keep RNNoise.

The durable user-level setup is [packaging/rnnoise.sh](packaging/rnnoise.sh): `enable <LADSPA .so>` copies the plugin into `~/.local/lib/iblueconnect/`, writes one `~/.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf`, and restarts only the already-enabled `filter-chain.service`; `rollback` removes the fragment/plugin and restarts that service. Each restart requires a fresh private Snapshot with status `ready`, phase `idle`, and zero HFP streams. Enable also requires a physical-source link and the virtual mic as the selected default after restart, otherwise it attempts a guarded rollback. Its mock offline test and shell syntax check pass. The temporary RNNoise client was stopped after the user confirmed call end and another fresh idle/zero-stream check; durable enable then succeeded. Post-enable read-only inspection found one RNNoise source selected, two C-Media → filter links, no HFP streams, and active PipeWire, WirePlumber, filter-chain, and app-daemon services. The app daemon's virtual-source route verification was installed and restarted after a separate fresh idle check. Changing the selected physical input while the filter is active remains untested; subsequent call results follow.

## Durable RNNoise call check (2026-09-28)

Two subsequent user-operated calls had no audio in either direction. During the second, the daemon reported `ready/active/unknown` and PipeWire exposed zero HFP playback and capture streams. WirePlumber logged one SCO transport failure with `Transport endpoint is not connected`; the Bluetooth service had no matching error message in that window. The selected RNNoise virtual source still existed, but it had no HFP stream to feed. The call ended before further live device inspection. A later private Snapshot reported `ready/idle` with zero HFP streams and all audio services active. No audio or Bluetooth service was changed. The durable setup is unverified for live audio; SCO establishment is the immediate failure to investigate.

The user approved restarting WirePlumber and oFono to reset this failed SCO state. A fresh private `ready/idle` Snapshot and zero-HFP graph preceded each restart. Post-restart, PipeWire, WirePlumber, filter-chain, oFono, and the app daemon were active; the daemon was `ready/idle`, RNNoise remained the default source, and two physical-source links fed its capture node. No configuration changed. The next user-operated call formed one HFP playback and one HFP capture stream, with two active HFP-to-output links, one RNNoise-to-HFP link, and two physical-input-to-RNNoise links. The user confirmed clear two-way audibility. After the call, the daemon returned to `ready/idle` and both HFP streams cleared. The durable RNNoise setup now has one successful live duplex call; the user reports less fan and keyboard noise with clear speech.
