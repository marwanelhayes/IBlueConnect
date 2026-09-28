# IBlueConnect

IBlueConnect lets an iPhone use an Ubuntu laptop as a hands-free endpoint for cellular calls. It runs a small Rust daemon in the background and opens its GTK 4/libadwaita window only from the application icon. The user connects the iPhone to the laptop in Bluetooth settings; IBlueConnect never initiates that Bluetooth link.

The current target is Ubuntu 24.04 GNOME on X11 with PipeWire, WirePlumber, BlueZ, oFono, and a system-selected wired headset or microphone. Other devices and desktops have not been accepted as V1 targets. The app is local-only and stores synced contacts and recent calls in user-owned files.

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
| `src/ofono.rs`, `src/backend.rs`, `src/call.rs` | oFono integration and call state |
| `src/audio.rs`, `src/notifications.rs`, `src/presentation.rs` | Audio-route observation and incoming-call presentation |
| `src/pbap.rs`, `src/vcard.rs` | OBEX/PBAP phonebook and call-history parsing |
| `src/contacts_store.rs`, `src/history*.rs`, `src/call_log.rs` | Private cache, local recents, and merge logic |
| `packaging/install-user.sh` | Build and install the user daemon, UI, service, and desktop launcher |
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

Connect the iPhone **yourself** from Bluetooth settings, then open IBlueConnect from the application menu. If oFono is removed later, `sh packaging/ofono-rollback.sh` restores the native backend; the script stops the IBlueConnect daemon. It does not reconnect the phone.

## Optional microphone noise suppression

RNNoise is separate from the core installer. The current laptop has it enabled, and one user-operated call confirmed clear two-way audio with less fan and keyboard noise. It is not yet verified across device changes or a fresh install. To add it on another setup, first install and start the core daemon, connect the phone manually, and wait until the call is idle:

```sh
sh packaging/build-rnnoise.sh
systemctl --user enable filter-chain.service
sh packaging/rnnoise.sh enable target/rnnoise/build/bin/ladspa/librnnoise_ladspa.so
```

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
```

Hardware acceptance requires user-operated calls. Tests and graph inspection alone do not prove that an iPhone call works.

## V1 status

The current machine has demonstrated app-originated calling, contact and call-history sync, and one successful two-way call through the durable RNNoise filter. Offline Rust and GTK checks pass. This is a working milestone, not complete V1 acceptance. Key remaining work includes both in-call audio handoff directions, mute and live DTMF acceptance, first-run and settings flows, lock/DND and missed-call notification behavior, live manual-disconnect and device-switch checks, and full accessibility/privacy/resource acceptance. A prior silent-call failure had no HFP audio streams; service restart restored audio for one later call, but automatic recovery is unproven. See the [V1 acceptance matrix](IBlueConnect_Spec.md#13-acceptance-matrix) and [latest handoff](HANDOFF.md) for details.
