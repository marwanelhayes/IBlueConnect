#define main iblueconnect_application_main
#include "app.c"
#undef main
#include <glib/gstdio.h>

typedef struct {
    MockApp app;
    GDBusConnection *service, *client;
    guint object;
    GDBusMethodInvocation *audio, *snapshot, *mute;
    gboolean reject_exit;
    guint exit_calls;
    char exit_action[16];
} AppFixture;

static char *tap_directory, *tap_log;

static void app_test_iterate(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}

#define APP_AWAIT(condition) do { \
    gint64 deadline = g_get_monotonic_time() + 3000000; \
    while (!(condition) && g_get_monotonic_time() < deadline) app_test_iterate(); \
    g_assert_true(condition); \
} while (0)

static void app_test_settle(void)
{
    gint64 deadline = g_get_monotonic_time() + 80000;
    while (g_get_monotonic_time() < deadline) app_test_iterate();
}

static void app_test_method(GDBusConnection *connection, const char *sender, const char *path,
                            const char *interface, const char *method, GVariant *parameters,
                            GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)parameters;
    AppFixture *fixture = data;
    GDBusMethodInvocation **pending = NULL;
    if (strcmp(method, "CallAudioDetails") == 0) pending = &fixture->audio;
    else if (strcmp(method, "SnapshotDetails") == 0) pending = &fixture->snapshot;
    else if (strcmp(method, "SetMuted") == 0) pending = &fixture->mute;
    if (pending) {
        g_assert_null(*pending);
        *pending = g_object_ref(invocation);
    } else if (strcmp(method, "Calls") == 0) {
        GVariantBuilder rows;
        g_variant_builder_init(&rows, G_VARIANT_TYPE("a(sssssb)"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(sa(sssssb))", "ready", &rows));
    } else if (strcmp(method, "BatteryStatus") == 0)
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "unavailable"));
    else if (strcmp(method, "SafeExit") == 0) {
        const char *action;
        g_variant_get(parameters, "(&s)", &action);
        g_strlcpy(fixture->exit_action, action, sizeof fixture->exit_action);
        fixture->exit_calls++;
        if (fixture->reject_exit)
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.freedesktop.DBus.Error.Failed", "Call state is active or unknown. End the call on the phone and try again.");
        else g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(method, "ResumeMonitoring") == 0)
        g_dbus_method_invocation_return_value(invocation, NULL);
    else g_assert_not_reached();
}

static void app_test_finish(GDBusMethodInvocation **pending, GVariant *reply_value)
{
    g_assert_nonnull(*pending);
    g_dbus_method_invocation_return_value(*pending, reply_value);
    g_clear_object(pending);
}

static GDBusConnection *app_test_connection(void)
{
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
        g_getenv("DBUS_SESSION_BUS_ADDRESS"), G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    g_assert_no_error(error);
    return connection;
}

static void app_test_setup(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    GError *error = NULL;
    g_assert_true(g_file_set_contents(tap_log, "", 0, &error));
    g_assert_no_error(error);
    fixture->service = app_test_connection();
    fixture->client = app_test_connection();
    GVariant *name = g_dbus_connection_call_sync(fixture->service, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.iblueconnect.Daemon", 0), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(name);
    g_variant_unref(name);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.iblueconnect.Daemon'>"
        "<method name='CallAudioDetails'><arg type='t' direction='out'/><arg type='t' direction='out'/>"
        "<arg type='b' direction='out'/></method>"
        "<method name='SnapshotDetails'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
        "<arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/>"
        "<arg type='s' direction='out'/></method>"
        "<method name='Calls'><arg type='s' direction='out'/><arg type='a(sssssb)' direction='out'/></method>"
        "<method name='SetMuted'><arg type='b' direction='in'/></method>"
        "<method name='BatteryStatus'><arg type='s' direction='out'/></method>"
        "<method name='SafeExit'><arg type='s' direction='in'/></method>"
        "<method name='ResumeMonitoring'/></interface></node>", &error);
    g_assert_no_error(error);
    static const GDBusInterfaceVTable vtable = { .method_call = app_test_method };
    fixture->object = g_dbus_connection_register_object(fixture->service,
        "/org/iblueconnect/Daemon", info->interfaces[0], &vtable, fixture, NULL, &error);
    g_assert_no_error(error);
    g_dbus_node_info_unref(info);
    MockApp *app = &fixture->app;
    app->daemon = g_dbus_proxy_new_sync(fixture->client,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
        NULL, "org.iblueconnect.Daemon", "/org/iblueconnect/Daemon", "org.iblueconnect.Daemon",
        NULL, &error);
    g_assert_no_error(error);
    static guint application_number;
    char *application_id = g_strdup_printf("org.iblueconnect.AppTest%u", application_number++);
    app->application = gtk_application_new(application_id, G_APPLICATION_NON_UNIQUE);
    g_free(application_id);
    g_assert_true(g_application_register(G_APPLICATION(app->application), NULL, &error));
    g_assert_no_error(error);
    mock_ui_init(&app->model);
    mock_ui_incoming(&app->model, -1);
    mock_ui_answer(&app->model);
    app->model.live = true;
    g_strlcpy(app->model.call_id, "/call-a", sizeof app->model.call_id);
    app->popup = mock_call_popup_new(app->application, popup_action, app);
    refresh_popup(app);
}

static void app_test_teardown(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    app->privacy_locked = true;
    app->state_epoch++;
    stop_meters(app);
    if (fixture->audio) app_test_finish(&fixture->audio, g_variant_new("(ttb)", (guint64)101, (guint64)202, FALSE));
    if (fixture->snapshot) app_test_finish(&fixture->snapshot, g_variant_new("(ssssss)", "ready", "", "idle", "unknown", "", ""));
    if (fixture->mute) app_test_finish(&fixture->mute, NULL);
    app_test_settle();
    while (gtk_application_get_windows(app->application))
        gtk_window_destroy(gtk_application_get_windows(app->application)->data);
    g_clear_pointer(&app->hidden_windows, g_ptr_array_unref);
    g_clear_object(&app->daemon);
    g_clear_object(&app->application);
    g_dbus_connection_unregister_object(fixture->service, fixture->object);
    g_dbus_connection_close_sync(fixture->service, NULL, NULL);
    g_dbus_connection_close_sync(fixture->client, NULL, NULL);
    g_clear_object(&fixture->service);
    g_clear_object(&fixture->client);
    app_test_settle();
}

static void app_test_audio_reply(AppFixture *fixture, guint64 incoming, guint64 outgoing, gboolean muted)
{
    follow_call_audio(&fixture->app);
    APP_AWAIT(fixture->audio != NULL);
    app_test_finish(&fixture->audio, g_variant_new("(ttb)", incoming, outgoing, muted));
    APP_AWAIT(!fixture->app.audio_pending);
}

static gboolean app_test_tapped(const char *target)
{
    char *contents = NULL;
    g_file_get_contents(tap_log, &contents, NULL, NULL);
    gboolean found = contents && strstr(contents, target);
    g_free(contents);
    return found;
}

static GtkWidget *app_test_find(GtkWidget *widget, const char *name)
{
    if (strcmp(gtk_widget_get_name(widget), name) == 0) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = app_test_find(child, name);
        if (found) return found;
    }
    return NULL;
}

static GBytes *app_test_wave_pixels(MockApp *app)
{
    app_test_settle();
    GtkWidget *wave = app_test_find(GTK_WIDGET(mock_call_popup_window(app->popup)), "voice-waveform");
    int width = gtk_widget_get_width(wave), height = gtk_widget_get_height(wave);
    g_assert_cmpint(width, >, 0);
    g_assert_cmpint(height, >, 0);
    GdkPaintable *paintable = gtk_widget_paintable_new(wave);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_assert_nonnull(node);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surface);
    GBytes *pixels = g_bytes_new(cairo_image_surface_get_data(surface),
        cairo_image_surface_get_stride(surface) * height);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    return pixels;
}

static void two_meters_follow_streams_and_mute(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    app_test_audio_reply(fixture, 101, 202, FALSE);
    g_assert_nonnull(app->voice);
    g_assert_nonnull(app->microphone);
    APP_AWAIT(app_test_tapped("--target 101 "));
    APP_AWAIT(app_test_tapped("--target 202 "));
    app_test_audio_reply(fixture, 111, 222, FALSE);
    g_assert_cmpuint(app->voice_serial, ==, 111);
    g_assert_cmpuint(app->microphone_serial, ==, 222);
    APP_AWAIT(app_test_tapped("--target 111 "));
    APP_AWAIT(app_test_tapped("--target 222 "));
    LiveVoiceMeter *receiving = app->voice;
    app_test_audio_reply(fixture, 111, 222, TRUE);
    g_assert_true(app->model.muted);
    g_assert_null(app->microphone);
    g_assert_cmpuint(app->microphone_serial, ==, 0);
    g_assert_true(app->voice == receiving);
    GBytes *flat = app_test_wave_pixels(app);
    on_microphone(1, app);
    GBytes *muted = app_test_wave_pixels(app);
    g_assert_true(g_bytes_equal(flat, muted));
    app->model.muted = false;
    app->mute_pending = true;
    refresh_popup(app);
    on_microphone(1, app);
    GBytes *pending = app_test_wave_pixels(app);
    g_assert_true(g_bytes_equal(flat, pending));
    app->mute_pending = false;
    app->model.muted = true;
    refresh_popup(app);
    g_bytes_unref(flat);
    g_bytes_unref(muted);
    g_bytes_unref(pending);
    on_voice(1, app);
    GtkWidget *avatar = app_test_find(GTK_WIDGET(mock_call_popup_window(app->popup)), "caller-ring");
    g_assert_true(gtk_widget_has_css_class(avatar, "speaking-them"));
}

static void stale_audio_does_not_restart_after_lock(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    follow_call_audio(app);
    APP_AWAIT(fixture->audio != NULL);
    on_privacy_changed(TRUE, app);
    app_test_finish(&fixture->audio, g_variant_new("(ttb)", (guint64)101, (guint64)202, FALSE));
    APP_AWAIT(!app->audio_pending);
    g_assert_null(app->voice);
    g_assert_null(app->microphone);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
}

static void stale_audio_does_not_cross_calls(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    follow_call_audio(app);
    APP_AWAIT(fixture->audio != NULL);
    g_strlcpy(app->model.call_id, "/call-b", sizeof app->model.call_id);
    app_test_finish(&fixture->audio, g_variant_new("(ttb)", (guint64)101, (guint64)202, FALSE));
    APP_AWAIT(!app->audio_pending);
    g_assert_null(app->voice);
    g_assert_null(app->microphone);
}

static void privacy_hides_windows_without_ignoring_call(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    app->main_window = mock_main_window_new(app->application, &app->model, main_action, app);
    gtk_window_present(GTK_WINDOW(app->main_window));
    GtkWidget *transient = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(transient), app->application);
    gtk_window_set_transient_for(GTK_WINDOW(transient), GTK_WINDOW(app->main_window));
    gtk_window_present(GTK_WINDOW(transient));
    MockUiModel before = app->model;
    on_privacy_changed(TRUE, app);
    g_assert_false(gtk_widget_get_visible(app->main_window));
    g_assert_false(gtk_widget_get_visible(transient));
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
    g_assert_cmpmem(&app->model, sizeof app->model, &before, sizeof before);
    popup_action("end", NULL, app);
    popup_action("mute", NULL, app);
    g_assert_cmpmem(&app->model, sizeof app->model, &before, sizeof before);
    refresh(app);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
    on_privacy_changed(FALSE, app);
    g_assert_true(app->privacy_refresh_pending);
    g_assert_true(gtk_widget_get_visible(app->main_window));
    g_assert_true(gtk_widget_get_visible(transient));
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
    APP_AWAIT(fixture->snapshot != NULL);
    app_test_finish(&fixture->snapshot, g_variant_new("(ssssss)", "ready", "", "idle", "unknown", "", ""));
    APP_AWAIT(!app->privacy_refresh_pending);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
}

static void stale_snapshot_does_not_replace_new_state(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    load_snapshot(app);
    APP_AWAIT(fixture->snapshot != NULL);
    on_privacy_changed(TRUE, app);
    app->model.call = MOCK_CALL_INCOMING;
    g_strlcpy(app->model.call_id, "/call-new", sizeof app->model.call_id);
    app_test_finish(&fixture->snapshot,
        g_variant_new("(ssssss)", "ready", "", "idle", "unknown", "", ""));
    app_test_settle();
    g_assert_cmpint(app->model.call, ==, MOCK_CALL_INCOMING);
    g_assert_cmpstr(app->model.call_id, ==, "/call-new");
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(app->popup))));
}

static void safe_exit_failure_preserves_app(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    app->main_window = mock_main_window_new(app->application, &app->model, main_action, app);
    gtk_window_present(GTK_WINDOW(app->main_window));
    fixture->reject_exit = TRUE;
    quit_action(NULL, NULL, app);
    g_assert_true(app->exit_pending);
    APP_AWAIT(!app->exit_pending);
    g_assert_cmpuint(fixture->exit_calls, ==, 1);
    g_assert_cmpstr(fixture->exit_action, ==, "quit");
    g_assert_true(gtk_widget_get_visible(app->main_window));
    g_assert_nonnull(app->exit_dialog);
    g_assert_true(gtk_widget_get_visible(app->exit_dialog));
    g_assert_cmpint(app->model.call, ==, MOCK_CALL_ACTIVE);
    gtk_window_destroy(GTK_WINDOW(app->exit_dialog));
    fixture->reject_exit = FALSE;
    request_safe_exit(app, MOCK_MAIN_DISCONNECT);
    APP_AWAIT(!app->exit_pending);
    g_assert_cmpuint(fixture->exit_calls, ==, 2);
    g_assert_cmpstr(fixture->exit_action, ==, "disconnect");
    g_assert_true(gtk_widget_get_visible(app->main_window));
    APP_AWAIT(fixture->snapshot != NULL);
    app_test_finish(&fixture->snapshot,
        g_variant_new("(ssssss)", "calling unavailable", "", "unavailable", "unknown", "", ""));
    app_test_settle();
}

/* Brightness of the controller's own background, sampled in the body margin beside the content. */
static int controller_background(MockApp *app)
{
    GtkWidget *window = GTK_WIDGET(mock_call_popup_window(app->popup));
    gtk_window_present(GTK_WINDOW(window));
    app_test_settle();
    GtkWidget *handle = gtk_widget_get_ancestor(app_test_find(window, "caller-ring"), GTK_TYPE_WINDOW_HANDLE);
    g_assert_nonnull(handle);
    graphene_rect_t box;
    g_assert_true(gtk_widget_compute_bounds(handle, window, &box));
    int width = gtk_widget_get_width(window), height = gtk_widget_get_height(window);
    GdkPaintable *paintable = gtk_widget_paintable_new(window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surface);
    int x = (int)box.origin.x + 5, y = (int)(box.origin.y + box.size.height / 2);
    const guchar *px = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4;
    int brightness = MAX(px[0], MAX(px[1], px[2]));
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    return brightness;
}

/* Black builds on GNOME Dark with near-black surfaces, for the controller as much as the main window. */
static void black_is_darker_than_dark(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    main_action(MOCK_MAIN_APPEARANCE, -1, "dark", app);
    int dark = controller_background(app);
    main_action(MOCK_MAIN_APPEARANCE, -1, "black", app);
    g_assert_cmpint(adw_style_manager_get_color_scheme(adw_style_manager_get_default()), ==, ADW_COLOR_SCHEME_FORCE_DARK);
    int black = controller_background(app);
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_BLACK);
    g_assert_cmpint(dark, >, 20);
    g_assert_cmpint(black, <, 16);
    main_action(MOCK_MAIN_APPEARANCE, -1, "dark", app);
    g_assert_cmpint(controller_background(app), ==, dark); /* Leaving Black restores GNOME Dark exactly. */
    main_action(MOCK_MAIN_APPEARANCE, -1, "system", app);
}

/* The choice is saved and applied to every window of the single app instance, controller included. */
static void appearance_is_saved_and_applied(AppFixture *fixture, gconstpointer ignored)
{
    (void)ignored;
    MockApp *app = &fixture->app;
    AdwStyleManager *style = adw_style_manager_get_default();
    main_action(MOCK_MAIN_APPEARANCE, -1, "dark", app);
    g_assert_cmpint(app->model.appearance, ==, MOCK_APPEARANCE_DARK);
    g_assert_cmpint(adw_style_manager_get_color_scheme(style), ==, ADW_COLOR_SCHEME_FORCE_DARK);
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_DARK);
    main_action(MOCK_MAIN_APPEARANCE, -1, "light", app);
    g_assert_cmpint(adw_style_manager_get_color_scheme(style), ==, ADW_COLOR_SCHEME_FORCE_LIGHT);
    main_action(MOCK_MAIN_APPEARANCE, -1, "system", app);
    g_assert_cmpint(adw_style_manager_get_color_scheme(style), ==, ADW_COLOR_SCHEME_DEFAULT);
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_SYSTEM);
}

int main(int argc, char **argv)
{
    char *config_home = g_dir_make_tmp("iblueconnect-app-config-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
    g_test_init(&argc, &argv, NULL);
    char *display = g_strdup(g_getenv("DISPLAY"));
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    if (display) g_setenv("DISPLAY", display, TRUE);
    g_free(display);
    GError *error = NULL;
    tap_directory = g_dir_make_tmp("iblueconnect-app-test-XXXXXX", &error);
    g_assert_no_error(error);
    tap_log = g_build_filename(tap_directory, "taps", NULL);
    char *program = g_build_filename(tap_directory, "pw-record", NULL);
    g_assert_true(g_file_set_contents(program,
        "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$IBLUECONNECT_TEST_TAPS\"\nexec /bin/sleep 30\n", -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(program, 0700), ==, 0);
    char *test_path = g_strconcat(tap_directory, ":", g_getenv("PATH"), NULL);
    g_setenv("PATH", test_path, TRUE);
    g_setenv("IBLUECONNECT_TEST_TAPS", tap_log, TRUE);
    adw_init();
    g_test_add("/app/two-meters-and-mute", AppFixture, NULL, app_test_setup, two_meters_follow_streams_and_mute, app_test_teardown);
    g_test_add("/app/stale-audio-lock", AppFixture, NULL, app_test_setup, stale_audio_does_not_restart_after_lock, app_test_teardown);
    g_test_add("/app/stale-audio-call", AppFixture, NULL, app_test_setup, stale_audio_does_not_cross_calls, app_test_teardown);
    g_test_add("/app/privacy-windows", AppFixture, NULL, app_test_setup, privacy_hides_windows_without_ignoring_call, app_test_teardown);
    g_test_add("/app/stale-snapshot", AppFixture, NULL, app_test_setup, stale_snapshot_does_not_replace_new_state, app_test_teardown);
    g_test_add("/app/safe-exit", AppFixture, NULL, app_test_setup, safe_exit_failure_preserves_app, app_test_teardown);
    g_test_add("/app/appearance", AppFixture, NULL, app_test_setup, appearance_is_saved_and_applied, app_test_teardown);
    g_test_add("/app/appearance-black", AppFixture, NULL, app_test_setup, black_is_darker_than_dark, app_test_teardown);
    int result = g_test_run();
    g_unlink(program);
    g_unlink(tap_log);
    g_rmdir(tap_directory);
    g_free(program);
    g_free(test_path);
    g_free(tap_log);
    g_free(tap_directory);
    /* GTK retains its session connection until process exit. Stop our private bus without waiting for it;
     * unref would finalize through g_test_dbus_down(), which waits for that connection forever. */
    g_test_dbus_stop(bus);
    return result;
}
