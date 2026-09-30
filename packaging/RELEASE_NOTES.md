# IBlueConnect preview installers

Separate amd64 packages for Ubuntu 24.04 and Ubuntu 26.04, built against each
release's native libraries. Use the package matching Settings → About; other
Ubuntu releases, ARM laptops, Android phones, and full Wayland acceptance are
not supported by this preview.

The packages include the background Rust daemon, GTK frontend, contact/history
sync, RNNoise plugin, version-aware call-only routing, and consent-based setup
with backups and an Undo action. No IBlueConnect package maintainer script
changes audio settings; installing missing Ubuntu dependencies can start their
own services. End calls and disconnect Bluetooth devices before installation.

Install the matching `.deb` with Ubuntu's software installer or
`sudo apt install ./iblueconnect_0.1.0-1ubuntu24.04_amd64.deb` (use `26.04` for
that Ubuntu version). Open IBlueConnect from the app menu and approve setup.
Connect the phone manually afterward and allow contact sharing on the phone.
Keep Ubuntu's default input on the physical microphone. Verify both call
directions and noise suppression on your own hardware.

These are preview packages: offline builds/tests and package installation are
checked separately from live Bluetooth hardware. The current upgraded 26.04.1
laptop has a confirmed two-way call with reduced background noise; fresh
hardware and native 26.04 oFono 2.x remain unverified for live calls.

Not implemented: app-controlled phone/laptop audio handoff, call-preserving
Quit/Disconnect/Forget, saved preferences/primary-phone selection, separate
optional sync controls, Clear history/Forget phone actions, lock redaction of
an already-visible controller, and locally missed-call notification actions.
Do not rely on lock-screen privacy for an already-visible call controller.

Mute/DTMF, Hold/Resume and conference controls exist but still need full live
acceptance, as do device switching, DND/focus behavior, fallback/recovery, and
accessibility/resource targets. Phone-synced missed calls remain silent.

For uninstall, end all calls, then stop the background daemon with
`systemctl --user stop iblueconnectd.service`. Disconnect Bluetooth devices
manually, run `sh /usr/lib/iblueconnect/first-run.sh --undo`, then
`sudo apt remove iblueconnect` only after Undo succeeds.
Undo preserves contacts/history. Keep the package installed if Undo reports it
has paused; finish Undo before removing files it needs.

Package size/hash metadata accompany both installers. Corresponding unmodified
noise-suppression-for-voice v1.10 source and licenses accompany this release;
the source archive is not required to install the app.

Existing source installations need manual migration; the installer refuses to
replace a per-user service. An app/OS upgrade requests setup again: end calls,
stop the background daemon, and disconnect Bluetooth devices before proceeding.
