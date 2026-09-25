# IBlueConnect — Independent design review

Three read-only agents reviewed the revised specification: technical feasibility, interaction design, and acceptance coverage. This is a specification/source audit, not a hardware or implementation pass. No app exists yet.

## Outcome

The explicit interview decisions are captured. Review findings have been incorporated into the specification. The user resolved active-call exit behavior: transfer audio to the iPhone before Quit/Disconnect; on failed or unverified handoff, stay connected. Hang up still ends the actual cellular call. The user has since reported the existing phone-initiated ASUS/headphone audio path working; see the specification's **Existing working behavior** baseline. This partially supports V07/V10 but does not constitute full application acceptance.

| Finding | Disposition |
|---|---|
| Phone/laptop handoff cannot be inferred from Bluetooth connection or generic call-control APIs | Added explicit SCO/control separation, route evidence, no automatic reacquisition, and two-way handoff feasibility gate |
| Audio architecture lacked the actual two-way bridge | Added phone-to-output and mic-to-phone paths, system-following local endpoints, feedback protection, and isolated mute |
| Relayed in-band ringing could bypass single-sound/DND policy | Added suppression and a no-pre-answer-microphone test |
| Candidate telephony configuration was unpinned | Require backend/plugin ownership, version-appropriate WirePlumber configuration, permissions, restart behavior, and exact phone build/settings |
| Persistent popup without focus theft is not guaranteed by ordinary GTK window presentation | Added desktop feasibility gate and explicit keyboard/window tests |
| A missed-call notification could create a replacement popup/banner | Require silent, banner-free notification-center posting |
| Lock privacy could permanently remove undismissed notifications | Restore them silently on unlock and distinguish privacy removal from user dismissal |
| Notification-center capacity limits indefinite retention | Added four-missed-call test; document observed limit and return any conflict for a grouping decision |
| Mute behavior across handoff was ambiguous | Retain laptop mute for the same call, reset next call, never claim it controls phone mute |
| Optional contacts were mandatory in navigation prose | Made contact-only search/navigation conditional on implementation |
| Secondary-call/Forget behavior lacked acceptance coverage | Added explicit acceptance cases |
| Active-call Quit/Disconnect/Forget behavior | Resolved: verify phone handoff before exit/disconnection; preserve session on failure; Hang up remains a separate call-ending action; acceptance V30/V31 added |
| Defaults were described too broadly as agreed | Distinguished explicit product decisions from implementation defaults |

## Local observations

Read-only queries on the target session reported:
- GNOME Shell 46.0, notification protocol 1.2.
- Advertised notification capabilities: actions, body, body-markup, icon-static, persistence, sound.
- org.gnome.desktop.notifications show-banners was true at inspection.
- Ubuntu 24.04 x86_64; GNOME/X11; BlueZ 5.72; PipeWire 1.0.5; WirePlumber 0.4.17; GTK 4.14.5; libadwaita 1.5.0.
- oFono was not installed.

No test notification was posted, no phone call was placed, and no service configuration was changed. Advertised capabilities do not prove visible action layout or notification retention.

Additional read-only implementation preflight after the exit decision:
- An iPhone is already paired and the laptop Bluetooth adapter is powered. Pairing does not establish working telephony or current call readiness.
- PipeWire and WirePlumber user services are active; libspa-0.2-bluetooth 1.0.5 is installed.
- rustc/cargo are not available on the inspected command path. GTK4, libadwaita, PipeWire, and SQLite development metadata are unavailable through pkg-config.
- The local package index offers oFono 1.31, whereas some API research below used 2.14 documentation. Verify the actual candidate's source/API before relying on newer documentation.
- No dependencies were installed or Bluetooth/audio preferences changed. Native build setup and real-device feasibility remain outstanding.

## Primary-source evidence

- [oFono hands-free audio API](https://kernel.googlesource.com/pub/scm/network/ofono/ofono/+/refs/tags/2.14/doc/handsfree-audio-api.txt) exposes SCO acquisition, not a guaranteed iPhone internal audio selector.
- [PipeWire 1.0.5 oFono backend](https://raw.githubusercontent.com/PipeWire/pipewire/1.0.5/spa/plugins/bluez5/backend-ofono.c) acquires audio through oFono and releases the SCO socket. Whether this yields the required iPhone handoff remains untested.
- [oFono call-manager API](https://kernel.googlesource.com/pub/scm/network/ofono/ofono/+/refs/tags/2.14/doc/voicecallmanager-api.txt) distinguishes telephony call transfer from the desired endpoint handoff.
- [oFono hands-free API](https://kernel.googlesource.com/pub/scm/network/ofono/ofono/+/refs/tags/2.14/doc/handsfree-api.txt) describes in-band ringing.
- [PipeWire 1.0.5 loopback module](https://raw.githubusercontent.com/PipeWire/pipewire/1.0.5/src/modules/module-loopback.c) and [WirePlumber 0.4.17 policy](https://raw.githubusercontent.com/PipeWire/wireplumber/0.4.17/src/config/policy.lua.d/10-default-policy.lua) provide candidate stream bridging/default-following facilities.
- [WirePlumber 0.4.17 Bluetooth configuration](https://raw.githubusercontent.com/PipeWire/wireplumber/0.4.17/src/config/bluetooth.lua.d/50-bluez-config.lua) documents native/oFono backend choice; installation alone is not complete integration.
- [GNOME 46 message tray](https://raw.githubusercontent.com/GNOME/gnome-shell/46.0/js/ui/messageTray.js) skips banners for low-urgency notifications and limits each source to three notifications. Verify Ubuntu's installed behavior before selecting a strategy.
- [GNOME 46 calendar](https://raw.githubusercontent.com/GNOME/gnome-shell/46.0/js/ui/calendar.js) implements notification action buttons and the DND preference.
- [GTK4 Window.present](https://docs.gtk.org/gtk4/method.Window.present.html) may focus a window; [GTK3 keep-above](https://gnome.pages.gitlab.gnome.org/gtk/gtk3/method.Window.set_keep_above.html) is not a GTK4 API.
- [libadwaita accent-color property](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/property.StyleManager.accent-color.html) starts at version 1.6, newer than the observed target.
- Apple's [call routing guide](https://support.apple.com/en-euro/guide/iphone/iph29145acf1/ios) and [in-call audio destination guide](https://support.apple.com/en-mt/guide/iphone/iph3c9951d7/ios) describe phone controls, not certification of this Linux endpoint.

## Pending checks

1. Verify the now-agreed active-call exit behavior, including failed handoff and the separate Hang up action (V30/V31).
2. Inspect and reproduce the user's existing working audio path before changing the stack, then run the real-device feasibility matrix for calls, audio, DTMF, route ownership, and both transfer directions. Preserve the existing path as a regression baseline.
3. Run desktop popup, focus, DND, lock/privacy, and missed-notification checks.
4. Resolve any demonstrated platform limitation without weakening requirements silently.
5. Build and independently review the actual native interface only after the feasibility gate.

V07/V10 have partial **User-reported baseline** evidence, detailed in the spec; full acceptance is pending. Other V1 hardware and interface rows remain **Not tested**. No agent-run call test has occurred.

The user additionally requires a virtual environment for any Python code or tooling. The spec now makes this mandatory; the preferred Rust/native stack does not itself require Python or an unused virtual environment.

## Session continuation instruction

The user requests a handoff when the five-hour Codex allowance reaches **10% remaining or less**. Check available usage metadata at active work checkpoints; do not infer quota from elapsed wall time or context usage. This session's latest inspected metadata reported 55% used in a 300-minute window (45% remaining), so the trigger had not fired. No background quota monitor is installed.

When the threshold is reached, create the destination with `mktemp -t handoff-XXXXXX.md`, read that file before writing it, then save a concise continuation handoff and give the user its path. Reference this review and `IBlueConnect_Spec.md` rather than duplicating their requirements, evidence, or delivery plan. Include only current progress, uncaptured blockers, next actions, and relevant session constraints.

Suggested next-session skills: **ponytail:ponytail** for minimal native implementation; **grill-me** only for unresolved product choices (one question at a time, with a recommendation); **diagnose** if the real call/audio path fails, following its setup prerequisite; **handoff** for the eventual continuation document. Read the chosen skill instructions before use. Do not reopen settled product decisions or introduce a web/Figma mock as a substitute for the required native application.
