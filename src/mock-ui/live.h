#ifndef IBLUECONNECT_LIVE_H
#define IBLUECONNECT_LIVE_H

#include <gio/gio.h>
#include "mock-state.h"

typedef struct LiveData LiveData;

/* Maps SnapshotDetails onto the UI model. A NULL status means the daemon is unreachable. */
void live_apply_snapshot(MockUiModel *model, const char *status, const char *id,
                         const char *phase, const char *caller, const char *number);
void live_apply_calls(MockUiModel *model, const char *status, GVariant *rows);

/* BatteryStatus and Contacts status strings from the daemon. */
void live_apply_battery(MockUiModel *model, const char *status);
void live_apply_contacts_status(MockUiModel *model, const char *status);

/* Installs daemon Contacts a(sss) and Recents a(sssx) as the UI data set until live_free(). */
LiveData *live_load(GVariant *contacts, GVariant *recents, gint64 now);
void live_free(LiveData *data);

/* Ask Siri follows the audio link the phone opens for Siri, the only end-of-session sign the iPhone gives.
 * session_over means the link closed by itself, so oFono's stale "voice recognition on" can be cleared. */
#define LIVE_SIRI_START_MS 8000 /* No link by then: the phone did not bring Siri to this laptop. */
#define LIVE_SIRI_QUIET_MS 2000 /* Shorter gaps do not end a session; clearing would cut Siri off. */
MockSiriState live_siri_step(MockSiriState state, gboolean audio, gboolean in_call,
                             gint64 waited_ms, gint64 quiet_ms, gboolean *session_over);

/* Loudness of 16-bit mono samples as 0..1 after a linear amplitude gain; line hiss reads as 0. */
double live_voice_level(const gint16 *samples, size_t count, double gain);

/* Your side's meter gain. Old value 1.0 (no gain); new value 4.0 (about +12 dB), set 2026-10-25.
 * Calculation: the caller's voice arrives at telephony nominal speech level, about -26 dBov (ITU-T P.56),
 * which meters 0.60; a raw laptop mic at normal talking distance is about -38 dBFS (estimate, not measured),
 * which meters 0.30. The 12 dB gap is 10^(12/20) = 3.98 as amplitude, so 4.0 makes both sides meter alike.
 * The caller's side keeps gain 1.0. */
#define LIVE_MICROPHONE_GAIN 4.0

/* Taps the call's incoming-voice PipeWire stream (read-only) and reports its level about 20 times a second,
 * then -1 once if the stream ends. The callback may stop the meter. */
typedef void (*LiveVoiceFn)(double level, gpointer data);
typedef struct LiveVoiceMeter LiveVoiceMeter;
LiveVoiceMeter *live_voice_meter_start(guint64 serial, double gain, LiveVoiceFn fn, gpointer data);
void live_voice_meter_stop(LiveVoiceMeter *meter);

#endif
