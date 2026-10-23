#include "privacy.h"

typedef struct {
    GDBusConnection *service;
    guint name, object;
    GDBusMethodInvocation *pending;
    gboolean acquired, locked;
    guint notifications;
} Fixture;

static void changed(gboolean locked, gpointer data)
{
    Fixture *fixture = data;
    fixture->locked = locked;
    fixture->notifications++;
}

static void get_active(GDBusConnection *connection, const char *sender, const char *path,
                       const char *interface, const char *method, GVariant *parameters,
                       GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)parameters;
    Fixture *fixture = data;
    g_assert_cmpstr(method, ==, "GetActive");
    g_assert_null(fixture->pending);
    fixture->pending = g_object_ref(invocation);
}

static void acquired(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection; (void)name;
    ((Fixture *)data)->acquired = TRUE;
}

static void iterate(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}

#define AWAIT(condition) do { \
    gint64 deadline = g_get_monotonic_time() + 3000000; \
    while (!(condition) && g_get_monotonic_time() < deadline) iterate(); \
    g_assert_true(condition); \
} while (0)

static void settle(void)
{
    gint64 deadline = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < deadline) iterate();
}

static void signal_active(Fixture *fixture, gboolean locked)
{
    GError *error = NULL;
    g_dbus_connection_emit_signal(fixture->service, NULL, "/org/gnome/ScreenSaver",
        "org.gnome.ScreenSaver", "ActiveChanged", g_variant_new("(b)", locked), &error);
    g_assert_no_error(error);
    g_dbus_connection_flush_sync(fixture->service, NULL, &error);
    g_assert_no_error(error);
}

static void answer(Fixture *fixture, gboolean locked)
{
    g_dbus_method_invocation_return_value(fixture->pending, g_variant_new("(b)", locked));
    g_clear_object(&fixture->pending);
}

static void setup(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    GError *error = NULL;
    fixture->locked = TRUE;
    fixture->service = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_assert_no_error(error);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.gnome.ScreenSaver'>"
        "<method name='GetActive'><arg type='b' direction='out'/></method>"
        "<signal name='ActiveChanged'><arg type='b'/></signal></interface></node>", &error);
    g_assert_no_error(error);
    static const GDBusInterfaceVTable vtable = { .method_call = get_active };
    fixture->object = g_dbus_connection_register_object(fixture->service,
        "/org/gnome/ScreenSaver", info->interfaces[0], &vtable, fixture, NULL, &error);
    g_assert_no_error(error);
    g_dbus_node_info_unref(info);
    fixture->name = g_bus_own_name_on_connection(fixture->service, "org.gnome.ScreenSaver",
        G_BUS_NAME_OWNER_FLAGS_NONE, acquired, NULL, fixture, NULL);
    AWAIT(fixture->acquired);
}

static void teardown(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    if (fixture->pending) answer(fixture, FALSE);
    if (fixture->name) g_bus_unown_name(fixture->name);
    g_dbus_connection_unregister_object(fixture->service, fixture->object);
    g_dbus_connection_close_sync(fixture->service, NULL, NULL);
    g_clear_object(&fixture->service);
    settle();
}

static void follows_lock_and_owner_loss(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    PrivacyWatch *watch = privacy_watch_start(changed, fixture);
    g_assert_true(privacy_watch_locked(watch));
    AWAIT(fixture->pending != NULL);
    answer(fixture, FALSE);
    AWAIT(!privacy_watch_locked(watch));
    g_assert_false(fixture->locked);
    signal_active(fixture, TRUE);
    AWAIT(privacy_watch_locked(watch));
    signal_active(fixture, FALSE);
    AWAIT(!privacy_watch_locked(watch));
    g_bus_unown_name(fixture->name);
    fixture->name = 0;
    AWAIT(privacy_watch_locked(watch));
    fixture->acquired = FALSE;
    fixture->name = g_bus_own_name_on_connection(fixture->service, "org.gnome.ScreenSaver",
        G_BUS_NAME_OWNER_FLAGS_NONE, acquired, NULL, fixture, NULL);
    AWAIT(fixture->pending != NULL);
    answer(fixture, FALSE);
    AWAIT(!privacy_watch_locked(watch));
    privacy_watch_stop(watch);
}

static void newer_signal_wins_over_initial_reply(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    PrivacyWatch *watch = privacy_watch_start(changed, fixture);
    AWAIT(fixture->pending != NULL);
    signal_active(fixture, TRUE);
    settle();
    answer(fixture, FALSE);
    settle();
    g_assert_true(privacy_watch_locked(watch));
    g_assert_true(fixture->locked);
    signal_active(fixture, FALSE);
    AWAIT(!privacy_watch_locked(watch));
    privacy_watch_stop(watch);
}

static void query_failure_is_private(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    PrivacyWatch *watch = privacy_watch_start(changed, fixture);
    AWAIT(fixture->pending != NULL);
    g_dbus_method_invocation_return_dbus_error(fixture->pending,
        "org.gnome.ScreenSaver.Error", "Unavailable");
    g_clear_object(&fixture->pending);
    settle();
    g_assert_true(privacy_watch_locked(watch));
    privacy_watch_stop(watch);
}

static void stopping_cancels_pending_callback(Fixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    PrivacyWatch *watch = privacy_watch_start(changed, fixture);
    AWAIT(fixture->pending != NULL);
    privacy_watch_stop(watch);
    guint before = fixture->notifications;
    answer(fixture, FALSE);
    signal_active(fixture, FALSE);
    settle();
    g_assert_cmpuint(fixture->notifications, ==, before);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    g_test_add("/privacy/lock-and-owner-loss", Fixture, NULL, setup, follows_lock_and_owner_loss, teardown);
    g_test_add("/privacy/newer-signal", Fixture, NULL, setup, newer_signal_wins_over_initial_reply, teardown);
    g_test_add("/privacy/query-failure", Fixture, NULL, setup, query_failure_is_private, teardown);
    g_test_add("/privacy/stop-pending", Fixture, NULL, setup, stopping_cancels_pending_callback, teardown);
    int result = g_test_run();
    g_test_dbus_down(bus);
    g_object_unref(bus);
    return result;
}
