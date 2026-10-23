# IBlueConnect

IBlueConnect lets an iPhone use an Ubuntu laptop as a hands-free endpoint for cellular calls. It runs a small Rust daemon in the background and opens its GTK 4/libadwaita window only from the application icon. The user connects the iPhone to the laptop in Bluetooth settings; IBlueConnect never initiates that Bluetooth link.

Development began on Ubuntu 24.04 GNOME/X11; the current laptop runs Ubuntu 26.04.1. The app uses PipeWire, WirePlumber, BlueZ, oFono, and a system-selected wired/USB headset or microphone. The corrected two-way call and call-only RNNoise route have been verified on this upgraded laptop. Both native packages build, pass offline checks, and install in isolated Ubuntu environments. Fresh-laptop Bluetooth calls remain unverified, including Ubuntu 26.04's native oFono 2.x. This is a **preview**, not completed V1 acceptance. Contacts and recent calls stay in user-owned local files.

## Ubuntu versions and releases

Maintain one `main` branch with shared source and compatibility code selected by installed capabilities. Use short-lived feature/fix branches; an optional maintenance branch such as `release/1.x` is for an older application release receiving backported fixes. A separate long-lived branch for each Ubuntu version would duplicate changes unnecessarily here.

One application release provides two native installers. The [release workflow](.github/workflows/release.yml) builds and checks each Ubuntu environment, installs the package in isolation, measures mock UI memory, and publishes both packages under the same preview tag. Source fixes are shared; each installer links against its target OS libraries.

Use a release tag such as `v0.1.0-preview.1`, then attach the separate `.deb` installers, checksums, metadata, memory measurements, and corresponding RNNoise plugin source. CI cannot establish Bluetooth audio quality, phone permissions, or real GNOME behavior; those require user-operated calls on each target.

| Environment | Current evidence |
|---|---|
| Ubuntu 24.04 amd64 | Native build, offline GTK/Rust/setup tests, and isolated package installation pass. WirePlumber 0.4 uses Lua configuration. Earlier phone tests exist; the packaged first-run setup still needs a fresh desktop/phone test. |
| Ubuntu 26.04 / 26.04.1 amd64 | Native build, offline tests, and isolated package installation pass. WirePlumber 0.5 uses SPA-JSON. Live two-way RNNoise audio works on the upgraded laptop with retained oFono 1.31; native oFono 2.18 still needs a phone test. |
| Android/other phones | Not supported by this preview. The backend uses HFP/PBAP capabilities rather than an Apple-only check, but calling, sync, and permissions need device tests. |

Do not install the 26.04 package on 24.04, or retain it after an OS downgrade. Reinstall the matching package after an OS change. ARM, other Ubuntu releases, other Linux distributions, and non-GNOME desktop acceptance are outside this preview.

## Run the UI with live or mock data

The installed UI is built from `src/mock-ui/` and follows the real daemon by default. Its explicit developer mode uses simulated data, allowing interface checks without a phone or real calls:

```sh
sh packaging/build-mock-ui.sh
./target/iblueconnect-mock --developer
```

In developer mode, the separate **IBlueConnect Mock Controls** window simulates incoming, outgoing, connected, disconnected, and error states. You can close the main window while using the mock floating call controller. Run `sh packaging/test-mock-ui.sh` for headless interaction checks, including offline live-data mapping checks.

The mock UI follows the system light/dark appearance; its developer controls can preview either appearance without changing GNOME settings. Press Ctrl+Q or **Quit Mock App** to stop it.

Live integration status:

| Feature | Current implementation |
|---|---|
| Floating incoming/active controller | Follows daemon call state; it also opens (silently) when the phone connects mid-call. Incoming calls launch the controller in background mode with equal-width Ignore, Decline (red) and Answer (green) pills, sized to its content. The active controller matches the voice strip: one row of soft tiles for Mute ("Mute"/"Muted"), Audio, Keypad and, when available, Hold/Resume, Merge, Swap and Add call, a compact toggle, and a wide red End pill; the window widens to fit rather than adding rows. |
| Dialing, mute, in-call keypad, hold/resume, call waiting, swap, add/merge/end calls, conference management | Connected to daemon operations; full live acceptance is still pending for several controls. |
| Device status, contacts, recents, battery, voice strip | Connected to daemon data. Apple-inspired bar strip: your voice (system accent) from the left, the caller (green) from the right, avatar ring shows who speaks. Your side is flat while muted; it does not confirm remote audibility. |
| Siri over Bluetooth | "Ask Siri" button in its own row under the connection card starts Siri on the iPhone (HFP voice recognition); speak and hear Siri through the laptop, e.g. "FaceTime audio Mom". Needs a ready link, no calls, unlocked screen. Live behavior pending. |
| Appearance | System / Light / Dark / Black in the phone menu (Black is GNOME Dark with near-black surfaces); saved, and the call controller always matches the main window. |
| Recents | iPhone-style rows: tap a person to call back; centred separator lines (90% of width); latest 50 calls with "Show more". Planned: "!" button showing all recent calls with that person; virtualized lists for large phone books. |
| Lock privacy and guarded exit controls | Installed: private windows hide while the screen is locked. Quit, Disconnect, and confirmed Forget require fresh idle state; active or uncertain calls block exit because automatic audio handoff is unavailable. |
| Audio transfer and preferences | Laptop/phone transfer is not implemented. Only the appearance choice is saved; other preference switches exist only in developer mode and are not persisted. |

X11 can place the mock call panel near the top-right and request above-window visibility. Wayland uses a normal GTK window because GNOME does not give applications reliable absolute placement. Headless UI tests do not establish live GNOME Shell behavior.

## How it fits together

```text
iPhone -- Bluetooth/BlueZ -- oFono (HFP call control)
                                   |
                              Rust daemon
                      call state, PBAP sync, history
                        |                 |
                 session D-Bus       floating call controller
                        |
                  GTK/libadwaita UI

iPhone call audio -- PipeWire/WirePlumber -- Ubuntu-selected input/output
                                 |
                      optional RNNoise mic filter
```

The daemon owns call state and publishes a snapshot and change signals over session D-Bus. The UI shows Recents, Keypad, and Contacts; closing it leaves the daemon running. PBAP sync uses BlueZ OBEX over an already-connected phone. Contacts and synced phone call history share a private SQLite cache; locally observed call history is stored separately. The optional RNNoise filter processes outgoing microphone audio through PipeWire. It does not process incoming call audio.

## Repository map

| Path | Purpose |
|---|---|
| `src/bin/iblueconnectd.rs` | Background daemon, session D-Bus API, sync and event coordination |
| `src/iblueconnect-ui.c` | Older UI retained as a rollback |
| `src/mock-ui/` | Installed GTK 4/libadwaita UI, live daemon binding, and explicit mock/developer modes |
| `src/ofono.rs`, `src/backend.rs`, `src/call.rs` | oFono integration and call state |
| `src/audio.rs`, `src/presentation.rs`, `src/bin/iblueconnectd.rs` | Audio-route observation, call mute, and incoming-call presentation policy/execution |
| `src/pbap.rs`, `src/vcard.rs` | OBEX/PBAP phonebook and call-history parsing |
| `src/contacts_store.rs`, `src/history*.rs`, `src/call_log.rs` | Private cache, local recents, and merge logic |
| `packaging/install-user.sh` | Build and install the user daemon, UI, service, and desktop launcher |
| `packaging/build-mock-ui.sh`, `packaging/test-mock-ui.sh` | Build and check the installed frontend and developer mode |
| `packaging/ofono-*.sh` | Enable the approved HFP backend or restore the native backend |
| `packaging/build-rnnoise.sh`, `packaging/rnnoise.sh` | Build and separately enable or roll back microphone noise suppression |
| `packaging/systemd/`, `packaging/desktop/` | Login service and application launcher |
| `IBlueConnect_Spec.md`, `Feasibility.md`, `HANDOFF.md` | V1 requirements, hardware findings, and latest project state |

## Download and install the preview

Release: **`v0.1.0-preview.1`**. Publication is pending; the links below are the intended GitHub Release assets and will work after publication. If this repository is private, sign in with repository access.

| Your Ubuntu version | Matching amd64 installer | Download | Installed app files |
|---|---|---:|---:|
| 24.04 / 24.04.x | [Ubuntu 24.04 installer](https://github.com/marwanelhayes/IBlueConnect/releases/download/v0.1.0-preview.1/iblueconnect_0.1.0-1ubuntu24.04_amd64.deb) | 5.87 MiB (6.16 MB) | 9.47 MiB |
| 26.04 / 26.04.x | [Ubuntu 26.04 installer](https://github.com/marwanelhayes/IBlueConnect/releases/download/v0.1.0-preview.1/iblueconnect_0.1.0-1ubuntu26.04_amd64.deb) | 5.88 MiB (6.16 MB) | 9.48 MiB |

These sizes cover the application, daemon, setup, and RNNoise plugin. Missing Ubuntu dependencies require additional download/disk space; `apt` shows the amount before you confirm. Contact/history caches grow with your phone data. The accompanying 35.6 MiB plugin source archive is for source availability and is **not needed to install**. Download checksums from the same release to verify the installer.

### First installation on another laptop

1. Use Ubuntu GNOME with Bluetooth, PipeWire/WirePlumber, and a working physical wired/USB microphone and output. Check **Settings → About** for the Ubuntu version; `dpkg --print-architecture` must say `amd64`.
2. End all phone calls. Disconnect **all Bluetooth devices**, including phones and headsets, manually in Settings. Select the physical microphone in **Settings → Sound → Input**.
3. Download only the installer matching your Ubuntu version. In a terminal opened in its download folder, run **one** matching command:

   ```sh
   # Ubuntu 24.04 only:
   sudo apt install ./iblueconnect_0.1.0-1ubuntu24.04_amd64.deb

   # Ubuntu 26.04 only:
   sudo apt install ./iblueconnect_0.1.0-1ubuntu26.04_amd64.deb
   ```

4. Open **IBlueConnect** from the app menu. Read the first-run permission dialog and choose **Allow and set up**. Authenticate if Ubuntu requests it. Cancel leaves app setup unapplied. Installing missing Ubuntu dependencies can start their own services; first-run audio setup can briefly interrupt sound.
5. Setup backs up existing app-managed settings, enables the quiet background daemon at login, configures oFono and version-appropriate call routing, and starts RNNoise. Contacts/history sync is included in this consent; there is currently no independent sync opt-out switch. Existing custom audio configuration causes setup to stop for manual review.
6. Connect the iPhone **manually** in Bluetooth settings. Enable **Sync Contacts** for this laptop on the iPhone and allow sharing when prompted. The app never initiates this Bluetooth link or answers calls automatically.
7. Arrange a test cellular call yourself. Select the laptop on the iPhone if necessary; verify that both people can hear and that the other person hears less background noise. Keep Ubuntu's default input on the physical mic: the filter is applied only to outgoing HFP phone-call audio. It does not clean the other person's incoming voice.

Closing the main window leaves the daemon running quietly. Incoming calls show the floating controller; phone-synced missed calls create no popup, sound, or notification. Bluetooth headset microphones are outside this preview's supported setup; the oFono backend can affect their HFP availability.

**Existing source installations:** this package refuses to replace a service in `~/.config/systemd/user/iblueconnectd.service`; migration requires manual review. For an app/OS upgrade, end calls, stop the old daemon with `systemctl --user stop iblueconnectd.service`, disconnect Bluetooth devices, install the matching package, and open the app for setup again. No automatic OS downgrade migration is promised.

**Desktop/privacy limits:** X11 supports controller positioning; Wayland uses a normal GTK window without guaranteed placement/stacking. Full GNOME/Wayland acceptance remains pending. The October 23 source update adds fail-closed lock privacy for the main window, controller, and app dialogs, with private D-Bus tests; installed on the development laptop but not live-verified yet. Earlier installed binaries retain the lock privacy limitation.

### Expected RAM

Measured daemon + UI memory from the **installed native packages**, with mock contacts/calls in isolated D-Bus/Xvfb sessions, Cairo rendering, and five samples per state. PSS counts shared memory proportionally; RSS counts it in full. These are observations, not maximum limits or live-call measurements.

| State | Ubuntu 24.04: PSS / RSS | Ubuntu 26.04: PSS / RSS |
|---|---:|---:|
| Background daemon, window closed, no phone | 5.3 / 7.1 MiB | 5.3 / 7.2 MiB |
| Daemon + main window | 100.0 / 123.2 MiB | 114.6 / 143.1 MiB |
| Daemon + active-call preview controller | 87.2 / 110.0 MiB | 100.7 / 129.0 MiB |

On the current upgraded 26.04.1 laptop, idle with the UI closed, the daemon used 8.5 MiB PSS / 12.1 MiB RSS; RNNoise's filter service used 8.8 / 15.6 MiB, and retained oFono 1.31 used 2.5 / 6.5 MiB. Together that is about **20 MiB PSS / 34 MiB RSS**. This excludes GNOME and existing PipeWire, WirePlumber, and BlueZ services. Phone data, rendering, concurrent windows, and active calls can change usage; allow roughly **150–200 MiB** for app/UI/helpers as an initial planning estimate, with no established upper bound.

The spec's under-80 MB open-window target is **not met in these isolated measurements**. CPU/latency targets and full live-call resource budgets remain unverified. See [measurement samples](packaging/resource-measurements.json) and [measurement script](packaging/measure-memory.py).

### Undo and uninstall

1. End all calls. Only after call end, stop the daemon:

   ```sh
   systemctl --user stop iblueconnectd.service
   ```

2. Disconnect all Bluetooth devices manually, then restore the pre-setup configuration and service state:

   ```sh
   sh /usr/lib/iblueconnect/first-run.sh --undo
   ```

3. **Only after Undo succeeds**, remove the app:

   ```sh
   sudo apt remove iblueconnect
   ```

If Undo pauses, retain the package, finish Undo, and retry removal. Undo preserves local contacts/history and backups. It does not connect the phone. Removing the `.deb` alone does not undo consented audio settings or delete private caches.

## Developer/source installation

The installer needs a Rust toolchain with the project's crates available offline, a C compiler, `pkg-config`, GTK 4/libadwaita/X11 development files, and the GNOME/PipeWire/BlueZ desktop services. From the repository root:

```sh
sh packaging/install-user.sh
systemctl --user start iblueconnectd.service
```

This installs to `~/.local/bin`, adds a systemd user service and desktop launcher, and enables the daemon for the next login. oFono is the user-approved HFP backend. Enable it only when no call is active; this script changes the audio backend and can briefly interrupt laptop audio:

```sh
sh packaging/ofono-enable.sh
```

The source backend setup needs the running daemon for its private idle check. It selects legacy Lua configuration for WirePlumber 0.4 and SPA-JSON for 0.5 and later. It stops if call state cannot be verified and rechecks before service changes. These source scripts require developer supervision for initial bootstrap; the preview `.deb` uses its separate first-run setup.

Connect the iPhone **yourself** from Bluetooth settings, then open IBlueConnect from the application menu. If oFono is removed later, `sh packaging/ofono-rollback.sh` restores the native backend; the script stops the IBlueConnect daemon. It does not reconnect the phone.

## Optional microphone noise suppression

RNNoise is bundled and configured by the preview `.deb` first-run setup. For **source installations only**, build and enable it separately as below. The current laptop has one verified two-way call with less fan/keyboard noise; device changes and fresh-desktop acceptance remain pending. First install/start the source daemon, connect the phone manually, and wait until the call is idle:

```sh
sh packaging/build-rnnoise.sh
systemctl --user enable filter-chain.service
sh packaging/rnnoise.sh enable target/rnnoise/build/bin/ladspa/librnnoise_ladspa.so
```

The backend setup installs a call-only rule: physical microphone → RNNoise → outgoing phone audio. Keep Ubuntu's default input set to the physical microphone; other apps keep using it. On Ubuntu 26.04.1 with WirePlumber 0.5.13, a live graph confirmed this route with no direct microphone bypass, and the user confirmed working two-way audio and reduced noise. Filter-absent fallback and microphone switching still need live tests.

The build script downloads the pinned [noise-suppression-for-voice v1.10 source](https://github.com/werman/noise-suppression-for-voice/releases/tag/v1.10) and verifies its checksum. It needs `curl`, `sha256sum`, `tar`, CMake, and a C++ compiler. Pass a previously downloaded v1.10 archive to `build-rnnoise.sh` to build without that download. The enable script checks a fresh daemon snapshot and PipeWire graph before restarting the user filter service; it refuses to change the filter during a call. To remove the filter after the call ends:

```sh
sh packaging/rnnoise.sh rollback
```

The filter reduces noise from the laptop microphone. It does not address the incoming voice, which was reported muffled on the current CVSD call route.

## Checks

```sh
rust_bin="$PWD/.tools/rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"
RUSTC="$rust_bin/rustc" RUSTDOC="$rust_bin/rustdoc" CARGO_TARGET_DIR=/tmp/iblueconnect-target "$rust_bin/cargo" test --offline
cc -Wall -Wextra -DIBLUECONNECT_UI_TEST src/iblueconnect-ui.c -o /tmp/iblueconnect-ui-test $(pkg-config --cflags --libs gtk4 libadwaita-1)
/tmp/iblueconnect-ui-test
sh packaging/test-mock-ui.sh
sh packaging/test_build_rnnoise.sh
python3 -m venv .venv
.venv/bin/python -B packaging/test_rnnoise.py
.venv/bin/python -B packaging/test_ofono.py
.venv/bin/python -B packaging/test_setup.py
.venv/bin/python -B packaging/test_deb.py
```

Hardware acceptance requires user-operated calls. Tests and graph inspection alone do not prove that an iPhone call works.

## V1 status

The current machine has demonstrated app-originated calling, contact and call-history sync, and successful two-way call-only RNNoise filtering. This is a working milestone, not complete V1 acceptance. The Ubuntu upgrade configuration defect and filter-routing defect are fixed; automatic recovery from every transport failure is unproven.

**Unsupported scope:** FaceTime Audio is unavailable over this HFP link, and calls are not labeled as FaceTime (HFP reports no call type). Dual-SIM line choice is unavailable: HFP has no line selection, so the iPhone uses its default line or the contact's preferred line (set on the iPhone under the contact's line label). Android support, Bluetooth headset microphone support, multiple-phone selection, SMS/iMessage, contact editing, iCloud, screen mirroring, files/photos, and recording are not provided by this preview.

### Missing implementation

- Persisted preferences/startup controls (only the appearance choice is saved so far), separate optional sync consent, saved primary-phone selection, and standalone Clear call history. The packaged first run now requests combined background/audio/sync consent, checks dependencies/state, and keeps backups; sync starts automatically when the manually connected phone becomes ready. Guarded Forget is implemented and installed (October 23).
- App-operated phone/laptop audio transfer, actual input/output display, Open Sound settings, and exit during an ongoing call. New guarded Quit/Disconnect/Forget controls require fresh idle/zero-audio state and block active or uncertain calls; automatic handoff is unavailable. Disconnect/Forget pause monitoring until **Resume call monitoring** is chosen after manual connection. Automatic laptop-initiated Bluetooth connection remains excluded.
- Silent notification-center entries/actions for locally observed missed calls. The presentation policy exists, but the current daemon effect runner does not execute those notification effects. Phone-synced missed calls must remain completely silent and only appear in Recents.

### Implemented but needing further verification

- Live mute/reset/handoff behavior, remote IVR DTMF, Hold/Resume retest, and multi-call/conference controls.
- New outgoing waveform/incoming bar, GNOME lock hiding/restoration, guarded exit/disconnect, and confirmed cache removal. These source changes require installation and user-operated desktop/call verification.
- GNOME lock/DND behavior, one permitted alert, popup focus/keyboard behavior, and GUI crash/reopen behavior during a call. Some policy paths are tested offline; that does not establish full desktop acceptance.
- Input/output changes and unplug/replug during calls, live manual disconnect, RNNoise-absent fallback, and recovery after transport failure.
- Full accessibility/privacy acceptance and live resource/CPU targets. Native package memory measurements are above; the open-window memory target is not met in the isolated test.

See the [V1 acceptance matrix](IBlueConnect_Spec.md#13-acceptance-matrix) and [latest handoff](HANDOFF.md). The matrix's introductory "all other rows remain Not tested" statement predates later implementation and test records; use the dated evidence rather than treating that paragraph as current results.

## License

IBlueConnect is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (`GPL-3.0-or-later`). It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for the full text.

The packaged RNNoise plugin ([noise-suppression-for-voice](https://github.com/werman/noise-suppression-for-voice)) is GPL-3.0 with its own notices, shipped alongside the package.
