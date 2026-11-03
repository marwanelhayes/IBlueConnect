# Voice quality research: received and sent voice

Date: 2026-11-01. Status: research only. Nothing was installed, built, reconfigured or restarted, no call was placed, and no Bluetooth connection was initiated.

Every claim is labelled by how it was established:

- **Source:** read in the cited upstream code, spec or paper, at the pinned version.
- **Measured here:** a read-only check on this laptop on 2026-11-01.
- **Repo:** recorded earlier in this repository.
- **Inference:** my own conclusion from the evidence. It has not been proven.
- **Unverified:** I could not confirm it.

Performance figures come from the authors of each project on their own hardware. None of them were measured on this laptop.

## Where this stands (2026-11-01)

Paused by the user before any fix. Nothing has been changed; the mSBC retry has not been approved or run.

One read-only sample during a live user-operated call replaced some of the assumptions below:

- Both HFP directions were running `cvsd` at 8000 Hz, so CVSD is now measured on the current stack.
- The route was mic → RNNoise → phone and phone → headset output, with `ERR` 0 over a few seconds. That sample was too short to say anything about "pixelated".
- The `SCO_OPTIONS` warning appeared twice on this working call, which supports reading it as routine.
- Buffers: output 2048 at 48 kHz, mic 1024 at 48 kHz, incoming 512 at 8 kHz. The 480-sample RNNoise block still doesn't divide the mic's 1024.

**User observation (2026-11-03).** On calls routed to the laptop, the user's own voice was fine, but the received voice sounded noisy on cellular calls and not on FaceTime Audio calls. Both kinds of call cross the same HFP link, so the difference points at the call's source audio. Untested explanation: a cellular call arrives already narrowband-coded by the network and is re-encoded as CVSD, while FaceTime Audio is coded once. The user has now reported far-end noise, so Rank 5's "nobody has complained" no longer holds. mSBC stays the first step.

**mSBC retry (2026-11-03): works.** On a user-operated cellular call the same day, both HFP directions ran `codec=msbc` at 16000 Hz, the daemon reported `ready/active`, and `ERR` stayed 0 over about 6 seconds. The user confirmed the incoming voice was clearer and less noisy and that the other person heard them clearly. After the call ended, `ofono-enable.sh` was made to write the line on WirePlumber 0.5+ only (0.4 keeps it off; it failed there). `test_ofono.py` asserted it first and failed, then all 12 tests passed. The live file was rewritten from `--print-config`; it differs from the tested call only by a comment, so no restart was needed. Earlier steps: User approved. With the phone disconnected (0 connected devices, 0 HFP nodes, daemon phase `unavailable`), `bluez5.enable-msbc = true` was added to the live `51-iblueconnect-ofono.conf`; backup at `target/rollback-2026-11-03/51-iblueconnect-ofono.conf.pre-msbc`. WirePlumber then oFono were restarted; all services active. WirePlumber logged `Failed to start HFP/HSP backend ofono` once at the oFono restart, as it has 19 times before on normal restarts. The daemon was not restarted. A 30-minute private `btmon` capture saw no phone connection, so nothing about the codec is known yet. `packaging/ofono-enable.sh` was not changed: re-running it drops the line. Undo: restore the backup (or `ofono-rollback.sh`), then restart WirePlumber and oFono with no call active.

No voice scores were collected. Resume at section 7: scores for the baseline call, then the mic level check, then the mSBC retry.

## 1. Conclusion

**Received voice.** The muffling is mostly the link. The route has been running CVSD, an 8 kHz codec, and no filter can restore what that link never carried. The fix to try first is one line of configuration, `bluez5.enable-msbc = true`, which allows the 16 kHz mSBC codec. That setting failed once, but on the old Ubuntu 24.04 stack (PipeWire 1.0.5, WirePlumber 0.4) and with no diagnostic capture, and it has never been retried on the current stack. Apple documents wideband for cellular calls, and this iPhone and adapter already ran `msbc` under PipeWire's native backend. So run one instrumented retry. Whatever the result, add a small presence EQ on the incoming stream using PipeWire's built-in filters, which needs no new dependency. If mSBC still fails and the capture shows oFono's path is the cause, the escalation is PipeWire's native backend with its Telephony API. That reverses a standing user decision and currently loses Ask Siri, so it needs a new decision from the user.

**Sent voice.** Don't replace RNNoise first. This check found three defects around it, and all three can be fixed without installing anything:

1. The microphone's hardware gain is at its maximum, +30 dB.
2. The iPhone's own echo cancelling and noise reduction is still on, so your voice is processed twice. Apple tells accessories that do their own processing to turn it off.
3. Only one RNNoise control is set in the config. The others fell through to accidental defaults that add 100 ms of delay.

Fix those in that order, then add a high-pass filter, a gentle compressor and a limiter around RNNoise. Only if RNNoise is still the audible problem, replace it with DeepFilterNet's LADSPA plugin with its attenuation limit set to 18–24 dB. The swap fits the existing filter-chain design. Add echo cancellation only if you actually take calls on the laptop speakers. With the wired headset it isn't needed.

## 2. What was checked against the repo

| Stated fact | Result |
|---|---|
| Incoming voice reported muffled on the CVSD route | Confirmed: [Feasibility.md](Feasibility.md) "Audio quality baseline", [README.md](README.md) line 199. This is user-reported and was never measured. |
| RNNoise only touches the microphone direction | Confirmed: `packaging/rnnoise.sh` creates a virtual source, and the WirePlumber rule in `packaging/ofono-enable.sh` matches only `api.bluez5.sco.sink`. |
| Ubuntu 26.04.1 with WirePlumber 0.5 | Confirmed. Measured here: Ubuntu 26.04.1, kernel 7.0.0-42, PipeWire 1.6.2, WirePlumber 0.5.13, BlueZ 5.85, and oFono 1.31 retained from 24.04 (Ubuntu's candidate is 2.18). |

Two corrections to the repo's picture:

- **Update, same day: `cvsd` confirmed on the current stack.** A read-only sample of a live call showed `cvsd` at 8000 Hz in both directions, so the inference below is now measured.
- **The `cvsd` reading is old.** It was recorded on the 24.04 stack. On the current stack, CVSD is what the code and config imply, because the `enable-msbc` setting is absent, but I found no codec reading from a live call after the upgrade. Treat it as inference until the test plan below confirms it.
- **"Transport endpoint is not connected" is very likely not a SCO failure.** [Feasibility.md](Feasibility.md) (2026-09-28 and 2026-09-30 entries) treats this log line as a possible SCO failure. Source: PipeWire [authorizes the deferred socket](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-ofono.c#L557) with a one-byte read and [immediately asks for the MTU](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-ofono.c#L580). The Linux [`recvmsg` path](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/net/bluetooth/sco.c?h=v7.0) moves the socket to `BT_CONFIG` on that read, and [`SCO_OPTIONS`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/net/bluetooth/sco.c?h=v7.0) returns `ENOTCONN` in that state. Measured here: all 106 occurrences in the last 30 days are this one `getsockopt(SCO_OPTIONS)` warning. Inference: it fires on each normal accepted connection and should not be used as evidence of a failed call. I did not match occurrences to individual calls, because that would mean handling call times. The [144-byte MTU fallback](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-ofono.c#L99) it triggers looks harmless on USB, because [`sco-io.c` takes the packet size from received data](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/sco-io.c#L235).

## 3. The link ceiling

### 3.1 What each layer allows

| Layer | Finding | Basis |
|---|---|---|
| Codecs on the stack | CVSD 8 kHz, [mSBC 16 kHz](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/hfp-codec-msbc.c#L85), [LC3-SWB 32 kHz](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/hfp-codec-lc3-swb.c#L78), and [Apple's LC3 24 kHz, codec id 127](https://gitlab.freedesktop.org/pipewire/pipewire/-/commit/64f2f38ec492eb377aa9175e7fe6594f7fa905ac). All four codec libraries are installed. | Source; measured here |
| oFono | Knows only [CVSD and mSBC](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/src/hfp.h?h=1.31). [Version 2.18](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/src/hfp.h?h=2.18) has the same two. **LC3 is unreachable through oFono, and upgrading oFono doesn't change that.** | Source |
| oFono wideband gate | Advertises mSBC with [`AT+BAC=1,2`](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/drivers/hfpmodem/slc.c?h=1.31) only if its audio agent registered mSBC [and the kernel supports transparent SCO](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/src/handsfree-audio.c?h=1.31). This is read when the phone link comes up. | Source |
| PipeWire's oFono backend | [Registers mSBC only if `bluez5.enable-msbc` is true, and defaults it to false](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-ofono.c#L909). The installed config doesn't set it (measured here). **This is why the route is CVSD.** The [PipeWire docs](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/doc/dox/config/pipewire-props.7.md) describe the property as a quirk override, but this backend treats it as an on/off switch. | Source; measured here |
| Kernel and adapter | Measured here: Intel adapter `8087:0026`, Bluetooth 5.2, `disable_esco=N`. [btusb selects the wideband USB alternate setting](https://github.com/torvalds/linux/blob/v7.0/drivers/bluetooth/btusb.c#L2387). [Repo](Feasibility.md): this hardware already carried `msbc` under the native backend. Not re-tested on kernel 7.0. | Measured here; source; repo |
| iPhone | Apple's [Accessory Design Guidelines R32, §56.11.3.7](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf): "Devices use Wide Band Speech for eSCO connections, such as cellular calls, FaceTime, and Siri." Apple's [profile page](https://support.apple.com/HT204387) lists HFP 1.8 (dated 2023). The [SIG test suite](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2025/02/HFP.TS_.p28.pdf) has LC3-SWB cases in later HFP versions. Whether this iPhone 17 on iOS 27 offers LC3 as a gateway is **unverified**. | Source; unverified |
| Phone network | Wideband also requires ["mobile handsets, the underlying telephone infrastructure and the mobile cells"](https://arxiv.org/abs/2010.10677) to support it. [AMR-WB](https://www.itu.int/rec/T-REC-G.722.2) and [EVS](https://www.3gpp.org/dynareport/26441.htm) are the wideband-capable codecs. What this operator delivers on each call is **unverified**. | Source; unverified |

Inference on the network: the voice was clearer when the call was moved to the iPhone itself ([Feasibility.md](Feasibility.md)). So the phone holds better audio than CVSD delivers, at least on that call. The network isn't the only limit, but some calls may stay narrowband even with mSBC working, especially across carriers.

### 3.2 Why the earlier mSBC trial failed: unknown

[Feasibility.md](Feasibility.md) records the failure: the iPhone selected the laptop, and PipeWire created zero HFP streams. No debug capture was taken. What can be said from the source:

- [PipeWire emits the HFP nodes only when the transport reaches `PENDING`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/bluez5-device.c#L784). The [oFono backend reaches that state only after a successful `NewConnection`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-ofono.c#L581). Inference: the failure happened at or before that step, not in the audio decoding. These links are to the current version; the failed trial ran 1.0.5.
- The CVSD path works, so the failing step should be specific to mSBC. The candidates are the `+BCS` exchange, [oFono's `BT_VOICE` setting on accept](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/src/handsfree-audio.c?h=1.31), the [kernel's eSCO accept parameters for transparent mode](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/net/bluetooth/sco.c?h=v7.0), or PipeWire's handling on the old version.
- The stack has changed since then. Three June 2025 commits changed this backend's codec handling ([61b0ea45](https://gitlab.freedesktop.org/pipewire/pipewire/-/commit/61b0ea4589711d5b380632d3950f51fa0620a172), [83f6d719](https://gitlab.freedesktop.org/pipewire/pipewire/-/commit/83f6d719b1615d94055c661f3960cc2b26736879), [df591638](https://gitlab.freedesktop.org/pipewire/pipewire/-/commit/df591638a59efa59d89566467f93968358f55dd6)), and the kernel moved from the 24.04 series to 7.0. None of these is proven to fix the failure. They are the reason a retry is worthwhile.
- Upgrading oFono is not the fix. Comparing 1.31 with 2.18, I found no changed codec-related line in `handsfree-audio.c` or `slc.c`. The notable change is that the [profile registers as HFP 1.8 instead of 1.7](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/plugins/hfp_hf_bluez5.c?h=2.18).

### 3.3 What wideband would and wouldn't give

mSBC doubles the sample rate, which takes the top of the audio band from below 4 kHz to below 8 kHz. Inference: that is the band that separates "s", "f" and "th" and removes the muffled quality, so nothing else on the received side can compete with it.

One limit remains. PipeWire's mSBC packet-loss concealment [needs SpanDSP](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/plc.h), and measured here Ubuntu's `libspa-codec-bluez5-hfp-msbc.so` doesn't link it. CVSD has [no concealment hook at all](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/hfp-codec-cvsd.c). LC3 [has it built in](https://gitlab.freedesktop.org/pipewire/pipewire/-/commit/d0680a2b3d85717089e0b8fefdbe2578b7c902c3). So mSBC dropouts may remain audible, and mSBC is a compressed format, so a lost packet can sound worse than on CVSD (inference). That is the point of the "pixelated" check in the test plan. Measured here: Wi-Fi is on 5 GHz, so 2.4 GHz overlap isn't the explanation at the moment. Whether radio errors explain "pixelated" remains unverified.

## 4. Received voice: ranked options

### Rank 1: retry mSBC on the current stack, with capture

- **Change:** add `bluez5.enable-msbc = true` to `monitor.bluez.properties` in `~/.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf`, then restart WirePlumber and then oFono. [`packaging/ofono-enable.sh`](packaging/ofono-enable.sh) already does that order with its idle guard.
- **If it works:** add the property to `print_config` in that script and update `packaging/test_ofono.py`. The existing enable and rollback scripts stay as they are otherwise.
- **Rollback:** remove the line and restart in the same order.
- **Risk:** known and recoverable. If it fails again, the call has no laptop audio until moved to the iPhone, as last time. Apply the [Feasibility.md](Feasibility.md) lesson: don't restart anything until the user confirms the call has ended.

### Rank 2: small presence EQ on the incoming stream (do this regardless)

The EQ can't restore missing bandwidth, but it can make the band that exists clearer. It needs only PipeWire's [built-in biquad filters](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/modules/module-filter-chain.c), which are IIR filters. Inference: they add no meaningful latency. The values below are my starting points and must be tuned by ear:

```
filter.graph = { nodes = [
  { type = builtin name = hp label = bq_highpass control = { "Freq" = 120.0 "Q" = 0.7 } }
  { type = builtin name = presence label = bq_peaking control = { "Freq" = 2800.0 "Q" = 1.0 "Gain" = 4.0 } }
] links = [ { output = "hp:Out" input = "presence:In" } ] }
```

This would be a second, separate filter-chain fragment with `media.class = Audio/Sink` and its own enable and rollback script, mirroring [`packaging/rnnoise.sh`](packaging/rnnoise.sh). It keeps one job per fragment and leaves RNNoise alone. On mSBC, retune the peak or add a mild `bq_highshelf` near 4 kHz. Three changes are needed:

1. A WirePlumber rule matching `factory.name = "api.bluez5.sco.source"` that sets `target.object` to the new sink, built the same way as the existing outgoing rule.
2. `src/audio.rs`: [the playback check (lines 130–133)](src/audio.rs) accepts any `Audio/Sink`, so a virtual sink with no physical output behind it would still count as verified. It needs the same link-group logic the capture side already has (lines 136–152), ideally pulled into one shared function. Tests go first.
3. The virtual sink will probably appear in GNOME's output list. That is unverified. The live nodes also expose an `audioconvert.filter-graph` property ([source](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/audioconvert/audioconvert.c#L829)), which might attach the EQ to the HFP stream with no extra node. I found no user documentation for it, so it needs a small experiment first. If it works, it would remove changes 2 and 3.

### Rank 3: level control, only if volume swings are a complaint

There are two free options. The [built-in EBU R128 plugin](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/daemon/filter-chain/35-ebur128.conf) is installed, but its example uses the *Global* LUFS measure. Inference: that adapts slowly and is a poor fit for short calls. SWH's `sc4m_1916` compressor (`swh-plugins`, GPL-2+) is a better fit. Nobody has reported volume swings yet, so skip this until someone does.

### Rank 4: escalate to PipeWire's native backend, if and only if rank 1 fails because of oFono

[PipeWire's Telephony service](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/README-Telephony.md) provides call control on the native backend and implements `org.ofono.Manager`, `VoiceCallManager` and `VoiceCall`. Native mSBC is already proven on this hardware (repo), and this route is the only one that could reach LC3. It also exposes the active codec directly as a [`Codec` property](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/telephony.c#L125).

It is not a drop-in replacement for what this daemon does (source compared with `src/ofono.rs`):

| Daemon use | Telephony service | Consequence |
|---|---|---|
| `org.ofono.Handsfree` `VoiceRecognition` (Ask Siri) | Not implemented, and [`backend-native.c`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/backend-native.c) has no `AT+BVRA` | **Ask Siri stops working** |
| `Dial(number, "default")` | [`Dial(number)`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/telephony.c#L93) | Signature change |
| Modem `Powered` and `Online` readiness | I found no such properties in `telephony.c` | Readiness logic changes |
| Phone address parsed from the oFono modem path, for PBAP sync (`src/ofono.rs` line 471) | [Different object path](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/README-Telephony.md) (`/org/pipewire/Telephony/ag0`) | Contact sync needs another source for the address |
| System bus `org.ofono` | Session bus by default, or a [`provide-ofono` mode](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/telephony.c#L430) | Bus and service name change |

Call control through this service has not been tried with this iPhone. AGENTS.md makes oFono the permanent backend, so this needs a new user decision. Recommendation: if it comes to this, do it behind the backend interface the daemon already has, keep the oFono route as rollback, and accept the Siri loss only if the audio gain is worth it to the user.

### Rank 5: not recommended now

- **Denoising the incoming voice.** DeepFilterNet ships a [sink config](https://github.com/Rikorose/DeepFilterNet/blob/v0.5.6/ladspa/README.md), but nobody has complained of noise from the far end. Its behaviour on 8 kHz content upsampled to 48 kHz is unverified. Skip it.
- **Neural bandwidth extension.** No option can run in real time inside PipeWire today:
  - [LavaSR](https://github.com/ysharma3501/LavaSR) (Apache-2.0) reports about 50× real time on CPU and lists real-time enhancement as a use, but I found no causal streaming mode or LADSPA plugin in its README.
  - [AP-BWE](https://github.com/yxlu-0102/AP-BWE) (MIT, weights included) reports 18.1× on CPU, for offline use.
  - [FlashSR](https://github.com/ysharma3501/FlashSR) (Apache-2.0) has streaming, but takes 16 kHz input and its example uses 250 ms chunks, which is far too slow for a call.
  - [Google's streaming model](https://arxiv.org/abs/2010.10677) reports 16 ms architectural latency, but I found no official code, and an [unofficial version](https://github.com/zeroone-universe/RealTimeBWE) says it leaves out the streaming convolution.
  - [BBWEXNet](https://github.com/eagomez2/bbwexnet) is a demo page for a paper, and targets 16 to 48 kHz.
  - `callenhancer`, listed by [audiosronnx](https://github.com/TigreGotico/audiosronnx), has non-commercial weights and runs at about 0.3–0.4× real time, so it's excluded on both counts.

  All of these invent high frequencies, and none would beat real mSBC (inference). Revisit only if mSBC proves unreachable.
- **Harmonic exciter** ([Calf Exciter](https://github.com/calf-studio-gear/calf), LGPL, LV2). This is the non-neural form of the same idea. It runs in real time, but its benefit on phone speech is unverified and it can sound harsh. Try it only if mSBC is unreachable and you want one more experiment.
- **Raising resampler quality.** [`resample.quality` defaults to 4](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/doc/dox/config/pipewire-props.7.md) (measured here at 4). Inference: this won't be audible on speech this narrow. Leave it.

## 5. Sent voice: ranked options

### Rank 1: check the microphone level

Measured here: the C-Media `Mic` control is at 46 of 46, or **+30 dB**, and PipeWire reports the source volume as 1.00. It was 92% in [Feasibility.md](Feasibility.md). Inference: a hot input can clip before RNNoise, and no later stage can undo clipping. It also raises the fan and keyboard noise RNNoise must fight. I didn't measure actual peak levels, because that needs recording your voice. The test plan has a user-run check. Target about -12 to -6 dBFS peaks on loud speech. The app's waveform meter applies its own gain to the display, so it may need retuning afterwards. The hardware gain setting is per-device, so other apps change with it.

### Rank 2: stop the iPhone processing your voice a second time

Measured here: oFono reports `EchoCancelingNoiseReduction = true` on the live modem, and the iPhone advertises the feature. So RNNoise's output is, by default, subject to the phone's own processing. Apple's [Guidelines R32 §56.11.3.4](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf) says an accessory doing its own processing "needs to turn these features off on the device (the Audio Gateway), to avoid unnecessary audio quality degradation due to duplicate audio processing."

- **Change:** oFono already exposes this as a [writable property](https://git.kernel.org/pub/scm/network/ofono/ofono.git/tree/doc/handsfree-api.txt?h=1.31#n69) that sends `AT+NREC=0`. No service restarts or new dependencies. If it helps, the daemon sets it once whenever a modem becomes ready, in `src/ofono.rs`, next to the existing `VoiceRecognition` write.
- **Reversal:** the property is non-persistent and can't be switched back on from the laptop. Inference: reconnecting the phone restores it. Confirm with the read-only check in the test plan.
- **Warning:** do this only with the wired headset, or after local echo cancellation is added and verified (rank 6). On laptop speakers, this removes the phone's echo cancelling and the other person may hear themselves.
- **Unverified:** how much this changes the sound on an iPhone. What iOS actually does on this path isn't documented beyond the quotation above.

### Rank 3: make the RNNoise settings deliberate

The installed config sets only `VAD Threshold (%)`. Measured here, the live node holds:

| Control | Live value | Where it came from |
|---|---|---|
| VAD Threshold | 50% | Set in [`rnnoise.sh`](packaging/rnnoise.sh) |
| VAD Grace Period | **500 ms** | Middle of range, by default |
| Retroactive VAD Grace | **100 ms** | Middle of range, by default |

The plugin's [`default_low` hint is mapped to `LADSPA_HINT_DEFAULT_MIDDLE`](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/src/ladspa_plugin/ladspa%2B%2B.h#L287), and [PipeWire honours that default](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/filter-graph/plugin_ladspa.c#L74). The upstream example uses [200 and 0](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/README.md). The README warns that retroactive grace "introduces latency", so this install has about 100 ms of extra delay nobody asked for. It does help protect word starts, so setting it to zero isn't automatically better.

Why RNNoise output can sound degraded (source):

- It works on [22 Bark-derived bands, and its author names the main disadvantage: "we cannot model finer details"](https://arxiv.org/abs/1709.08243), relying on a pitch filter for what lies between harmonics. Inference: this limited resolution is the source of its slightly hollow character.
- Below the threshold, the plugin [writes hard zeros with no fade or comfort noise](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/src/common/src/RnNoiseCommonPlugin.cpp#L212). The code carries a TODO asking whether it should "output some noise instead". There is a [minimum grace of 20 blocks, 200 ms](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/src/common/src/RnNoiseCommonPlugin.cpp#L10). Inference: the other person may hear noise switching on and off abruptly around speech.
- Its 480-sample block doesn't divide the current 1024 quantum (measured here while idle; the call-time quantum is unverified). The plugin [zero-fills until it has enough output and "STRONGLY" recommends divisible sizes](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/src/common/src/RnNoiseCommonPlugin.cpp#L191). Inference: this adds some further latency.

The change is in `print_config` in `rnnoise.sh`: set every control explicitly and pin the block size. Try these one at a time:

```
control = { "VAD Threshold (%)" = 50.0 "VAD Grace Period (ms)" = 300 "Retroactive VAD Grace (ms)" = 30 }
capture.props = { node.latency = "960/48000" ... }
```

Then A/B the threshold at 50 against 20. Lower means less gating and more residual noise. Upstream [suggests 85–95%](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/README.md), which favours silence over voice, so I wouldn't go there for this goal. Whether `node.latency` holds through the HFP route is unverified: check it in `pw-top`.

### Rank 4: finish the chain around the suppressor

Use this order: high-pass → (echo cancel, if ever needed) → noise suppression → compressor → limiter → PipeWire's resampler to the HFP rate. WebRTC's own processing [runs in the same order](https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing/-/blob/v1.3/webrtc/modules/audio_processing/audio_processing_impl.cc#L1149): high-pass, echo control, noise suppression, gain control. Inference: compressing before suppression would raise the noise floor it has to remove.

| Stage | Tool | Decision |
|---|---|---|
| High-pass near 100 Hz | Built-in `bq_highpass` | Add. Free, and removes rumble and handling noise. |
| Compressor | [`sc4m_1916` from `swh-plugins`](https://github.com/swh/ladspa) (GPL-2+; controls checked in the Ubuntu package) | Add, gently (around 2:1 to 3:1). A new `Depends`. |
| Limiter | `fast_lookahead_limiter_1913` or `hard_limiter_1413` from the same package | Add as a safety. Its lookahead latency is unmeasured, so check it. Inference: CVSD handles overload badly. |
| De-esser | None | Skip on CVSD: inference, sibilance sits mostly above the band the link carries. Reconsider only if mSBC works and someone complains. |
| AGC | None | Skip. Inference: it would bring up noise in pauses and fight the compressor. |
| Capture rate | Keep 48 kHz | RNNoise's [480-sample block](https://github.com/werman/noise-suppression-for-voice/blob/v1.10/src/common/include/common/RnNoiseCommonPlugin.h#L57) is 10 ms at that rate, and PipeWire resamples to the HFP rate already. |

[LSP's compressors](https://github.com/lsp-plugins/lsp-plugins) (LGPL-3.0) are a capable alternative, but heavier as a dependency.

These plugins load into PipeWire's filter-chain process, not into IBlueConnect, and they would be package dependencies rather than bundled code. Inference: this doesn't create a GPL-3.0-or-later conflict. GPL-2+ and LGPL-3.0 are compatible with it anyway. Not legal advice.

### Rank 5: replace RNNoise, only if ranks 1–4 aren't enough

**Recommended: [DeepFilterNet LADSPA v0.5.6](https://github.com/Rikorose/DeepFilterNet/blob/v0.5.6/ladspa/README.md).** It has a prebuilt [`libdeep_filter_ladspa-0.5.6-x86_64-unknown-linux-gnu.so`](https://github.com/Rikorose/DeepFilterNet/releases/tag/v0.5.6) and a [shipped PipeWire config](https://github.com/Rikorose/DeepFilterNet/blob/v0.5.6/ladspa/filter-chain-configs/deepfilter-mono-source.conf). Its key control for this goal is `Attenuation Limit (dB)`, where the config suggests 18–24 dB for medium reduction. Inference: that limit is what keeps speech natural, and RNNoise has nothing like it. On the authors' own [benchmark](https://arxiv.org/abs/2205.05474), the original DeepFilterNet scores PESQ 2.81 and CSIG 4.14 against RNNoise's 2.33 and 3.40. Change: in `rnnoise.sh`'s graph, swap the node and keep the output name `echo-cancel-source`, so the WirePlumber rule still works. Rollback is the existing script.

Cautions:

- **Maintenance.** Latest release is August 2023, and the last push to the repo was October 2024. Treat it as dormant, and pin the build.
- **CPU and latency.** Minimum 20 ms, plus host latency, per its README. The [paper](https://arxiv.org/abs/2305.08227) reports RTF 0.19 single-threaded on an i5-8250U for DeepFilterNet3, but the LADSPA build uses a no-lookahead model, for which I found no figure. Measure on this i7-12700H. Compared with the current 100 ms retroactive setting, it may still reduce total delay. That's unverified.
- **Double processing.** Do rank 2 first, or you test against the phone's processing as well.

**Alternative: [GTCRN LADSPA](https://github.com/Xiaobin-Rong/gtcrn/tree/502ebfab64da7c4a9af78dcb9c6ceef1ebb01c73/ladspa).** MIT, updated in August 2026, and very light: 33 MMAC/s, and its authors report [RTF 0.07 on an i5-12400](https://github.com/Xiaobin-Rong/gtcrn/blob/502ebfab64da7c4a9af78dcb9c6ceef1ebb01c73/README.md). A 16 kHz model suits HFP, since neither CVSD nor mSBC carries more. It has a `Strength` mix control. Its authors' table puts it above both RNNoise and DeepFilterNet on PESQ, but that's their own test, and on the speech-signal measure (DNSMOS SIG) it ties RNNoise at 3.00. Its README calls the LADSPA plugin experimental and community-maintained. I found no prebuilt binary in its instructions, so it needs a build (ONNX Runtime, optionally Docker). Latency from its [512-point FFT at 16 kHz](https://github.com/Xiaobin-Rong/gtcrn/blob/502ebfab64da7c4a9af78dcb9c6ceef1ebb01c73/ladspa/src/stft.rs) is about 32 ms (inference), plus resampling. Choose it if DeepFilterNet proves too heavy or breaks on a future Ubuntu.

**Ruled out for now:**

- [DPDFNet](https://github.com/ceva-ip/DPDFNet) (Apache-2.0): it has 8/16/48 kHz streaming models, but I found no LADSPA plugin, and its 8 kHz models need at least 1.29 GMACs. Weights licence not confirmed separately.
- PipeWire's [ONNX filter](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/modules/module-filter-chain.c#L928), which is installed: it passes tensors and state to a model, as in its Silero VAD example, but GTCRN needs an STFT around the model (hence `stft.rs`). So it's no drop-in.
- Speex: PipeWire's echo-cancel module offers only `webrtc` and `null` on this system (measured here: `spa-0.2/aec/`).
- WebRTC noise suppression: tried, with [no clear improvement](Feasibility.md) on fan and keyboard noise.

### Rank 6: echo cancellation, only for laptop-speaker calls

The [verified route uses a wired headset](Feasibility.md), so skip this unless you take calls on the speakers. If needed, use [`module-echo-cancel`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/modules/module-echo-cancel.c) with [WebRTC](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/aec/aec-webrtc.cpp#L108), which is already installed, ahead of RNNoise, with `aec.args = { webrtc.noise_suppression = false webrtc.gain_control = false }` so RNNoise stays the only suppressor.

**Trap:** its default source name is [`echo-cancel-source`](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/modules/module-echo-cancel.c#L1388), the same name [IBlueConnect gives the RNNoise output](packaging/rnnoise.sh) and that the WirePlumber rule targets. Set `source.props.node.name` to something else, or the two will collide. Better, also rename the RNNoise node to `iblueconnect-call-mic`, which fixes a misleading name. That touches `rnnoise.sh`, `ofono-enable.sh` and its tests.

**Siri note.** Apple's [§46.5](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf) says single-channel noise reduction, automatic gain control and "attempts to blank out non-speech periods in the waveform shall not be applied" to Siri input. The current rule routes all outgoing HFP through RNNoise, Siri sessions included. If Ask Siri mishears, that's a likely cause (inference, not tested). A compressor would add to the same conflict.

## 6. Candidates

Latency and CPU are the authors' figures unless marked otherwise. "Not measured" means nobody has measured it on this laptop.

| Candidate | Direction | Licence | Real time | Latency | CPU | Integration cost |
|---|---|---|---|---|---|---|
| [mSBC in PipeWire](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/hfp-codec-msbc.c) | Both | MIT (plugin); libsbc licence not re-read | Yes | Not measured | Not measured | One config line; guarded restart |
| `AT+NREC=0` via oFono | Sent | GPL-2.0-only (oFono, per 2.18 headers) | Yes | None added (inference) | None | Small daemon change |
| [RNNoise controls](https://github.com/werman/noise-suppression-for-voice/tree/v1.10) | Sent | Plugin GPL-3.0; RNNoise BSD-style `COPYING`, model data in the same tree | Yes | Measured live: 100 ms retroactive setting, on top of 10 ms hop | [1.3% of a Haswell core for the 2018 model](https://arxiv.org/abs/1709.08243). Bundled model is newer, so not measured | One line in `rnnoise.sh` |
| PipeWire built-in biquads | Both | MIT | Yes | Negligible (inference) | Negligible (inference) | Config, rule, `audio.rs` for incoming |
| [EBU R128 plugin](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/src/daemon/filter-chain/35-ebur128.conf) | Received | PipeWire project; file licence not re-read | Yes, but slow to adapt (inference) | Not measured | Not measured | Config |
| SWH `sc4m`, limiters | Both | GPL-2+ | Yes | Compressor unmeasured; limiter lookahead unmeasured | Not measured | New package dependency |
| [LSP compressors](https://github.com/lsp-plugins/lsp-plugins) | Both | LGPL-3.0 | Yes | Not measured | Not measured | Larger dependency |
| [DeepFilterNet LADSPA](https://github.com/Rikorose/DeepFilterNet/tree/v0.5.6) | Sent | MIT or Apache-2.0. [Paper says weights are published under its licence](https://arxiv.org/abs/2305.08227); the repo's text says "all code" | Yes | ≥20 ms plus host | RTF 0.19 on i5-8250U (DFN3); LADSPA model not measured | Bundle one `.so`; swap one node |
| [GTCRN LADSPA](https://github.com/Xiaobin-Rong/gtcrn/tree/502ebfab64da7c4a9af78dcb9c6ceef1ebb01c73) | Sent | MIT (code and repo weights; no separate weights licence) | Yes; experimental | About 32 ms (inference) | RTF 0.07 on i5-12400 | Must build; ONNX Runtime |
| [DPDFNet](https://github.com/ceva-ip/DPDFNet) | Sent | Apache-2.0; weights not confirmed | Streaming API; no LADSPA found | Not found | ≥1.29 GMAC, heavy | Needs a wrapper |
| [WebRTC AEC](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/aec/aec-webrtc.cpp) | Sent | PipeWire plugin MIT; WebRTC library BSD-style, not re-read | Yes | Not measured | Not measured | Module config; node rename |
| [PipeWire native + Telephony](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/1.6.2/spa/plugins/bluez5/README-Telephony.md) | Both | MIT | Yes | Not measured | Not measured | **High:** daemon changes, Siri loss, new user decision |
| [LavaSR](https://github.com/ysharma3501/LavaSR), [AP-BWE](https://github.com/yxlu-0102/AP-BWE) | Received | Apache-2.0; MIT incl. weights | **Offline in this architecture** | Unsuitable or unknown | About 50× and 18× real time on CPU | Custom plugin needed |
| [Calf Exciter](https://github.com/calf-studio-gear/calf) | Received | LGPL | Yes | Not measured | Not measured | LV2 dependency; benefit unverified |
| `callenhancer` weights | Received | **CC-BY-NC-4.0: free, not open source** | **No**, 0.3–0.4× | Unsuitable | Too heavy | Excluded |

All selected options are free and open source. Inference: nothing recommended conflicts with GPL-3.0-or-later. Apache-2.0 is compatible with GPLv3 specifically, and if DeepFilterNet is bundled, choose either licence and keep the notices alongside the RNNoise ones in the `.deb`. Training-data licences behind the model weights were not audited.

## 7. User-operated test plan

Nothing here can be done by an agent alone: it needs real calls. Before any service restart, use the existing guards (fresh `Snapshot` with phase `idle`, zero HFP nodes, and the user's own word that the call ended). Change one thing per call. Ask the same person to rate each one, and read the same sentence with plenty of "s", "f" and "th".

### Baseline (no changes)

During a call routed to the laptop:

```sh
# Codec and direction only; prints no names or addresses.
pw-dump | jq -r '.[] | select(.type=="PipeWire:Interface:Node")
  | .info.props | select(."api.bluez5.profile"=="headset-audio-gateway")
  | "\(."media.class") codec=\(."api.bluez5.codec") rate=\(."audio.rate" // "?")"'
pw-top   # Note QUANT, RATE and ERR for the HFP and RNNoise rows. Quit with q.
```

Record which codec the current stack actually uses, and whether ERR rises when the voice sounds "pixelated".

To read the radio link parameters, the user runs this in another terminal, starting it before the call:

```sh
sudo btmon 2>&1 | grep -E -A14 'Synchronous Connect Complete' | grep -Ev 'Address'
```

Check `Air mode` (CVSD or Transparent) and `Retransmission window`. A zero window would support the packet-loss explanation for "pixelated". Keep the output to yourself or summarize it. **Warning:** don't remove that filter and share raw `btmon` output, because it contains your addresses and can contain caller details. Even the filtered output should be read before sharing.

### Step 1: microphone level (no call)

```sh
pw-record --target alsa_input.usb-ASUSTeK_COMPUTER_INC._C-Media_R__Audio-00.analog-stereo /tmp/mic-test.wav
```

Speak loudly for 10 seconds, stop with Ctrl-C, and open the file in Audacity (or any editor you like). If the peaks touch full scale, lower the source with `wpctl set-volume <mic id> 0.7` and repeat until the peaks sit around -12 to -6 dBFS. Delete the file afterwards.

### Step 2: iPhone processing off (headset only)

With no call active:

```sh
m=$(busctl --system --json=short call org.ofono / org.ofono.Manager GetModems | jq -r '.data[0][0][0]')
busctl --system call org.ofono "$m" org.ofono.Handsfree SetProperty sv EchoCancelingNoiseReduction b false
```

Make a call and compare with the baseline. Afterwards, reconnect the phone yourself, then check that it reset (this prints only the value):

```sh
m=$(busctl --system --json=short call org.ofono / org.ofono.Manager GetModems | jq -r '.data[0][0][0]')
busctl --system --json=short call org.ofono "$m" org.ofono.Handsfree GetProperties | jq '.data[0].EchoCancelingNoiseReduction.data'
```

### Step 3: mSBC retry

1. The user approves. Confirm no call is active, and save a copy of `51-iblueconnect-ofono.conf`.
2. Add `bluez5.enable-msbc = true`. Restart WirePlumber, then oFono, with the guard before each. If the phone drops, the user reconnects it. The app must never do that.
3. Raise logging without a restart: `wpctl set-log-level 4` applies to WirePlumber. Afterwards, set it back with `wpctl set-log-level 2`; that previous level is my assumption, so check it. These logs can contain addresses, so keep them private. If the failure repeats, a temporary `ofonod -n -d` is the next step, and it needs separate approval because it changes how a system service runs.
4. Start the `btmon` command above and add `AT\+B(AC|CS)|\+BCS` to the search, so the codec exchange appears if `btmon` shows it as text (unverified). Treat this output as private too.
5. The user makes a test call and selects the laptop on the iPhone.

| Result | Meaning | Next |
|---|---|---|
| Two HFP nodes with `codec=msbc`, rate 16000, two-way audio | Success | Compare with baseline, repeat over several calls and carriers, then make it permanent in `ofono-enable.sh` with tests |
| Two HFP nodes with `codec=cvsd` | Phone didn't pick mSBC. Check `AT+BAC=1,2` and `+BCS` | Check the order: the agent must register before the phone link comes up |
| `+BCS: 2` seen, no `Synchronous Connect Complete` or a failure status | eSCO setup failed | Keep the capture; compare with native-backend parameters |
| Connection complete with Transparent air mode, but zero nodes | The problem is in the PipeWire and oFono hand-off | Debug logs; this is a candidate upstream bug report |

If it fails: move the call to the iPhone, wait for the user to say the call has ended, confirm idle, and restore the saved file. Restart in the same order.

### Step 4 onwards: one change per call

Apply the incoming EQ, then RNNoise settings, then the outgoing chain, then DeepFilterNet if needed. For each, record:

- The far-end rating of your voice, and whether word starts are cut off.
- Your rating of the far-end voice.
- Noticeable delay or talking over each other.
- `pw-top` ERR.
- CPU of the filter-chain process during the call, using `top` with the PID from `systemctl --user status filter-chain.service`.

Also confirm that the filter-absent fallback still works, and that disconnecting the phone still updates an open window.

## 8. Open questions

1. Does the current stack really negotiate CVSD on a live call? (Baseline step.)
2. Why did the first mSBC trial fail, and does it still fail on PipeWire 1.6.2 with kernel 7.0?
3. Does this iPhone offer LC3-SWB or codec 127 as a gateway? Only a native-backend test can show that, and oFono can't reach them either way.
4. Does the operator carry wideband on the user's typical calls, or will some stay narrowband regardless of the link?
5. How audible is the iPhone's EC/NR on this path, and does reconnecting really reset it?
6. Is "pixelated" caused by radio errors? What are the retransmission settings the phone chooses?
7. Is the microphone clipping at +30 dB?
8. Does the laptop ever play calls through its speakers? If so, rank 6 of the sent list applies and step 2 needs care.
9. Can `audioconvert.filter-graph` carry the incoming EQ directly on the HFP stream? That would avoid the virtual sink and the `audio.rs` change.
10. Does RNNoise harm Ask Siri recognition enough to route Siri around it?
11. The current HFP specification is [HFP 1.10](https://www.bluetooth.com/specifications/specs/hands-free-profile-1-10/). I did not read its text, and took codec behaviour from implementers, Apple's guidelines and the SIG test suite. CVSD's behaviour under overload, mentioned in the limiter row, is general knowledge I didn't source.
