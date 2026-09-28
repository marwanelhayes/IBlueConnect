#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#include "call-popup.h"
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
} MockApp;

static void refresh(MockApp *app)
{
    if (app->main_window)
        mock_main_window_update(app->main_window);
    if (app->popup)
        mock_call_popup_update(app->popup, &app->model);
}

static void popup_action(const char *action, const char *argument, gpointer data)
{
    MockApp *app = data;
    if (strcmp(action, "ignore") == 0)
        mock_ui_ignore(&app->model);
    else if (strcmp(action, "decline") == 0)
        mock_ui_decline(&app->model);
    else if (strcmp(action, "answer") == 0)
        mock_ui_answer(&app->model);
    else if (strcmp(action, "mute") == 0)
        mock_ui_toggle_mute(&app->model);
    else if (strcmp(action, "audio") == 0 && argument)
        mock_ui_select_audio(&app->model, (size_t)strtoul(argument, NULL, 10));
    else if (strcmp(action, "compact") == 0 && argument)
        mock_ui_set_compact(&app->model, strcmp(argument, "1") == 0);
    else if (strcmp(action, "end") == 0)
        mock_ui_end_call(&app->model);
    else if (strcmp(action, "dismiss") == 0)
        app->model.popup_visible = false;
    /* Keypad tones are visual mock interactions until a call backend exists. */
    refresh(app);
}

static void main_action(MockMainAction action, int contact_index,
                        const char *value, gpointer data)
{
    MockApp *app = data;
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
    refresh(app);
    return G_SOURCE_CONTINUE;
}

static void clear_main(GtkWidget *widget, gpointer data)
{
    MockApp *app = data;
    if (app->main_window == widget)
        app->main_window = NULL;
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
        g_application_hold(application);
        app->popup = mock_call_popup_new(GTK_APPLICATION(application), popup_action, app);
        g_timeout_add_seconds(1, tick, app);
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
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--developer") == 0)
            app.developer_mode = true;
        else if (strncmp(argv[i], "--preview=", 10) == 0 &&
                 (strcmp(argv[i] + 10, "incoming") == 0 ||
                  strcmp(argv[i] + 10, "active") == 0 ||
                  strcmp(argv[i] + 10, "compact") == 0 ||
                  strcmp(argv[i] + 10, "outgoing") == 0))
            app.preview = argv[i] + 10;
        else {
            g_printerr("Usage: iblueconnect-mock [--developer|--self-test|--preview=incoming|active|compact|outgoing]\n");
            return 2;
        }
    }
    mock_ui_init(&app.model);
    adw_init();
    app.application = gtk_application_new("org.iblueconnect.Mock", G_APPLICATION_DEFAULT_FLAGS);
    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(quit_action), app.application);
    g_action_map_add_action(G_ACTION_MAP(app.application), G_ACTION(quit));
    g_object_unref(quit);
    const char *quit_accels[] = { "<Primary>q", NULL };
    gtk_application_set_accels_for_action(app.application, "app.quit", quit_accels);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(app.application), 1, argv);
    g_object_unref(app.application);
    return status;
}
