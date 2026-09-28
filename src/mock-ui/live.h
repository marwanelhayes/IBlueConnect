#ifndef IBLUECONNECT_LIVE_H
#define IBLUECONNECT_LIVE_H

#include <glib.h>
#include "mock-state.h"

typedef struct LiveData LiveData;

/* Maps one daemon Snapshot onto the UI model. A NULL status means the daemon is unreachable. */
void live_apply_snapshot(MockUiModel *model, const char *status, const char *id,
                         const char *phase, const char *caller);

/* BatteryStatus and Contacts status strings from the daemon. */
void live_apply_battery(MockUiModel *model, const char *status);
void live_apply_contacts_status(MockUiModel *model, const char *status);

/* Installs daemon Contacts a(sss) and Recents a(sssx) as the UI data set until live_free(). */
LiveData *live_load(GVariant *contacts, GVariant *recents, gint64 now);
void live_free(LiveData *data);

#endif
