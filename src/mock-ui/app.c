#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#include "call-popup.h"
#include "live.h"
#include "main-view.h"
#include "mock-state.h"

typedef struct {
    GtkApplication *application;
    GtkWidget *main_window;
    GtkWidget *developer_window;
    MockCallPopup *popup;
    MockUiModel model;
    bool developer_mode;
    bool activated;
    const char *preview;
    bool background; /* Started by the daemon for a call: no main window on first activation. */
    bool snapshot_seen;
    GDBusProxy *daemon;
    LiveData *data;
    GVariant *pending_contacts;
    LiveVoiceMeter *voice; /* Taps the phone's voice while a call's audio is on the laptop. */
    bool audio_pending;
} MockApp;

static void activate(GApplication *application, gpointer data);
static void load_snapshot(MockApp *app);

/* A live app started for a call exits once the call is gone and no window needs it. */
static void quit_when_unused(MockApp *app)
{
    if (app->model.live && app->snapshot_seen && !app->main_window && !app->developer_window &&
        !app->model.popup_visible && app->model.call == MOCK_CALL_IDLE)
        g_application_quit(G_APPLICATION(app->application));
}

static void refresh_popup(MockApp *app)
{
    if (app->popup)
        mock_call_popup_update(app->popup, &app->model);
    quit_when_unused(app);
}

static void refresh(MockApp *app)
{
    if (app->main_window)
        mock_main_window_update(app->main_window);
    refresh_popup(app);
}

static void call_daemon(MockApp *app, const char *method, GVariant *args,
                        GAsyncReadyCallback done)
{
    if (!app->daemon) {
        if (args) g_variant_unref(g_variant_ref_sink(args));
        return;
    }
    g_dbus_proxy_call(app->daemon, method, args, G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000,
                      NULL, done, app);
}

static GVariant *reply(GObject *source, GAsyncResult *result)
{
    GError *error = NULL;
    GVariant *value = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (error) {
        g_printerr("IBlueConnect daemon request failed\n"); /* No caller details in logs. */
        g_error_free(error);
    }
    return value;
}

static void on_ignored_reply(GObject *source, GAsyncResult *result, gpointer data)
{
    GVariant *value = reply(source, result);
    if (value) g_variant_unref(value);
    (void)data;
}

static void on_call_reply(GObject *source, GAsyncResult *result, gpointer data)
{
    on_ignored_reply(source, result, data);
    load_snapshot(data);
}

static void on_answer(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (value) g_variant_unref(value);
    else if (app->model.call == MOCK_CALL_ACTIVE) {
        app->model.call = MOCK_CALL_INCOMING; /* Let the user retry or answer on the phone. */
        refresh_popup(app);
    }
}

static void on_voice(double level, gpointer data)
{
    MockApp *app = data;
    if (level >= 0) {
        if (app->popup) mock_call_popup_set_voice(app->popup, level);
        return;
    }
    live_voice_meter_stop(app->voice); /* Audio left the laptop; the next tick asks again. */
    app->voice = NULL;
    app->model.call_audio = false;
    if (app->popup) mock_call_popup_update(app->popup, &app->model);
}

static void on_call_audio(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GError *error = NULL;
    GVariant *value = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    app->audio_pending = false;
    if (error) g_error_free(error); /* Expected while the call's audio is on the iPhone. */
    app->model.call_audio = value && app->model.call == MOCK_CALL_ACTIVE;
    if (app->model.call_audio) {
        guint64 serial;
        gboolean muted;
        g_variant_get(value, "(tb)", &serial, &muted);
        app->model.muted = muted;
        if (!app->voice) app->voice = live_voice_meter_start(serial, on_voice, app);
    }
    if (value) g_variant_unref(value);
    if (app->popup) mock_call_popup_update(app->popup, &app->model);
}

/* Called from the 1 s tick and call changes, never from its own replies, so failures retry slowly.
 * While a live call is active, keep the voice meter and real mute state; drop both when it ends. */
static void follow_call_audio(MockApp *app)
{
    if (!app->model.live) return;
    if (app->model.call != MOCK_CALL_ACTIVE) {
        live_voice_meter_stop(app->voice);
        app->voice = NULL;
        app->model.call_audio = app->model.muted = false;
    } else if (!app->voice && !app->audio_pending && app->daemon) {
        app->audio_pending = true;
        g_dbus_proxy_call(app->daemon, "CallAudio", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000,
                          NULL, on_call_audio, app);
    }
}

static void on_muted(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (value) g_variant_unref(value);
    live_voice_meter_stop(app->voice); /* The next tick re-reads the real mute state and restarts the tap. */
    app->voice = NULL;
}

static void on_calls(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (value) {
        const char *status;
        g_variant_get_child(value, 0, "&s", &status);
        GVariant *rows = g_variant_get_child_value(value, 1);
        live_apply_calls(&app->model, status, rows);
        g_variant_unref(rows);
        g_variant_unref(value);
    } else {
        live_apply_calls(&app->model, NULL, NULL);
    }
    follow_call_audio(app);
    refresh(app);
}

static void on_snapshot(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (value) {
        const char *status, *id, *phase, *audio, *caller, *number;
        g_variant_get(value, "(&s&s&s&s&s&s)", &status, &id, &phase, &audio, &caller, &number);
        live_apply_snapshot(&app->model, status, id, phase, caller, number);
        g_variant_unref(value);
    } else {
        live_apply_snapshot(&app->model, NULL, "", "idle", "", "");
    }
    app->snapshot_seen = true;
    call_daemon(app, "Calls", NULL, on_calls);
}

static void on_battery(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    const char *status = NULL;
    if (value) g_variant_get(value, "(&s)", &status);
    live_apply_battery(&app->model, status);
    if (value) g_variant_unref(value);
    if (app->main_window) mock_main_window_update(app->main_window);
}

static void on_recents(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    GVariant *contacts = app->pending_contacts;
    app->pending_contacts = NULL;
    if (value && contacts) {
        GVariant *contact_rows = g_variant_get_child_value(contacts, 1);
        GVariant *recent_rows = g_variant_get_child_value(value, 1);
        LiveData *old = app->data;
        app->data = live_load(contact_rows, recent_rows, g_get_real_time() / G_USEC_PER_SEC);
        app->model.caller_index = mock_ui_contact_named(app->model.caller_name);
        refresh(app);
        live_free(old); /* Widgets copied what they show; the old set is unused now. */
        g_variant_unref(contact_rows);
        g_variant_unref(recent_rows);
    }
    if (value) g_variant_unref(value);
    if (contacts) g_variant_unref(contacts);
}

static void on_contacts(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (!value) return;
    const char *status;
    g_variant_get_child(value, 0, "&s", &status);
    live_apply_contacts_status(&app->model, status);
    if (app->pending_contacts) g_variant_unref(app->pending_contacts);
    app->pending_contacts = value;
    call_daemon(app, "Recents", NULL, on_recents);
}

static void load_snapshot(MockApp *app)
{
    call_daemon(app, "SnapshotDetails", NULL, on_snapshot);
    call_daemon(app, "BatteryStatus", NULL, on_battery);
}

static void load_lists(MockApp *app)
{
    call_daemon(app, "Contacts", NULL, on_contacts);
}

static void on_daemon_signal(GDBusProxy *proxy, const char *sender, const char *signal,
                             GVariant *parameters, gpointer data)
{
    (void)proxy;
    (void)sender;
    (void)parameters;
    if (g_strcmp0(signal, "Changed") == 0) load_snapshot(data);
    else if (g_strcmp0(signal, "ContactsChanged") == 0 || g_strcmp0(signal, "RecentsChanged") == 0)
        load_lists(data);
}

static void on_owner(GObject *proxy, GParamSpec *spec, gpointer data)
{
    (void)spec;
    MockApp *app = data;
    char *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
    if (owner) {
        load_snapshot(app);
        load_lists(app);
    } else {
        live_apply_snapshot(&app->model, NULL, "", "idle", "", "");
        app->snapshot_seen = true;
        refresh(app);
    }
    g_free(owner);
}

static void on_proxy(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    MockApp *app = data;
    app->daemon = g_dbus_proxy_new_for_bus_finish(result, NULL);
    if (!app->daemon) {
        live_apply_snapshot(&app->model, NULL, "", "idle", "", "");
        app->snapshot_seen = true;
        refresh(app);
        return;
    }
    g_signal_connect(app->daemon, "g-signal", G_CALLBACK(on_daemon_signal), app);
    g_signal_connect(app->daemon, "notify::g-name-owner", G_CALLBACK(on_owner), app);
    on_owner(G_OBJECT(app->daemon), NULL, app);
}

static void popup_action(const char *action, const char *argument, gpointer data)
{
    MockApp *app = data;
    if (app->model.live && *app->model.call_id) {
        const char *id = app->model.call_id;
        if (strcmp(action, "answer") == 0 && app->model.call == MOCK_CALL_INCOMING)
            call_daemon(app, "Answer", g_variant_new("(s)", id), on_answer);
        else if (strcmp(action, "decline") == 0 && app->model.call == MOCK_CALL_INCOMING)
            call_daemon(app, "Decline", g_variant_new("(s)", id), on_ignored_reply);
        else if (strcmp(action, "end") == 0)
            call_daemon(app, app->model.call_count > 1 || app->model.can_resume ? "EndCall" : "HangUp",
                        g_variant_new("(s)", id), on_call_reply);
        else if (strcmp(action, "hold") == 0 && app->model.can_hold)
            call_daemon(app, "Hold", g_variant_new("(s)", id), on_call_reply);
        else if (strcmp(action, "resume") == 0 && app->model.can_resume)
            call_daemon(app, "Resume", g_variant_new("(s)", app->model.held_call_id), on_call_reply);
        else if (strcmp(action, "merge") == 0 && app->model.can_merge)
            call_daemon(app, "Merge", NULL, on_call_reply);
        else if (strcmp(action, "end-conference") == 0 && app->model.can_end_conference)
            call_daemon(app, "EndConference", NULL, on_call_reply);
        else if (strcmp(action, "mute") == 0 && app->model.call_audio)
            call_daemon(app, "SetMuted", g_variant_new("(b)", !app->model.muted), on_muted);
        else if (strcmp(action, "tone") == 0 && argument && app->model.call == MOCK_CALL_ACTIVE && !app->model.call_held)
            call_daemon(app, "SendTone", g_variant_new("(ss)", id, argument), on_ignored_reply);
    }
    if (strcmp(action, "ignore") == 0)
        mock_ui_ignore(&app->model);
    else if (strcmp(action, "decline") == 0)
        mock_ui_decline(&app->model);
    else if (strcmp(action, "answer") == 0)
        mock_ui_answer(&app->model);
    else if (strcmp(action, "mute") == 0 && (!app->model.live || app->model.call_audio))
        mock_ui_toggle_mute(&app->model);
    else if (strcmp(action, "audio") == 0 && argument)
        mock_ui_select_audio(&app->model, (size_t)strtoul(argument, NULL, 10));
    else if (strcmp(action, "compact") == 0 && argument)
        mock_ui_set_compact(&app->model, strcmp(argument, "1") == 0);
    else if (strcmp(action, "end") == 0)
        mock_ui_end_call(&app->model);
    else if (strcmp(action, "dismiss") == 0)
        app->model.popup_visible = false;
    else if (strcmp(action, "open-app") == 0)
        activate(G_APPLICATION(app->application), app);
    /* Keypad tones are visual mock interactions until a call backend exists. */
    refresh_popup(app);
}

static void main_action(MockMainAction action, int contact_index,
                        const char *value, gpointer data)
{
    MockApp *app = data;
    if (app->model.live) {
        if (action == MOCK_MAIN_CALL && !value && contact_index >= 0 &&
            (size_t)contact_index < mock_contacts_count)
            value = mock_contacts[contact_index].numbers[0];
        if (action == MOCK_MAIN_CALL && value)
            call_daemon(app, app->model.can_add_call ? "AddCall" : "Dial",
                        g_variant_new("(s)", value), on_call_reply);
        else if (action == MOCK_MAIN_SYNC)
            call_daemon(app, "SyncContacts", NULL, on_ignored_reply);
        else if (action == MOCK_MAIN_RETRY)
            load_snapshot(app);
        else if (action == MOCK_MAIN_CONNECT) {
            GError *error = NULL;
            if (!g_spawn_command_line_async("gnome-control-center bluetooth", &error)) {
                g_printerr("Bluetooth settings could not open\n");
                g_error_free(error);
            }
        }
        return;
    }
    switch (action) {
    case MOCK_MAIN_CALL:
        if (contact_index < 0 && value)
            mock_ui_dial_number(&app->model, value);
        else
            mock_ui_outgoing(&app->model, contact_index);
        break;
    case MOCK_MAIN_CONNECT:
    case MOCK_MAIN_RETRY:
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_CONNECTED);
        app->model.calls_unavailable = false;
        break;
    case MOCK_MAIN_DISCONNECT:
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_DISCONNECTED);
        break;
    case MOCK_MAIN_SYNC:
        app->model.contact_sync_error = false;
        break;
    }
    refresh(app);
}

static gboolean tick(gpointer data)
{
    MockApp *app = data;
    mock_ui_tick(&app->model);
    follow_call_audio(app);
    if (app->model.live) refresh_popup(app); /* Timers and banners only; lists change on signals. */
    else refresh(app);
    return G_SOURCE_CONTINUE;
}

static void clear_main(GtkWidget *widget, gpointer data)
{
    MockApp *app = data;
    if (app->main_window == widget)
        app->main_window = NULL;
    quit_when_unused(app);
}

static void clear_developer(GtkWidget *widget, gpointer data)
{
    MockApp *app = data;
    if (app->developer_window == widget)
        app->developer_window = NULL;
}

static void developer_action(GtkButton *button, gpointer data)
{
    MockApp *app = data;
    const char *action = gtk_widget_get_name(GTK_WIDGET(button));
    if (strcmp(action, "connected") == 0)
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_CONNECTED);
    else if (strcmp(action, "disconnected") == 0)
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_DISCONNECTED);
    else if (strcmp(action, "connecting") == 0)
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_CONNECTING);
    else if (strcmp(action, "incoming") == 0) {
        mock_ui_finish_call(&app->model);
        mock_ui_incoming(&app->model, 0);
    } else if (strcmp(action, "unknown") == 0) {
        mock_ui_finish_call(&app->model);
        mock_ui_incoming(&app->model, -1);
    } else if (strcmp(action, "outgoing") == 0) {
        mock_ui_finish_call(&app->model);
        mock_ui_outgoing(&app->model, 0);
    } else if (strcmp(action, "call-connected") == 0)
        mock_ui_connect_call(&app->model);
    else if (strcmp(action, "caller-hung-up") == 0)
        mock_ui_end_call(&app->model);
    else if (strcmp(action, "connection-error") == 0)
        mock_ui_set_connection(&app->model, MOCK_CONNECTION_ERROR);
    else if (strcmp(action, "sync-error") == 0)
        app->model.contact_sync_error = true;
    else if (strcmp(action, "calls-unavailable") == 0)
        app->model.calls_unavailable = true;
    else if (strcmp(action, "empty-contacts") == 0)
        app->model.empty_contacts = !app->model.empty_contacts;
    else if (strcmp(action, "empty-recents") == 0)
        app->model.empty_recents = !app->model.empty_recents;
    else if (strcmp(action, "empty-favorites") == 0)
        app->model.empty_favorites = !app->model.empty_favorites;
    else if (strcmp(action, "empty-audio") == 0)
        app->model.empty_audio_devices = !app->model.empty_audio_devices;
    else if (strcmp(action, "theme-light") == 0)
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_LIGHT);
    else if (strcmp(action, "theme-dark") == 0)
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
    else if (strcmp(action, "theme-system") == 0)
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_DEFAULT);
    else if (strcmp(action, "quit") == 0)
        g_application_quit(G_APPLICATION(app->application));
    refresh(app);
}

static void add_developer_button(GtkWidget *box, const char *id, const char *label,
                                 MockApp *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_name(button, id);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    g_signal_connect(button, "clicked", G_CALLBACK(developer_action), app);
    gtk_box_append(GTK_BOX(box), button);
}

static void open_developer(MockApp *app)
{
    if (app->developer_window) {
        gtk_window_present(GTK_WINDOW(app->developer_window));
        return;
    }
    GtkWidget *window = gtk_application_window_new(app->application);
    app->developer_window = window;
    gtk_window_set_title(GTK_WINDOW(window), "IBlueConnect Mock Controls");
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 540);
    g_signal_connect(window, "destroy", G_CALLBACK(clear_developer), app);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_window_set_child(GTK_WINDOW(window), scroll);
    add_developer_button(box, "connected", "Simulate iPhone Connected", app);
    add_developer_button(box, "disconnected", "Simulate iPhone Disconnected", app);
    add_developer_button(box, "connecting", "Simulate Connecting", app);
    add_developer_button(box, "incoming", "Simulate Incoming Call", app);
    add_developer_button(box, "unknown", "Simulate Unknown Caller", app);
    add_developer_button(box, "outgoing", "Simulate Outgoing Call", app);
    add_developer_button(box, "call-connected", "Simulate Call Connected", app);
    add_developer_button(box, "caller-hung-up", "Simulate Caller Hung Up", app);
    add_developer_button(box, "connection-error", "Simulate Connection Error", app);
    add_developer_button(box, "sync-error", "Simulate Contact Sync Error", app);
    add_developer_button(box, "calls-unavailable", "Simulate Calls Unavailable", app);
    add_developer_button(box, "empty-contacts", "Toggle Empty Contacts", app);
    add_developer_button(box, "empty-recents", "Toggle Empty Recents", app);
    add_developer_button(box, "empty-favorites", "Toggle Empty Favorites", app);
    add_developer_button(box, "empty-audio", "Toggle Empty Audio Devices", app);
    add_developer_button(box, "theme-light", "Preview Light Appearance", app);
    add_developer_button(box, "theme-dark", "Preview Dark Appearance", app);
    add_developer_button(box, "theme-system", "Follow System Appearance", app);
    add_developer_button(box, "quit", "Quit Mock App", app);
    gtk_window_present(GTK_WINDOW(window));
}

static void quit_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void)action;
    (void)parameter;
    g_application_quit(G_APPLICATION(data));
}

static void activate(GApplication *application, gpointer data)
{
    MockApp *app = data;
    if (!app->activated) {
        app->activated = true;
        if (!app->model.live) g_application_hold(application);
        app->popup = mock_call_popup_new(GTK_APPLICATION(application), popup_action, app);
        g_timeout_add_seconds(1, tick, app);
        if (app->model.live)
            g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
                "org.iblueconnect.Daemon", "/org/iblueconnect/Daemon", "org.iblueconnect.Daemon",
                NULL, on_proxy, app);
        if (app->background) return;
    }
    if (app->model.live && app->model.call == MOCK_CALL_ACTIVE && !app->model.popup_visible) {
        app->model.popup_visible = true; /* Opening the app brings back a dismissed call controller. */
        refresh_popup(app);
    }
    if (app->preview) {
        if (strcmp(app->preview, "incoming") == 0)
            mock_ui_incoming(&app->model, 0);
        else if (strcmp(app->preview, "active") == 0 || strcmp(app->preview, "compact") == 0) {
            mock_ui_incoming(&app->model, 0);
            mock_ui_answer(&app->model);
            if (strcmp(app->preview, "compact") == 0)
                mock_ui_set_compact(&app->model, true);
        } else if (strcmp(app->preview, "outgoing") == 0)
            mock_ui_outgoing(&app->model, 0);
        refresh(app);
        return;
    }
    if (!app->main_window) {
        app->main_window = mock_main_window_new(GTK_APPLICATION(application), &app->model,
                                                 main_action, app);
        g_signal_connect(app->main_window, "destroy", G_CALLBACK(clear_main), app);
    }
    gtk_window_present(GTK_WINDOW(app->main_window));
    if (app->developer_mode)
        open_developer(app);
}

static int self_test(void)
{
    MockApp app = {0};
    adw_init();
    app.application = gtk_application_new("org.iblueconnect.MockSelfTest", 0);
    g_assert_true(g_application_register(G_APPLICATION(app.application), NULL, NULL));
    mock_ui_init(&app.model);
    app.popup = mock_call_popup_new(app.application, popup_action, &app);
    app.main_window = mock_main_window_new(app.application, &app.model, main_action, &app);
    mock_ui_incoming(&app.model, 0);
    refresh(&app);
    GtkWindow *same_window = mock_call_popup_window(app.popup);
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    popup_action("ignore", NULL, &app);
    g_assert_cmpint(app.model.call, ==, MOCK_CALL_INCOMING_IGNORED);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    mock_ui_finish_call(&app.model);
    mock_ui_incoming(&app.model, -1);
    refresh(&app);
    popup_action("answer", NULL, &app);
    g_assert_cmpint(app.model.call, ==, MOCK_CALL_ACTIVE);
    g_assert_true(mock_call_popup_window(app.popup) == same_window);
    gtk_window_destroy(GTK_WINDOW(app.main_window));
    app.main_window = NULL;
    refresh(&app);
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    app.activated = true;
    popup_action("open-app", NULL, &app);
    g_assert_nonnull(app.main_window);
    g_assert_true(gtk_widget_get_visible(app.main_window));
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    mock_ui_tick(&app.model);
    g_assert_cmpuint(app.model.elapsed_seconds, ==, 1);
    popup_action("mute", NULL, &app);
    g_assert_true(app.model.muted);
    popup_action("audio", "1", &app);
    g_assert_cmpuint(app.model.audio_index, ==, 1);
    popup_action("end", NULL, &app);
    g_assert_cmpint(app.model.call, ==, MOCK_CALL_ENDING);
    mock_ui_finish_call(&app.model);
    g_assert_false(app.model.popup_visible);
    gtk_window_destroy(same_window);
    g_object_unref(app.application);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    MockApp app = {0};
    bool mock = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--developer") == 0)
            app.developer_mode = mock = true;
        else if (strcmp(argv[i], "--mock") == 0)
            mock = true;
        else if (strcmp(argv[i], "--background") == 0)
            app.background = true;
        else if (strncmp(argv[i], "--preview=", 10) == 0 &&
                 (strcmp(argv[i] + 10, "incoming") == 0 ||
                  strcmp(argv[i] + 10, "active") == 0 ||
                  strcmp(argv[i] + 10, "compact") == 0 ||
                  strcmp(argv[i] + 10, "outgoing") == 0)) {
            app.preview = argv[i] + 10;
            mock = true;
        } else {
            g_printerr("Usage: iblueconnect [--background|--mock|--developer|--self-test|"
                       "--preview=incoming|active|compact|outgoing]\n");
            return 2;
        }
    }
    mock_ui_init(&app.model);
    app.model.live = !mock;
    if (app.model.live) {
        app.model.battery[0] = '\0';
        app.model.sync_label[0] = '\0';
        mock_ui_set_data(NULL, 0, NULL, 0, NULL, 0);
        app.model.connection = MOCK_CONNECTION_CONNECTING;
    }
    adw_init();
    app.application = gtk_application_new(app.model.live ? "org.iblueconnect.IBlueConnect"
                                                         : "org.iblueconnect.Mock",
                                          G_APPLICATION_DEFAULT_FLAGS);
    if (app.background) {
        /* The daemon asks for the call controller; a running instance already follows the call. */
        if (!g_application_register(G_APPLICATION(app.application), NULL, NULL) ||
            g_application_get_is_remote(G_APPLICATION(app.application))) {
            g_object_unref(app.application);
            return 0;
        }
    }
    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(quit_action), app.application);
    g_action_map_add_action(G_ACTION_MAP(app.application), G_ACTION(quit));
    g_object_unref(quit);
    const char *quit_accels[] = { "<Primary>q", NULL };
    gtk_application_set_accels_for_action(app.application, "app.quit", quit_accels);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(app.application), 1, argv);
    live_voice_meter_stop(app.voice);
    live_free(app.data);
    g_clear_object(&app.daemon);
    g_object_unref(app.application);
    return status;
}
