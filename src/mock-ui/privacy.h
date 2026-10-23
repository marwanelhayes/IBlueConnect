#ifndef IBLUECONNECT_PRIVACY_H
#define IBLUECONNECT_PRIVACY_H

#include <gio/gio.h>

typedef struct PrivacyWatch PrivacyWatch;
typedef void (*PrivacyChangedFn)(gboolean locked, gpointer data);

/* Unknown lock state is private until GNOME confirms an unlocked session. */
PrivacyWatch *privacy_watch_start(PrivacyChangedFn changed, gpointer data);
gboolean privacy_watch_locked(const PrivacyWatch *watch);
void privacy_watch_stop(PrivacyWatch *watch);

#endif
