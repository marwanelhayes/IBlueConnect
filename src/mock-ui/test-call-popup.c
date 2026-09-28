#include "call-popup.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    MockUiModel model;
    MockCallPopup *popup;
    const char *last_action;
    const char *last_argument;
} Fixture;

static GtkApplication *app;

static GtkWidget *find(GtkWidget *widget, const char *name)
{
    if (strcmp(gtk_widget_get_name(widget), name) == 0) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *match = find(child, name);
        if (match) return match;
    }
    return NULL;
}

static void action(const char *name, const char *argument, gpointer data)
{
    Fixture *f = data;
    f->last_action = name;
    f->last_argument = argument;
    if (strcmp(name, "ignore") == 0) mock_ui_ignore(&f->model);
    else if (strcmp(name, "decline") == 0) mock_ui_decline(&f->model);
    else if (strcmp(name, "answer") == 0) mock_ui_answer(&f->model);
    else if (strcmp(name, "mute") == 0) mock_ui_toggle_mute(&f->model);
    else if (strcmp(name, "end") == 0) mock_ui_end_call(&f->model);
    else if (strcmp(name, "compact") == 0) mock_ui_set_compact(&f->model, argument[0] == '1');
    else if (strcmp(name, "audio") == 0) mock_ui_select_audio(&f->model, (size_t)atoi(argument));
    mock_call_popup_update(f->popup, &f->model);
}

static void click(Fixture *f, const char *name)
{
    GtkWidget *button = find(GTK_WIDGET(mock_call_popup_window(f->popup)), name);
    g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
}

static void assert_identity_on_screen(GtkWindow *window)
{
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    graphene_rect_t bounds;
    g_assert_true(gtk_widget_compute_bounds(find(GTK_WIDGET(window), "caller-identity"),
                                            GTK_WIDGET(window), &bounds));
    g_assert_cmpfloat(bounds.origin.y, >=, 0);
    g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=,
                      gtk_widget_get_height(GTK_WIDGET(window)));
}

static void incoming_flow(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window)));
    assert_identity_on_screen(window);
    GtkWidget *avatar = find(GTK_WIDGET(window), "caller-avatar");
    g_assert_true(ADW_IS_AVATAR(avatar));
    g_assert_cmpstr(adw_avatar_get_text(ADW_AVATAR(avatar)), ==, "Ahmed Mohamed");
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "incoming-actions")));
    click(&f, "answer");
    GtkWidget *mute_content = gtk_button_get_child(GTK_BUTTON(find(GTK_WIDGET(window), "mute")));
    g_assert_true(ADW_IS_BUTTON_CONTENT(mute_content));
    g_assert_cmpstr(adw_button_content_get_label(ADW_BUTTON_CONTENT(mute_content)), ==, "Mute");
    g_assert_cmpint(f.model.call, ==, MOCK_CALL_ACTIVE);
    g_assert_true(mock_call_popup_window(f.popup) == window);
    assert_identity_on_screen(window);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "active-actions")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "incoming-actions")));
    click(&f, "mute");
    g_assert_true(f.model.muted);
    mute_content = gtk_button_get_child(GTK_BUTTON(find(GTK_WIDGET(window), "mute")));
    g_assert_cmpstr(adw_button_content_get_label(ADW_BUTTON_CONTENT(mute_content)), ==, "Unmute");
    click(&f, "compact");
    g_assert_true(f.model.compact);
    assert_identity_on_screen(window);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "keypad-toggle")));
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-selector")));
    click(&f, "expand");
    g_assert_false(f.model.compact);
    GtkEventController *keys = NULL;
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(window));
    for (guint i = 0; i < g_list_model_get_n_items(controllers) && !keys; i++) {
        GtkEventController *c = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(c)) keys = c;
        g_object_unref(c);
    }
    g_object_unref(controllers);
    g_assert_nonnull(keys);
    gboolean handled = FALSE;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_m, 0, 0, &handled);
    g_assert_true(handled);
    g_assert_false(f.model.muted);
    click(&f, "end");
    g_assert_cmpint(f.model.call, ==, MOCK_CALL_ENDING);
    mock_ui_tick(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "terminal-label"))), ==, "Call Ended");
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window)));
    mock_ui_tick(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window)));
    gtk_window_destroy(window);
}

static void ignore_and_decline(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_call_popup_update(f.popup, &f.model);
    click(&f, "ignore");
    g_assert_cmpint(f.model.call, ==, MOCK_CALL_INCOMING_IGNORED);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(f.popup))));
    mock_ui_finish_call(&f.model);
    mock_ui_incoming(&f.model, -1);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(strcmp(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(mock_call_popup_window(f.popup)),
                                                         "caller-identity"))), f.model.dialed_number) == 0);
    click(&f, "decline");
    g_assert_cmpint(f.model.call, ==, MOCK_CALL_DECLINED);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(mock_call_popup_window(f.popup)), "terminal-label")));
    mock_ui_tick(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(mock_call_popup_window(f.popup))));
    gtk_window_destroy(mock_call_popup_window(f.popup));
}

static void outgoing_controls(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_outgoing(&f.model, 1);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "outgoing-actions")));
    mock_ui_connect_call(&f.model);
    mock_ui_tick(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(mock_call_popup_window(f.popup) == window);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "call-timer"))), ==, "00:01");
    click(&f, "audio-option-2");
    g_assert_cmpuint(f.model.audio_index, ==, 2);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-check-2")));
    f.model.empty_audio_devices = true;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-empty")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-choices")));
    f.model.empty_audio_devices = false;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-empty")));
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-choices")));
    click(&f, "keypad-toggle");
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "in-call-keypad")));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(GTK_WIDGET(window), "tone-2"))), ==, "2");
    click(&f, "tone-1");
    g_assert_cmpstr(f.last_action, ==, "tone");
    g_assert_cmpstr(f.last_argument, ==, "1");
    gtk_window_destroy(window);
}

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv);
    app = gtk_application_new("org.iblueconnect.MockPopupTest", G_APPLICATION_NON_UNIQUE);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    g_test_add_data_func("/popup/incoming-to-active", NULL, incoming_flow);
    g_test_add_data_func("/popup/ignore-decline", NULL, ignore_and_decline);
    g_test_add_data_func("/popup/outgoing-controls", NULL, outgoing_controls);
    int result = g_test_run();
    g_object_unref(app);
    return result;
}
