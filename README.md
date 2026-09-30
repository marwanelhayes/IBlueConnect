# IBlueConnect

IBlueConnect lets an iPhone use an Ubuntu laptop as a hands-free endpoint for cellular calls. It runs a small Rust daemon in the background and opens its GTK 4/libadwaita window only from the application icon. The user connects the iPhone to the laptop in Bluetooth settings; IBlueConnect never initiates that Bluetooth link.

The current target is Ubuntu 24.04 GNOME on X11 with PipeWire, WirePlumber, BlueZ, oFono, and a system-selected wired headset or microphone. Other devices and desktops have not been accepted as V1 targets. The app is local-only and stores synced contacts and recent calls in user-owned files.

## Explore the GNOME interface prototype

The separate mock frontend lets you test the proposed GNOME experience without connecting a phone or changing the installed calling setup. It does not use the daemon or place calls:

```sh
sh packaging/build-mock-ui.sh
./target/iblueconnect-mock --developer
```

The main window contains the proposed navigation and device states. The separate **IBlueConnect Mock Controls** window simulates incoming, outgoing, connected, disconnected, and error states. You can close the main window while using the mock floating call controller. Run `sh packaging/test-mock-ui.sh` for its headless interaction check.

The mock UI follows the system light/dark appearance; its developer controls can preview either appearance without changing GNOME settings. Press Ctrl+Q or **Quit Mock App** to stop it.

Backend work deferred from this prototype:

| Mock feature | Future integration |
|---|---|
| Floating incoming/outgoing/active controller | Connect to real call state and decide how it coexists with the installed GNOME notification flow. |
| Mute, audio choices, iPhone handoff, and in-call keypad | Bind to verified telephony and audio capabilities; current controls only change mock state. |
| Device status, connection/error states, preferences, contacts and recent calls | Bind to daemon data and persistence; current values are fixtures and preference switches are session-only. |

X11 can place the mock call panel near the top-right and request above-window visibility. Wayland uses a normal GTK window because GNOME does not give applications reliable absolute placement. Headless UI tests do not establish live GNOME Shell behavior.

## How it fits together

```text
iPhone -- Bluetooth/BlueZ -- oFono (HFP call control)
                                   |
                              Rust daemon
                      call state, PBAP sync, history
                        |                 |
                 session D-Bus       GNOME notifications
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
| `src/iblueconnect-ui.c` | GTK 4/libadwaita window and call controls |
| `src/mock-ui/` | Standalone GTK 4/libadwaita interface prototype with mock state and developer controls |
| `src/ofono.rs`, `src/backend.rs`, `src/call.rs` | oFono integration and call state |
| `src/audio.rs`, `src/notifications.rs`, `src/presentation.rs` | Audio-route observation and incoming-call presentation |
| `src/pbap.rs`, `src/vcard.rs` | OBEX/PBAP phonebook and call-history parsing |
| `src/contacts_store.rs`, `src/history*.rs`, `src/call_log.rs` | Private cache, local recents, and merge logic |
| `packaging/install-user.sh` | Build and install the user daemon, UI, service, and desktop launcher |
| `packaging/build-mock-ui.sh`, `packaging/test-mock-ui.sh` | Build and check the UI-only prototype |
| `packaging/ofono-*.sh` | Enable the approved HFP backend or restore the native backend |
| `packaging/build-rnnoise.sh`, `packaging/rnnoise.sh` | Build and separately enable or roll back microphone noise suppression |
| `packaging/systemd/`, `packaging/desktop/` | Login service and application launcher |
| `IBlueConnect_Spec.md`, `Feasibility.md`, `HANDOFF.md` | V1 requirements, hardware findings, and latest project state |

## Install and run

The installer needs a Rust toolchain with the project's crates available offline, a C compiler, `pkg-config`, GTK 4/libadwaita/X11 development files, and the GNOME/PipeWire/BlueZ desktop services. From the repository root:

```sh
sh packaging/install-user.sh
```

This installs to `~/.local/bin`, adds a systemd user service and desktop launcher, and enables the daemon for the next login. oFono is the user-approved HFP backend. Enable it only when no call is active; this script changes the audio backend and can briefly interrupt laptop audio:

```sh
sh packaging/ofono-enable.sh
```

Start the installed daemon before running the backend setup so its private idle check can succeed (`systemctl --user start iblueconnectd.service` on a fresh installation). The backend script selects legacy Lua configuration for WirePlumber 0.4 and SPA-JSON for 0.5 and later. It stops if call state cannot be verified and rechecks before service changes; if a restart causes a transient unavailable state, inspect the state before continuing.

Connect the iPhone **yourself** from Bluetooth settings, then open IBlueConnect from the application menu. If oFono is removed later, `sh packaging/ofono-rollback.sh` restores the native backend; the script stops the IBlueConnect daemon. It does not reconnect the phone.

## Optional microphone noise suppression

RNNoise is separate from the core installer. The current laptop has it enabled, and one user-operated call confirmed clear two-way audio with less fan and keyboard noise. It is not yet verified across device changes or a fresh install. To add it on another setup, first install and start the core daemon, connect the phone manually, and wait until the call is idle:

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
sh packaging/test_build_rnnoise.sh
python3 packaging/test_rnnoise.py
python3 packaging/test_ofono.py
```

Hardware acceptance requires user-operated calls. Tests and graph inspection alone do not prove that an iPhone call works.

## V1 status

The current machine has demonstrated app-originated calling, contact and call-history sync, and one successful two-way call through the durable RNNoise filter. Offline Rust and GTK checks pass. This is a working milestone, not complete V1 acceptance. Key remaining work includes both in-call audio handoff directions, mute and live DTMF acceptance, first-run and settings flows, lock/DND and missed-call notification behavior, live manual-disconnect and device-switch checks, and full accessibility/privacy/resource acceptance. A prior silent-call failure had no HFP audio streams; service restart restored audio for one later call, but automatic recovery is unproven. See the [V1 acceptance matrix](IBlueConnect_Spec.md#13-acceptance-matrix) and [latest handoff](HANDOFF.md) for details.
