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

/* Loudness of 16-bit mono samples as 0..1; line hiss reads as 0. */
double live_voice_level(const gint16 *samples, size_t count);

/* Taps the call's incoming-voice PipeWire stream (read-only) and reports its level about 20 times a second,
 * then -1 once if the stream ends. The callback may stop the meter. */
typedef void (*LiveVoiceFn)(double level, gpointer data);
typedef struct LiveVoiceMeter LiveVoiceMeter;
LiveVoiceMeter *live_voice_meter_start(guint64 serial, LiveVoiceFn fn, gpointer data);
void live_voice_meter_stop(LiveVoiceMeter *meter);

#endif
