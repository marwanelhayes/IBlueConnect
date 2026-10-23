#include "call-popup.h"
#include <string.h>
#include <stdlib.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

typedef struct {
    MockUiModel model;
    MockCallPopup *popup;
    const char *last_action;
    char last_argument[128]; /* Copied: row buttons and their arguments are rebuilt on update. */
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
    g_strlcpy(f->last_argument, argument ? argument : "", sizeof f->last_argument);
    if (strcmp(name, "ignore") == 0) mock_ui_ignore(&f->model);
    else if (strcmp(name, "decline") == 0) mock_ui_decline(&f->model);
    else if (strcmp(name, "answer") == 0) mock_ui_answer(&f->model);
    else if (strcmp(name, "mute") == 0) mock_ui_toggle_mute(&f->model);
    else if (strcmp(name, "end") == 0) mock_ui_end_call(&f->model);
    else if (strcmp(name, "compact") == 0) mock_ui_set_compact(&f->model, argument[0] == '1');
    else if (strcmp(name, "audio") == 0) mock_ui_select_audio(&f->model, (size_t)atoi(argument));
    else if (strcmp(name, "ignore-waiting") == 0) mock_ui_ignore_waiting(&f->model);
    else mock_ui_calls_action(&f->model, name, argument);
    mock_call_popup_update(f->popup, &f->model);
}

static void click(Fixture *f, const char *name)
{
    GtkWidget *button = find(GTK_WIDGET(mock_call_popup_window(f->popup)), name);
    g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
}

/* Mute is a tile: the microphone icon over a caption, like the mockup. */
static const char *mute_icon(GtkWindow *window)
{
    GtkWidget *content = gtk_button_get_child(GTK_BUTTON(find(GTK_WIDGET(window), "mute")));
    return gtk_image_get_icon_name(GTK_IMAGE(g_object_get_data(G_OBJECT(content), "tile-image")));
}

static const char *mute_label(GtkWindow *window)
{
    GtkWidget *content = gtk_button_get_child(GTK_BUTTON(find(GTK_WIDGET(window), "mute")));
    return gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(content), "tile-caption")));
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

static void movable_popup(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    GtkWidget *identity = find(GTK_WIDGET(window), "caller-identity");
    while (identity && !GTK_IS_WINDOW_HANDLE(identity))
        identity = gtk_widget_get_parent(identity);
    g_assert_nonnull(identity); /* The caller row can be grabbed to move the popup. */
    GtkWidget *actions = find(GTK_WIDGET(window), "incoming-actions");
    while (actions && !GTK_IS_WINDOW_HANDLE(actions))
        actions = gtk_widget_get_parent(actions);
    g_assert_nonnull(actions); /* So can the space around the buttons. */
#ifdef GDK_WINDOWING_X11
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (GDK_IS_X11_SURFACE(surface)) {
        GdkDisplay *gdk = gdk_surface_get_display(surface);
        Display *display = gdk_x11_display_get_xdisplay(gdk);
        Window xid = gdk_x11_surface_get_xid(surface);
        gint64 deadline = g_get_monotonic_time() + 120 * G_TIME_SPAN_MILLISECOND;
        while (g_get_monotonic_time() < deadline) {
            while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
            g_usleep(1000);
        }
        XMoveWindow(display, xid, 80, 100);
        XSync(display, False);
        mock_call_popup_update(f.popup, &f.model);
        deadline = g_get_monotonic_time() + 120 * G_TIME_SPAN_MILLISECOND;
        while (g_get_monotonic_time() < deadline) {
            while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
            g_usleep(1000);
        }
        Window child;
        int x, y;
        g_assert_true(XTranslateCoordinates(display, xid, DefaultRootWindow(display),
                                             0, 0, &x, &y, &child));
        g_assert_cmpint(x, ==, 80);
        g_assert_cmpint(y, ==, 100);
        /* GNOME Shell must not count the popup as the app's window, or the app icon would only
         * focus the popup instead of opening the main window. */
        Atom app_id = XInternAtom(display, "_GTK_APPLICATION_ID", False), type;
        int format;
        unsigned long items, remaining;
        unsigned char *value = NULL;
        XGetWindowProperty(display, xid, app_id, 0, 64, False, AnyPropertyType, &type, &format,
                           &items, &remaining, &value);
        g_assert_cmpuint(type, ==, None);
        if (value) XFree(value);
        XClassHint hint;
        g_assert_true(XGetClassHint(display, xid, &hint));
        g_assert_cmpstr(hint.res_class, ==, "IBlueConnect-Call");
        XFree(hint.res_name);
        XFree(hint.res_class);
    }
#endif
    gtk_window_destroy(window);
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
    click(&f, "open-app");
    g_assert_cmpstr(f.last_action, ==, "open-app");
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window)));
    click(&f, "answer");
    g_assert_cmpstr(mute_icon(window), ==, "audio-input-microphone-symbolic");
    g_assert_cmpstr(mute_label(window), ==, "Mute");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "mute")), ==, "Mute");
    g_assert_cmpint(f.model.call, ==, MOCK_CALL_ACTIVE);
    g_assert_true(mock_call_popup_window(f.popup) == window);
    assert_identity_on_screen(window);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "active-actions")));
    click(&f, "open-app");
    g_assert_cmpstr(f.last_action, ==, "open-app");
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window)));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "incoming-actions")));
    click(&f, "mute");
    g_assert_true(f.model.muted);
    g_assert_cmpstr(mute_icon(window), ==, "microphone-sensitivity-muted-symbolic");
    g_assert_cmpstr(mute_label(window), ==, "Muted");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "mute")), ==, "Unmute");
    click(&f, "compact");
    g_assert_true(f.model.compact);
    assert_identity_on_screen(window);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "keypad-toggle")));
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-selector")));
    g_assert_cmpstr(mute_icon(window), ==, "microphone-sensitivity-muted-symbolic");
    g_assert_cmpstr(mute_label(window), ==, ""); /* Compact keeps icons only. */
    click(&f, "mute");
    g_assert_false(f.model.muted);
    g_assert_cmpstr(mute_icon(window), ==, "audio-input-microphone-symbolic");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "mute")), ==, "Mute");
    click(&f, "mute");
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

static void live_mode_hides_mock_controls(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.model.live = true;
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "active-actions")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "mute")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-selector")));
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "keypad-toggle")));
    f.model.call_audio = true; /* The daemon found this call's audio on the laptop. */
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "mute")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "audio-selector")));
    click(&f, "mute");
    g_assert_cmpstr(f.last_action, ==, "mute");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "mute")), ==, "Unmute");
    gtk_window_destroy(window);
}

static void live_call_shows_actual_number(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.model.live = true;
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    g_strlcpy(f.model.dialed_number, "555-0102", sizeof f.model.dialed_number);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "caller-number"))),
                    ==, "555-0102");
    f.model.dialed_number[0] = '\0';
    mock_call_popup_update(f.popup, &f.model);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "caller-number"))),
                    ==, "");
    gtk_window_destroy(window);
}

static void hold_and_resume_only_when_available(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.model.live = true;
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "hold")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "resume")));
    f.model.can_hold = true;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "hold")));
    click(&f, "hold");
    g_assert_cmpstr(f.last_action, ==, "hold");
    f.model.can_hold = false;
    f.model.can_resume = true;
    f.model.call_held = true;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "hold")));
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "resume")));
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "keypad-toggle")));
    click(&f, "resume");
    g_assert_cmpstr(f.last_action, ==, "resume");
    gtk_window_destroy(window);
}

static void merge_only_for_eligible_active_call(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.model.live = true;
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    f.model.can_merge = true;
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_false(gtk_widget_get_visible(find(GTK_WIDGET(window), "merge")));
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(gtk_widget_get_visible(find(GTK_WIDGET(window), "merge")));
    click(&f, "merge");
    g_assert_cmpstr(f.last_action, ==, "merge");
    gtk_window_destroy(window);
}

static void conference_end_targets_all_calls(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.model.live = true;
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    f.model.call_count = 2;
    f.model.can_end_conference = true;
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "call-subtitle"))),
                    ==, "2 calls · via iPhone");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "end")),
                    ==, "End conference");
    click(&f, "end");
    g_assert_cmpstr(f.last_action, ==, "end-conference");
    f.model.can_end_conference = false;
    mock_call_popup_update(f.popup, &f.model);
    click(&f, "end");
    g_assert_cmpstr(f.last_action, ==, "end");
    gtk_window_destroy(window);
}

static gboolean shown(Fixture *f, const char *name)
{
    GtkWidget *widget = find(GTK_WIDGET(mock_call_popup_window(f->popup)), name);
    return widget && gtk_widget_get_visible(widget);
}

static const char *text_of(Fixture *f, const char *name)
{
    GtkWidget *widget = find(GTK_WIDGET(mock_call_popup_window(f->popup)), name);
    g_assert_nonnull(widget);
    return gtk_label_get_text(GTK_LABEL(widget));
}

static int min_width(Fixture *f, const char *name)
{
    int minimum, natural;
    GtkWidget *window = GTK_WIDGET(mock_call_popup_window(f->popup));
    gtk_widget_measure(name ? find(window, name) : window, GTK_ORIENTATION_HORIZONTAL, -1,
                       &minimum, &natural, NULL, NULL);
    return minimum;
}

static void waiting_banner(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    GtkWindow *window = mock_call_popup_window(f.popup);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(shown(&f, "waiting-banner"));
    int single = min_width(&f, NULL);
    mock_ui_simulate_calls(&f.model, "waiting");
    mock_call_popup_update(f.popup, &f.model);
    char waiting[128];
    g_strlcpy(waiting, f.model.waiting_call_id, sizeof waiting);
    g_assert_true(shown(&f, "waiting-banner"));
    g_assert_cmpstr(text_of(&f, "waiting-caller"), ==, mock_contacts[1].name);
    g_assert_cmpstr(text_of(&f, "waiting-status"), ==, "Waiting");
    g_assert_true(gtk_widget_has_css_class(find(GTK_WIDGET(window), "waiting-decline"), "call-decline"));
    g_assert_true(gtk_widget_has_css_class(find(GTK_WIDGET(window), "hold-and-answer"), "call-answer"));
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "hold-and-answer")), ==, "Hold & Accept");
    g_assert_true(shown(&f, "hold-and-answer") && shown(&f, "release-and-answer"));
    g_assert_cmpint(min_width(&f, "waiting-banner"), <, single); /* The banner keeps the controller narrow. */
    click(&f, "waiting-ignore");
    g_assert_cmpstr(f.last_action, ==, "ignore-waiting");
    g_assert_false(shown(&f, "waiting-banner"));
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(window))); /* The phone keeps ringing. */
    mock_ui_simulate_calls(&f.model, "waiting");
    mock_call_popup_update(f.popup, &f.model);
    click(&f, "waiting-decline");
    g_assert_cmpstr(f.last_action, ==, "end-line");
    g_assert_cmpstr(f.last_argument, ==, waiting);
    g_assert_false(shown(&f, "waiting-banner"));
    mock_ui_simulate_calls(&f.model, "two-calls");
    mock_ui_add_line(&f.model, "/w", "", "0100", MOCK_LINE_WAITING, false);
    mock_ui_derive_calls(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(shown(&f, "waiting-banner"));
    g_assert_cmpstr(text_of(&f, "waiting-caller"), ==, "0100");
    g_assert_false(shown(&f, "hold-and-answer")); /* A held call already exists. */
    click(&f, "release-and-answer");
    g_assert_cmpstr(f.last_action, ==, "release-and-answer");
    g_assert_cmpstr(f.last_argument, ==, "/w");
    gtk_window_destroy(window);
}

static void call_list_and_multi_call_actions(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    GtkWindow *window = mock_call_popup_window(f.popup);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(shown(&f, "call-list")); /* One call keeps the single-call controller. */
    int single = min_width(&f, NULL);
    g_assert_false(shown(&f, "swap") || shown(&f, "add-call"));
    f.model.can_add_call = true;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(shown(&f, "add-call"));
    g_assert_false(shown(&f, "call-list"));
    click(&f, "add-call");
    g_assert_cmpstr(f.last_action, ==, "add-call");
    mock_ui_simulate_calls(&f.model, "two-calls");
    mock_call_popup_update(f.popup, &f.model);
    g_assert_true(shown(&f, "call-list"));
    g_assert_false(shown(&f, "conference-header"));
    g_assert_cmpstr(text_of(&f, "call-row-status-0"), ==, "Active");
    g_assert_cmpstr(text_of(&f, "call-row-status-1"), ==, "On hold");
    g_assert_cmpstr(text_of(&f, "call-row-caller-1"), ==, mock_contacts[1].name);
    g_assert_true(shown(&f, "swap") && shown(&f, "merge"));
    g_assert_false(shown(&f, "resume") || shown(&f, "add-call") || shown(&f, "call-row-private-0"));
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "call-row-end-1")), ==, "End call");
    char held[128];
    g_strlcpy(held, f.model.lines[1].id, sizeof held);
    click(&f, "call-row-end-1");
    g_assert_cmpstr(f.last_action, ==, "end-line");
    g_assert_cmpstr(f.last_argument, ==, held);
    g_assert_false(shown(&f, "call-list"));
    mock_ui_simulate_calls(&f.model, "two-calls");
    mock_call_popup_update(f.popup, &f.model);
    click(&f, "swap");
    g_assert_cmpstr(f.last_action, ==, "swap");
    g_assert_cmpstr(text_of(&f, "call-row-status-0"), ==, "On hold");
    mock_ui_simulate_calls(&f.model, "conference");
    mock_call_popup_update(f.popup, &f.model);
    g_assert_cmpstr(text_of(&f, "conference-header"), ==, "Conference (3)");
    g_assert_true(shown(&f, "call-row-private-2") && shown(&f, "add-call"));
    g_assert_cmpint(min_width(&f, "call-list"), <, single);
    g_assert_false(shown(&f, "swap") || shown(&f, "merge"));
    g_assert_cmpstr(gtk_widget_get_tooltip_text(find(GTK_WIDGET(window), "end")), ==, "End conference");
    char member[128];
    g_strlcpy(member, f.model.lines[2].id, sizeof member);
    click(&f, "call-row-private-2");
    g_assert_cmpstr(f.last_action, ==, "private");
    g_assert_cmpstr(f.last_argument, ==, member);
    g_assert_cmpstr(text_of(&f, "conference-header"), ==, "Conference (2)");
    g_assert_false(shown(&f, "call-row-private-0")); /* Private needs the conference alone. */
    click(&f, "compact");
    g_assert_false(shown(&f, "call-list"));
    gtk_window_destroy(window);
}

static GBytes *wave_pixels(GtkWidget *wave)
{
    gint64 deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) {
        while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
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

/* Counts accent-orange (#e95420) and green (#2ec27e) pixels per half of the strip. */
typedef struct { guint orange_left, orange_right, green_left, green_right; } WaveColors;

static WaveColors wave_colors(GBytes *bytes, GtkWidget *wave)
{
    WaveColors c = {0};
    int width = gtk_widget_get_width(wave), height = gtk_widget_get_height(wave);
    int stride = (int)(g_bytes_get_size(bytes) / height);
    const guchar *p = g_bytes_get_data(bytes, NULL);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const guchar *px = p + y * stride + x * 4; /* B, G, R, A */
            gboolean left = x < width / 2;
            if (px[2] > px[1] * 1.5 && px[1] > px[0] * 1.5) { if (left) c.orange_left++; else c.orange_right++; }
            if (px[1] > px[2] * 2 && px[1] > px[0] * 1.3) { if (left) c.green_left++; else c.green_right++; }
        }
    return c;
}

/* Apple-inspired strip: the caller's voice is green, enters from the right, and rings the avatar green. */
static void incoming_voice_is_green_from_the_right(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    gtk_window_present(window);
    GtkWidget *wave = find(GTK_WIDGET(window), "voice-waveform");
    GtkWidget *avatar = find(GTK_WIDGET(window), "caller-ring");
    g_assert_null(find(GTK_WIDGET(window), "incoming-level"));
    GBytes *silent = wave_pixels(wave);
    WaveColors quiet = wave_colors(silent, wave);
    g_assert_cmpuint(quiet.green_left + quiet.green_right + quiet.orange_left + quiet.orange_right, ==, 0);
    g_assert_false(gtk_widget_has_css_class(avatar, "speaking-them"));
    mock_call_popup_set_voice(f.popup, 1.0);
    GBytes *talking = wave_pixels(wave);
    WaveColors them = wave_colors(talking, wave);
    g_assert_cmpuint(them.green_right, >, 20);
    g_assert_cmpuint(them.green_right, >, them.green_left * 3);
    g_assert_cmpuint(them.orange_left + them.orange_right, ==, 0);
    g_assert_true(gtk_widget_has_css_class(avatar, "speaking-them"));
    g_assert_false(gtk_widget_has_css_class(avatar, "speaking-me"));
    GBytes *ring = wave_pixels(avatar);
    WaveColors ring_colors = wave_colors(ring, avatar);
    g_assert_cmpuint(ring_colors.green_left + ring_colors.green_right, >, 20); /* The ring is actually drawn. */
    g_bytes_unref(ring);
    GBytes *still = wave_pixels(wave);
    g_assert_true(g_bytes_equal(talking, still)); /* No decorative idle animation. */
    GtkWidget *caller = find(GTK_WIDGET(window), "wave-caller");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(caller)), ==, gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(window), "caller-identity"))));
    for (int i = 0; i < 20; i++) mock_call_popup_set_voice(f.popup, 0.0);
    g_assert_false(gtk_widget_has_css_class(avatar, "speaking-them"));
    f.model.live = true; /* Live audio leaving the laptop stills the strip at once. */
    f.model.call_audio = true;
    mock_call_popup_set_voice(f.popup, 1.0);
    f.model.call_audio = false;
    mock_call_popup_update(f.popup, &f.model);
    g_assert_false(gtk_widget_has_css_class(avatar, "speaking-them"));
    g_bytes_unref(silent);
    g_bytes_unref(talking);
    g_bytes_unref(still);
    gtk_window_destroy(window);
}

/* Your voice is the Ubuntu accent, enters from the left, and stays flat while muted. */
static void outgoing_voice_is_accent_from_the_left(gconstpointer ignored)
{
    (void)ignored;
    GtkCssProvider *accent = gtk_css_provider_new();
    gtk_css_provider_load_from_string(accent, "@define-color accent_color #e95420;");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(accent),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    gtk_window_present(window);
    GtkWidget *wave = find(GTK_WIDGET(window), "voice-waveform");
    GtkWidget *avatar = find(GTK_WIDGET(window), "caller-ring");
    GtkWidget *you = find(GTK_WIDGET(window), "wave-you");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(you)), ==, "You");
    GBytes *silent = wave_pixels(wave);
    mock_call_popup_set_microphone_level(f.popup, 1.0);
    GBytes *orange = wave_pixels(wave);
    WaveColors me = wave_colors(orange, wave);
    g_assert_cmpuint(me.orange_left, >, 20);
    g_assert_cmpuint(me.orange_left, >, me.orange_right * 3);
    g_assert_cmpuint(me.green_left + me.green_right, ==, 0);
    g_assert_true(gtk_widget_has_css_class(avatar, "speaking-me"));
    GBytes *ring = wave_pixels(avatar);
    WaveColors ring_colors = wave_colors(ring, avatar);
    g_assert_cmpuint(ring_colors.orange_left + ring_colors.orange_right, >, 20);
    g_bytes_unref(ring);
    gtk_css_provider_load_from_string(accent, "@define-color accent_color #20bbed;");
    GBytes *blue = wave_pixels(wave);
    g_assert_false(g_bytes_equal(orange, blue)); /* Follows the system accent. */
    gtk_css_provider_load_from_string(accent, "@define-color accent_color #e95420;");
    mock_call_popup_set_voice(f.popup, 1.0);
    GBytes *both = wave_pixels(wave);
    WaveColors meet = wave_colors(both, wave);
    g_assert_cmpuint(meet.orange_left, >, 20);
    g_assert_cmpuint(meet.green_right, >, 20);
    mock_call_popup_set_microphone_level(f.popup, -1);
    for (int i = 0; i < 20; i++) mock_call_popup_set_voice(f.popup, 0.0);
    GBytes *unavailable = wave_pixels(wave);
    g_assert_true(g_bytes_equal(silent, unavailable));
    f.model.muted = true;
    mock_call_popup_update(f.popup, &f.model);
    mock_call_popup_set_microphone_level(f.popup, 1.0);
    GBytes *muted = wave_pixels(wave);
    g_assert_true(g_bytes_equal(silent, muted));
    g_assert_true(gtk_widget_has_css_class(you, "muted"));
    g_assert_false(gtk_widget_has_css_class(avatar, "speaking-me"));
    click(&f, "compact");
    g_assert_true(gtk_widget_is_visible(wave));
    g_assert_false(gtk_widget_is_visible(you));
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(accent));
    g_object_unref(accent);
    g_bytes_unref(silent);
    g_bytes_unref(orange);
    g_bytes_unref(blue);
    g_bytes_unref(both);
    g_bytes_unref(unavailable);
    g_bytes_unref(muted);
    gtk_window_destroy(window);
}

static void assert_button_color(GtkWidget *button, gboolean green)
{
    /* Check rendered pixels, so a theme overriding our background fails too. */
    GdkPaintable *paintable = gtk_widget_paintable_new(button);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    int width = gtk_widget_get_width(button), height = gtk_widget_get_height(button);
    g_assert_cmpint(width, >, 0);
    g_assert_cmpint(height, >, 0);
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_assert_nonnull(node);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    GskRenderer *renderer = gtk_native_get_renderer(gtk_widget_get_native(button));
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
    gsize stride = width * 4;
    guchar *pixels = g_malloc(stride * height);
    gdk_texture_download(texture, pixels, stride);
    guint colored = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            /* GDK's default download format is premultiplied BGRA on x86_64. */
            const guchar *pixel = pixels + y * stride + x * 4;
            double red = pixel[2], lime = pixel[1], blue = pixel[0];
            if (green ? lime > red * 1.3 && lime > blue * 1.3 :
                        red > lime * 1.3 && red > blue * 1.3)
                colored++;
        }
    }
    g_assert_cmpuint(colored, >, width * height / 2);
    g_free(pixels);
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
}

/* The controller matches the strip: labelled tiles, a bold timer, and a wide solid-red End pill. */
static void controller_matches_the_strip(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_ui_answer(&f.model);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    gtk_window_present(window);
    for (int i = 0; i < 40; i++) while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(window), "call-controller"));
    GtkWidget *keypad = gtk_button_get_child(GTK_BUTTON(find(GTK_WIDGET(window), "keypad-toggle")));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(keypad), "tile-caption"))), ==, "Keypad");
    int row = 0; /* Labelled tiles still fit the 360 px controller's 332 px content width (shadows excluded). */
    gtk_widget_measure(find(GTK_WIDGET(window), "active-actions"), GTK_ORIENTATION_HORIZONTAL, -1, NULL, &row, NULL, NULL);
    g_assert_cmpint(row, <=, 332);
    g_assert_true(gtk_widget_has_css_class(find(GTK_WIDGET(window), "call-timer"), "heading"));
    GtkWidget *end = find(GTK_WIDGET(window), "end");
    g_assert_true(gtk_widget_get_hexpand(end));
    assert_button_color(end, FALSE);
    gtk_window_destroy(window);
}

/* No empty band under Ignore/Decline/Answer: the window hugs its content. */
static void incoming_window_has_no_empty_space(gconstpointer ignored)
{
    (void)ignored;
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    gtk_window_present(window);
    for (int i = 0; i < 40; i++) while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    graphene_rect_t actions;
    g_assert_true(gtk_widget_compute_bounds(find(GTK_WIDGET(window), "incoming-actions"), GTK_WIDGET(window), &actions));
    g_assert_cmpfloat(gtk_widget_get_height(GTK_WIDGET(window)) - (actions.origin.y + actions.size.height), <=, 24);
    gtk_window_destroy(window);
}

static void incoming_colors(gconstpointer ignored)
{
    (void)ignored;
    AdwStyleManager *style = adw_style_manager_get_default();
    AdwColorScheme previous = adw_style_manager_get_color_scheme(style);
    Fixture f = {0};
    mock_ui_init(&f.model);
    f.popup = mock_call_popup_new(app, action, &f);
    mock_ui_incoming(&f.model, 0);
    mock_call_popup_update(f.popup, &f.model);
    GtkWindow *window = mock_call_popup_window(f.popup);
    GtkWidget *answer = find(GTK_WIDGET(window), "answer");
    GtkWidget *decline = find(GTK_WIDGET(window), "decline");
    const GtkStateFlags states[] = { GTK_STATE_FLAG_NORMAL, GTK_STATE_FLAG_PRELIGHT,
                                   GTK_STATE_FLAG_ACTIVE, GTK_STATE_FLAG_INSENSITIVE };
    for (int dark = 0; dark < 2; dark++) {
        adw_style_manager_set_color_scheme(style, dark ? ADW_COLOR_SCHEME_FORCE_DARK :
                                                       ADW_COLOR_SCHEME_FORCE_LIGHT);
        for (guint i = 0; i < G_N_ELEMENTS(states); i++) {
            gtk_widget_set_state_flags(answer, states[i], TRUE);
            gtk_widget_set_state_flags(decline, states[i], TRUE);
            gint64 deadline = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
            while (g_get_monotonic_time() < deadline) {
                while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
                g_usleep(1000);
            }
            assert_button_color(answer, TRUE);
            assert_button_color(decline, FALSE);
        }
    }
    gtk_window_destroy(window);
    adw_style_manager_set_color_scheme(style, previous);
}

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv);
    app = gtk_application_new("org.iblueconnect.MockPopupTest", G_APPLICATION_NON_UNIQUE);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    g_test_add_data_func("/popup/incoming-to-active", NULL, incoming_flow);
    g_test_add_data_func("/popup/ignore-decline", NULL, ignore_and_decline);
    g_test_add_data_func("/popup/outgoing-controls", NULL, outgoing_controls);
    g_test_add_data_func("/popup/live-mode", NULL, live_mode_hides_mock_controls);
    g_test_add_data_func("/popup/live-number", NULL, live_call_shows_actual_number);
    g_test_add_data_func("/popup/hold-resume", NULL, hold_and_resume_only_when_available);
    g_test_add_data_func("/popup/merge", NULL, merge_only_for_eligible_active_call);
    g_test_add_data_func("/popup/conference-end", NULL, conference_end_targets_all_calls);
    g_test_add_data_func("/popup/movable", NULL, movable_popup);
    g_test_add_data_func("/popup/voice", NULL, incoming_voice_is_green_from_the_right);
    g_test_add_data_func("/popup/outgoing-wave", NULL, outgoing_voice_is_accent_from_the_left);
    g_test_add_data_func("/popup/incoming-colors", NULL, incoming_colors);
    g_test_add_data_func("/popup/incoming-compact-height", NULL, incoming_window_has_no_empty_space);
    g_test_add_data_func("/popup/controller-style", NULL, controller_matches_the_strip);
    g_test_add_data_func("/popup/waiting-banner", NULL, waiting_banner);
    g_test_add_data_func("/popup/call-list", NULL, call_list_and_multi_call_actions);
    int result = g_test_run();
    g_object_unref(app);
    return result;
}
