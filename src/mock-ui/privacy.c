#include "privacy.h"

struct PrivacyWatch {
    guint refs, name_watch, signal;
    guint64 generation;
    gboolean locked, stopped;
    GDBusConnection *connection;
    GCancellable *cancel;
    PrivacyChangedFn changed;
    gpointer data;
};

typedef struct {
    PrivacyWatch *watch;
    guint64 generation;
} PendingState;

static PrivacyWatch *retain(PrivacyWatch *watch)
{
    watch->refs++;
    return watch;
}

static void release(gpointer data)
{
    PrivacyWatch *watch = data;
    if (--watch->refs) return;
    g_clear_object(&watch->cancel);
    g_free(watch);
}

static void set_locked(PrivacyWatch *watch, gboolean locked)
{
    if (watch->stopped || watch->locked == locked) return;
    watch->locked = locked;
    watch->changed(locked, watch->data);
}

static void disconnect_owner(PrivacyWatch *watch)
{
    watch->generation++;
    if (watch->signal) g_dbus_connection_signal_unsubscribe(watch->connection, watch->signal);
    watch->signal = 0;
    g_clear_object(&watch->connection);
}

static void active_changed(GDBusConnection *connection, const char *sender, const char *path,
                           const char *interface, const char *signal, GVariant *parameters,
                           gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)signal;
    PrivacyWatch *watch = data;
    watch->generation++; /* A newer signal must win over an outstanding initial query. */
    gboolean locked = TRUE;
    if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)")))
        g_variant_get(parameters, "(b)", &locked);
    set_locked(watch, locked);
}

static void got_active(GObject *source, GAsyncResult *result, gpointer data)
{
    PendingState *pending = data;
    PrivacyWatch *watch = pending->watch;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
    if (!watch->stopped && pending->generation == watch->generation) {
        gboolean locked = TRUE;
        if (reply) g_variant_get(reply, "(b)", &locked);
        set_locked(watch, locked);
    }
    if (reply) g_variant_unref(reply);
    release(watch);
    g_free(pending);
}

static void appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    (void)name;
    PrivacyWatch *watch = data;
    disconnect_owner(watch);
    watch->connection = g_object_ref(connection);
    watch->signal = g_dbus_connection_signal_subscribe(connection, owner, "org.gnome.ScreenSaver",
        "ActiveChanged", "/org/gnome/ScreenSaver", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        active_changed, watch, NULL);
    PendingState *pending = g_new(PendingState, 1);
    *pending = (PendingState){ retain(watch), watch->generation };
    g_dbus_connection_call(connection, owner, "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
        "GetActive", NULL, G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000,
        watch->cancel, got_active, pending);
    set_locked(watch, TRUE);
}

static void vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection; (void)name;
    PrivacyWatch *watch = data;
    disconnect_owner(watch);
    set_locked(watch, TRUE);
}

PrivacyWatch *privacy_watch_start(PrivacyChangedFn changed, gpointer data)
{
    g_return_val_if_fail(changed != NULL, NULL);
    PrivacyWatch *watch = g_new0(PrivacyWatch, 1);
    watch->refs = 1;
    watch->locked = TRUE;
    watch->changed = changed;
    watch->data = data;
    watch->cancel = g_cancellable_new();
    watch->name_watch = g_bus_watch_name(G_BUS_TYPE_SESSION, "org.gnome.ScreenSaver",
        G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, retain(watch), release);
    return watch;
}

gboolean privacy_watch_locked(const PrivacyWatch *watch) { return watch->locked; }
void privacy_watch_stop(PrivacyWatch *watch)
{
    if (!watch) return;
    watch->stopped = TRUE;
    g_bus_unwatch_name(watch->name_watch);
    g_cancellable_cancel(watch->cancel);
    disconnect_owner(watch);
    release(watch);
}
