#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#include "call-popup.h"
#include "live.h"
#include "main-view.h"
#include "mock-state.h"
#include "privacy.h"
#include "appearance.h"

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
    LiveVoiceMeter *microphone;
    guint64 voice_serial, microphone_serial;
    guint audio_epoch;
    bool audio_pending;
    bool mute_pending;
    bool privacy_locked;
    bool privacy_refresh_pending;
    PrivacyWatch *privacy;
    GPtrArray *hidden_windows;
    guint state_epoch;
    bool exit_pending;
    GtkWidget *exit_dialog;
} MockApp;

typedef struct { MockApp *app; guint epoch; } StateRequest;

static void activate(GApplication *application, gpointer data);
static void load_snapshot(MockApp *app);

static void on_privacy_changed(gboolean locked, gpointer data);
static void request_safe_exit(MockApp *app, MockMainAction action);

/* A live app started for a call exits once the call is gone and no window needs it. */
static void quit_when_unused(MockApp *app)
{
    if (app->model.live && app->snapshot_seen && !app->main_window && !app->developer_window &&
        !app->model.popup_visible && app->model.call == MOCK_CALL_IDLE)
        g_application_quit(G_APPLICATION(app->application));
}

static void refresh_popup(MockApp *app)
{
    if (app->popup) {
        MockUiModel visible = app->model;
        if (app->privacy_locked || app->privacy_refresh_pending) {
            visible.popup_visible = false;
            visible.call_audio = false;
            visible.muted = true;
        }
        mock_call_popup_update(app->popup, &visible);
    }
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

static void on_siri(GObject *source, GAsyncResult *result, gpointer data);

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
    app->voice_serial = 0;
    if (app->popup) mock_call_popup_set_voice(app->popup, 0);
}

static void on_microphone(double level, gpointer data)
{
    MockApp *app = data;
    if (app->popup)
        mock_call_popup_set_microphone_level(app->popup,
            level >= 0 && !app->model.muted && !app->mute_pending && !app->privacy_locked ? level : -1);
    if (level < 0) {
        live_voice_meter_stop(app->microphone);
        app->microphone = NULL;
        app->microphone_serial = 0;
    }
}

static void stop_meters(MockApp *app)
{
    live_voice_meter_stop(app->voice);
    live_voice_meter_stop(app->microphone);
    app->voice = app->microphone = NULL;
    app->voice_serial = app->microphone_serial = 0;
    app->audio_epoch++;
    if (app->popup) {
        mock_call_popup_set_voice(app->popup, 0);
        mock_call_popup_set_microphone_level(app->popup, -1);
    }
}

typedef struct { MockApp *app; guint epoch; char id[128]; } AudioRequest;

static void on_call_audio(GObject *source, GAsyncResult *result, gpointer data)
{
    AudioRequest *request = data;
    MockApp *app = request->app;
    GError *error = NULL;
    GVariant *value = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    app->audio_pending = false;
    if (error) g_error_free(error); /* Expected while the call's audio is on the iPhone. */
    if (request->epoch != app->audio_epoch || strcmp(request->id, app->model.call_id) != 0 ||
        app->privacy_locked || app->privacy_refresh_pending || app->mute_pending) {
        if (value) g_variant_unref(value);
        g_free(request);
        return;
    }
    g_free(request);
    app->model.call_audio = value && app->model.call == MOCK_CALL_ACTIVE && !app->model.call_held;
    if (app->model.call_audio) {
        guint64 serial, microphone;
        gboolean muted;
        g_variant_get(value, "(ttb)", &serial, &microphone, &muted);
        app->model.muted = muted;
        if (serial != app->voice_serial) {
            live_voice_meter_stop(app->voice);
            app->voice = live_voice_meter_start(serial, on_voice, app);
            app->voice_serial = app->voice ? serial : 0;
        }
        if (muted) microphone = 0;
        if (microphone != app->microphone_serial) {
            live_voice_meter_stop(app->microphone);
            app->microphone = microphone ? live_voice_meter_start(microphone, on_microphone, app) : NULL;
            app->microphone_serial = app->microphone ? microphone : 0;
        }
        if (!microphone && app->popup) mock_call_popup_set_microphone_level(app->popup, -1);
    } else {
        stop_meters(app);
    }
    if (value) g_variant_unref(value);
    refresh_popup(app);
}

/* Called from the 1 s tick and call changes, never from its own replies, so failures retry slowly.
 * While a live call is active, keep the voice meter and real mute state; drop both when it ends. */
static void follow_call_audio(MockApp *app)
{
    if (!app->model.live) return;
    if (app->model.call != MOCK_CALL_ACTIVE || app->model.call_held || app->privacy_locked || app->privacy_refresh_pending) {
        stop_meters(app);
        app->model.call_audio = app->model.muted = false;
    } else if (!app->audio_pending && !app->mute_pending && app->daemon) {
        app->audio_pending = true;
        AudioRequest *request = g_new0(AudioRequest, 1);
        request->app = app;
        request->epoch = app->audio_epoch;
        g_strlcpy(request->id, app->model.call_id, sizeof request->id);
        g_dbus_proxy_call(app->daemon, "CallAudioDetails", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000,
                          NULL, on_call_audio, request);
    }
}

static void on_muted(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GVariant *value = reply(source, result);
    if (value) g_variant_unref(value);
    app->mute_pending = false;
    app->audio_epoch++;
    follow_call_audio(app);
}

static void on_calls(GObject *source, GAsyncResult *result, gpointer data)
{
    StateRequest *request = data;
    MockApp *app = request->app;
    GVariant *value = reply(source, result);
    gboolean current = request->epoch == app->state_epoch;
    g_free(request);
    if (!current) {
        if (value) g_variant_unref(value);
        return;
    }
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
    if (!app->privacy_locked) {
        if (app->privacy_refresh_pending && !app->model.call_count &&
            (app->model.call == MOCK_CALL_ENDED || app->model.call == MOCK_CALL_DECLINED))
            mock_ui_finish_call(&app->model);
        app->privacy_refresh_pending = false;
    }
    follow_call_audio(app);
    refresh(app);
}

static void on_snapshot(GObject *source, GAsyncResult *result, gpointer data)
{
    StateRequest *request = data;
    MockApp *app = request->app;
    GVariant *value = reply(source, result);
    if (request->epoch != app->state_epoch) {
        if (value) g_variant_unref(value);
        g_free(request);
        return;
    }
    if (value) {
        const char *status, *id, *phase, *audio, *caller, *number;
        g_variant_get(value, "(&s&s&s&s&s&s)", &status, &id, &phase, &audio, &caller, &number);
        live_apply_snapshot(&app->model, status, id, phase, caller, number);
        g_variant_unref(value);
    } else {
        live_apply_snapshot(&app->model, NULL, "", "idle", "", "");
    }
    app->snapshot_seen = true;
    g_dbus_proxy_call(app->daemon, "Calls", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000,
                      NULL, on_calls, request);
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
    if (!app->daemon) return;
    StateRequest *request = g_new(StateRequest, 1);
    *request = (StateRequest){ app, app->state_epoch };
    g_dbus_proxy_call(app->daemon, "SnapshotDetails", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000,
                      NULL, on_snapshot, request);
    call_daemon(app, "BatteryStatus", NULL, on_battery);
}

static void remember_hidden_window(MockApp *app, GtkWidget *window)
{
    if (!app->hidden_windows) app->hidden_windows = g_ptr_array_new_with_free_func(g_object_unref);
    if (!g_ptr_array_find(app->hidden_windows, window, NULL))
        g_ptr_array_add(app->hidden_windows, g_object_ref(window));
    gtk_widget_set_visible(window, FALSE);
}

static void on_privacy_changed(gboolean locked, gpointer data)
{
    MockApp *app = data;
    app->privacy_locked = locked;
    app->state_epoch++;
    stop_meters(app); /* Stops read-only meters; the actual call audio is untouched. */
    if (locked) {
        for (GList *item = gtk_application_get_windows(app->application); item; item = item->next) {
            GtkWidget *window = item->data;
            if (app->popup && window == GTK_WIDGET(mock_call_popup_window(app->popup)))
                gtk_widget_set_visible(window, FALSE);
            else if (gtk_widget_get_visible(window)) remember_hidden_window(app, window);
        }
    } else {
        app->privacy_refresh_pending = app->model.live;
        if (app->hidden_windows) {
            GList *windows = gtk_application_get_windows(app->application);
            for (guint i = 0; i < app->hidden_windows->len; i++) {
                GtkWidget *window = g_ptr_array_index(app->hidden_windows, i);
                if (g_list_find(windows, window)) gtk_widget_set_visible(window, TRUE);
            }
            g_ptr_array_set_size(app->hidden_windows, 0);
        }
        if (app->model.live) load_snapshot(app);
    }
    refresh_popup(app);
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
    if (app->privacy_locked || app->privacy_refresh_pending) return;
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
        else if (strcmp(action, "swap") == 0 && app->model.can_swap)
            call_daemon(app, "Swap", NULL, on_call_reply);
        else if (strcmp(action, "end-line") == 0 && argument && *argument)
            call_daemon(app, "EndCall", g_variant_new("(s)", argument), on_call_reply);
        else if (strcmp(action, "private") == 0 && argument && *argument && app->model.can_private)
            call_daemon(app, "PrivateChat", g_variant_new("(s)", argument), on_call_reply);
        else if (strcmp(action, "hold-and-answer") == 0 && app->model.can_hold_and_answer)
            call_daemon(app, "HoldAndAnswer", g_variant_new("(s)", app->model.waiting_call_id), on_call_reply);
        else if (strcmp(action, "release-and-answer") == 0 && app->model.can_release_and_answer)
            call_daemon(app, "ReleaseAndAnswer", g_variant_new("(s)", app->model.waiting_call_id), on_call_reply);
        else if (strcmp(action, "mute") == 0 && app->model.call_audio) {
            if (app->mute_pending) return;
            app->mute_pending = true;
            app->audio_epoch++;
            live_voice_meter_stop(app->microphone);
            app->microphone = NULL;
            app->microphone_serial = 0;
            if (app->popup) mock_call_popup_set_microphone_level(app->popup, -1);
            call_daemon(app, "SetMuted", g_variant_new("(b)", !app->model.muted), on_muted);
        }
        else if (strcmp(action, "tone") == 0 && argument && app->model.call == MOCK_CALL_ACTIVE && !app->model.call_held)
            call_daemon(app, "SendTone", g_variant_new("(ss)", id, argument), on_ignored_reply);
    }
    if (!app->model.live && mock_ui_calls_action(&app->model, action, argument))
        ; /* Mock call rows changed. */
    else if (strcmp(action, "ignore") == 0)
        mock_ui_ignore(&app->model);
    else if (strcmp(action, "ignore-waiting") == 0)
        mock_ui_ignore_waiting(&app->model); /* Hides the banner only; the phone keeps ringing. */
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
    else if (strcmp(action, "add-call") == 0) {
        activate(G_APPLICATION(app->application), app);
        if (app->main_window) mock_main_window_show_dial(app->main_window);
    }
    /* Keypad tones are visual mock interactions until a call backend exists. */
    refresh_popup(app);
}

static void clear_exit_dialog(GtkWidget *widget, gpointer data)
{
    MockApp *app = data;
    if (app->exit_dialog == widget) app->exit_dialog = NULL;
}

static GtkWidget *exit_dialog(MockApp *app, const char *title, const char *message)
{
    if (app->exit_dialog) gtk_window_destroy(GTK_WINDOW(app->exit_dialog));
    GtkWindow *parent = app->main_window ? GTK_WINDOW(app->main_window) :
                        app->popup ? mock_call_popup_window(app->popup) : NULL;
    GtkWidget *dialog = adw_message_dialog_new(parent, title, message);
    gtk_window_set_application(GTK_WINDOW(dialog), app->application);
    app->exit_dialog = dialog;
    g_signal_connect(dialog, "destroy", G_CALLBACK(clear_exit_dialog), app);
    return dialog;
}

static void show_exit_dialog(MockApp *app, GtkWidget *dialog)
{
    if (app->privacy_locked) remember_hidden_window(app, dialog);
    else gtk_window_present(GTK_WINDOW(dialog));
}

typedef struct { MockApp *app; MockMainAction action; } ExitRequest;

static void on_safe_exit(GObject *source, GAsyncResult *result, gpointer data)
{
    ExitRequest *request = data;
    MockApp *app = request->app;
    GError *error = NULL;
    GVariant *value = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    app->exit_pending = false;
    if (value) {
        g_variant_unref(value);
        stop_meters(app);
        if (request->action == MOCK_MAIN_QUIT) g_application_quit(G_APPLICATION(app->application));
        else {
            load_snapshot(app);
            if (request->action == MOCK_MAIN_FORGET) load_lists(app);
        }
    } else {
        if (error) g_dbus_error_strip_remote_error(error);
        GtkWidget *dialog = exit_dialog(app, "Action paused",
            error ? error->message : "The background service could not verify that exiting is safe.");
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close");
        adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "close");
        show_exit_dialog(app, dialog);
    }
    g_clear_error(&error);
    g_free(request);
}

static void request_safe_exit(MockApp *app, MockMainAction action)
{
    if (app->privacy_locked || app->exit_pending) return;
    if (!app->model.live) {
        if (action == MOCK_MAIN_QUIT) g_application_quit(G_APPLICATION(app->application));
        else mock_ui_set_connection(&app->model, MOCK_CONNECTION_DISCONNECTED);
        return;
    }
    ExitRequest *request = g_new(ExitRequest, 1);
    *request = (ExitRequest){ app, action };
    if (!app->daemon) {
        g_free(request);
        GtkWidget *dialog = exit_dialog(app, "Action paused", "The background service is unavailable. Reopen IBlueConnect and try again.");
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close");
        show_exit_dialog(app, dialog);
        return;
    }
    app->exit_pending = true;
    const char *name = action == MOCK_MAIN_QUIT ? "quit" : action == MOCK_MAIN_FORGET ? "forget" : "disconnect";
    g_dbus_proxy_call(app->daemon, "SafeExit", g_variant_new("(s)", name), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      10000, NULL, on_safe_exit, request);
}

static void on_siri(GObject *source, GAsyncResult *result, gpointer data)
{
    MockApp *app = data;
    GError *error = NULL;
    GVariant *value = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (value) g_variant_unref(value);
    else {
        g_dbus_error_strip_remote_error(error);
        GtkWidget *dialog = exit_dialog(app, "Siri unavailable", error->message);
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close");
        adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "close");
        show_exit_dialog(app, dialog);
    }
    g_clear_error(&error);
}

static void on_forget_response(AdwMessageDialog *dialog, const char *response, gpointer data)
{
    (void)dialog;
    if (strcmp(response, "forget") == 0) request_safe_exit(data, MOCK_MAIN_FORGET);
}

static void main_action(MockMainAction action, int contact_index,
                        const char *value, gpointer data)
{
    MockApp *app = data;
    if (app->privacy_locked) return;
    if (action == MOCK_MAIN_APPEARANCE) {
        app->model.appearance = appearance_parse(value);
        appearance_apply(app->model.appearance);
        GError *error = NULL;
        if (!appearance_save(app->model.appearance, &error)) { /* Still applied for this session. */
            g_warning("Appearance not saved: %s", error->message);
            g_error_free(error);
        }
        return;
    }
    if (action == MOCK_MAIN_QUIT || action == MOCK_MAIN_DISCONNECT) {
        request_safe_exit(app, action);
        return;
    }
    if (action == MOCK_MAIN_FORGET) {
        GtkWidget *dialog = exit_dialog(app, "Forget this phone?",
            "Disconnect the phone and remove IBlueConnect's cached contacts and call history. Bluetooth pairing is kept. An ongoing or uncertain call blocks this action.");
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel");
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "forget", "Forget phone");
        adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "forget", ADW_RESPONSE_DESTRUCTIVE);
        adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
        adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
        g_signal_connect(dialog, "response", G_CALLBACK(on_forget_response), app);
        show_exit_dialog(app, dialog);
        return;
    }
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
        else if (action == MOCK_MAIN_RESUME)
            call_daemon(app, "ResumeMonitoring", NULL, on_call_reply);
        else if (action == MOCK_MAIN_SIRI)
            call_daemon(app, "SetVoiceAssistant", g_variant_new("(b)", TRUE), on_siri);
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
        if (!value && contact_index >= 0 && (size_t)contact_index < mock_contacts_count)
            value = mock_contacts[contact_index].numbers[0];
        if (app->model.call_count && mock_ui_calls_action(&app->model, "add", value))
            break;
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
    case MOCK_MAIN_QUIT:
    case MOCK_MAIN_FORGET:
    case MOCK_MAIN_RESUME:
    case MOCK_MAIN_SIRI:
    case MOCK_MAIN_APPEARANCE:
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

static void clear_popup(GtkWidget *widget, gpointer data)
{
    (void)widget;
    MockApp *app = data;
    app->popup = NULL;
    stop_meters(app);
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
    add_developer_button(box, "waiting-call", "Simulate Call Waiting", app);
    add_developer_button(box, "two-calls", "Simulate Two Calls", app);
    add_developer_button(box, "conference", "Simulate Conference", app);
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
    request_safe_exit(data, MOCK_MAIN_QUIT);
}

static void activate(GApplication *application, gpointer data)
{
    MockApp *app = data;
    if (!app->activated) {
        app->activated = true;
        if (!app->model.live) g_application_hold(application);
        app->popup = mock_call_popup_new(GTK_APPLICATION(application), popup_action, app);
        g_signal_connect(mock_call_popup_window(app->popup), "destroy", G_CALLBACK(clear_popup), app);
        g_timeout_add_seconds(1, tick, app);
        if (app->model.live) {
            app->privacy_locked = true;
            app->privacy = privacy_watch_start(on_privacy_changed, app);
        }
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
        else
            mock_ui_simulate_calls(&app->model, app->preview);
        refresh(app);
        return;
    }
    if (!app->main_window) {
        app->main_window = mock_main_window_new(GTK_APPLICATION(application), &app->model,
                                                 main_action, app);
        g_signal_connect(app->main_window, "destroy", G_CALLBACK(clear_main), app);
    }
    if (app->privacy_locked) remember_hidden_window(app, app->main_window);
    else gtk_window_present(GTK_WINDOW(app->main_window));
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
    on_privacy_changed(TRUE, &app);
    g_assert_false(gtk_widget_get_visible(app.main_window));
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    refresh(&app);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(same_window)));
    popup_action("end", NULL, &app);
    g_assert_cmpint(app.model.call, ==, MOCK_CALL_ACTIVE);
    on_privacy_changed(FALSE, &app);
    g_assert_true(gtk_widget_get_visible(app.main_window));
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(same_window)));
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
                  strcmp(argv[i] + 10, "outgoing") == 0 ||
                  strcmp(argv[i] + 10, "waiting") == 0 ||
                  strcmp(argv[i] + 10, "two-calls") == 0 ||
                  strcmp(argv[i] + 10, "conference") == 0)) {
            app.preview = argv[i] + 10;
            mock = true;
        } else {
            g_printerr("Usage: iblueconnect [--background|--mock|--developer|--self-test|"
                       "--preview=incoming|active|compact|outgoing|waiting|two-calls|conference]\n");
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
    app.model.appearance = appearance_load(); /* Same choice for the main window and the call controller. */
    appearance_apply(app.model.appearance);
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
    g_signal_connect(quit, "activate", G_CALLBACK(quit_action), &app);
    g_action_map_add_action(G_ACTION_MAP(app.application), G_ACTION(quit));
    g_object_unref(quit);
    const char *quit_accels[] = { "<Primary>q", NULL };
    gtk_application_set_accels_for_action(app.application, "app.quit", quit_accels);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(app.application), 1, argv);
    privacy_watch_stop(app.privacy);
    stop_meters(&app);
    g_clear_pointer(&app.hidden_windows, g_ptr_array_unref);
    live_free(app.data);
    g_clear_object(&app.daemon);
    g_object_unref(app.application);
    return status;
}
