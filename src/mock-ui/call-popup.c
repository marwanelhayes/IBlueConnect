#include "call-popup.h"
#include <stdlib.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

struct MockCallPopup {
    GtkWidget *window, *avatar, *mute_content, *audio_content, *end_content, *identity, *number, *subtitle, *timer, *waveform;
    GtkWidget *incoming, *outgoing, *active, *terminal, *details;
    GtkWidget *compact_button, *expand_button, *mute_button, *keypad_button, *keypad, *tone_feedback;
    GtkWidget *audio_button, *audio_popover, *audio_choices, *audio_empty;
    GtkWidget **audio_checks;
    MockCallPopupAction action;
    gpointer user_data;
    MockCall phase;
    gboolean keypad_open;
    guint wave_tick;
#ifdef GDK_WINDOWING_X11
    guint placement_tick;
#endif
};

static GtkWidget *label(const char *text, const char *css)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (css) gtk_widget_add_css_class(widget, css);
    return widget;
}

static GtkWidget *button(const char *text, const char *icon, const char *name)
{
    GtkWidget *widget = icon ? gtk_button_new_from_icon_name(icon) : gtk_button_new_with_label(text);
    gtk_widget_set_name(widget, name);
    gtk_widget_set_tooltip_text(widget, text);
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
    return widget;
}

static GtkWidget *labeled(GtkWidget *button, const char *icon, const char *text)
{
    GtkWidget *content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(content), text);
    if (GTK_IS_MENU_BUTTON(button)) gtk_menu_button_set_child(GTK_MENU_BUTTON(button), content);
    else gtk_button_set_child(GTK_BUTTON(button), content);
    return content;
}

static void dispatch(MockCallPopup *popup, const char *name, const char *argument)
{
    if (popup->action) popup->action(name, argument, popup->user_data);
}

static void clicked(GtkButton *button, gpointer data)
{
    MockCallPopup *popup = data;
    const char *name = g_object_get_data(G_OBJECT(button), "mock-action");
    const char *argument = g_object_get_data(G_OBJECT(button), "mock-argument");
    dispatch(popup, name, argument ? argument : "");
}

static GtkWidget *action_button(MockCallPopup *popup, const char *text, const char *icon,
                                const char *name, const char *argument)
{
    GtkWidget *widget = button(text, icon, name);
    g_object_set_data(G_OBJECT(widget), "mock-action", (gpointer)name);
    g_object_set_data(G_OBJECT(widget), "mock-argument", (gpointer)argument);
    g_signal_connect(widget, "clicked", G_CALLBACK(clicked), popup);
    return widget;
}

static void toggle_keypad(GtkButton *button, gpointer data)
{
    (void)button;
    MockCallPopup *popup = data;
    popup->keypad_open = !popup->keypad_open;
    gtk_widget_set_visible(popup->keypad, popup->keypad_open);
    dispatch(popup, "keypad", popup->keypad_open ? "1" : "0");
}

static void tone_clicked(GtkButton *button, gpointer data)
{
    MockCallPopup *popup = data;
    const char *key = g_object_get_data(G_OBJECT(button), "mock-key");
    gtk_label_set_text(GTK_LABEL(popup->tone_feedback), key);
    dispatch(popup, "tone", key);
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                       GdkModifierType modifiers, gpointer data)
{
    (void)controller;
    (void)keycode;
    MockCallPopup *popup = data;
    if (popup->phase != MOCK_CALL_ACTIVE || modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;
    if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) dispatch(popup, "mute", "");
    else if (keyval == GDK_KEY_k || keyval == GDK_KEY_K) toggle_keypad(NULL, popup);
    else if (keyval == GDK_KEY_a || keyval == GDK_KEY_A)
        gtk_menu_button_popup(GTK_MENU_BUTTON(popup->audio_button));
    else if (keyval == GDK_KEY_Escape && popup->keypad_open) toggle_keypad(NULL, popup);
    else return FALSE;
    return TRUE;
}

static GtkWidget *make_keypad(MockCallPopup *popup)
{
    static const char *keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        GtkWidget *key = gtk_button_new_with_label(keys[i]);
        char name[24];
        g_snprintf(name, sizeof name, "tone-%c", keys[i][0]);
        gtk_widget_set_name(key, name);
        gtk_widget_set_hexpand(key, TRUE);
        g_object_set_data_full(G_OBJECT(key), "mock-key", g_strndup(keys[i], 1), g_free);
        g_signal_connect(key, "clicked", G_CALLBACK(tone_clicked), popup);
        gtk_grid_attach(GTK_GRID(grid), key, i % 3, i / 3, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    popup->tone_feedback = label("", "dim-label");
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->tone_feedback), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Last keypad key", -1);
    gtk_box_append(GTK_BOX(box), popup->tone_feedback);
    gtk_widget_set_name(box, "in-call-keypad");
    gtk_widget_set_visible(box, FALSE);
    return box;
}

static void audio_clicked(GtkButton *button, gpointer data)
{
    MockCallPopup *popup = data;
    const char *index = g_object_get_data(G_OBJECT(button), "mock-index");
    gtk_popover_popdown(GTK_POPOVER(popup->audio_popover));
    dispatch(popup, "audio", index);
}

static GtkWidget *make_audio(MockCallPopup *popup)
{
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "audio-speakers-symbolic");
    gtk_widget_set_name(menu, "audio-selector");
    gtk_widget_set_tooltip_text(menu, "Audio device");
    gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, "Audio device", -1);
    GtkWidget *popover = gtk_popover_new();
    popup->audio_popover = popover;
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(list, 8);
    gtk_widget_set_margin_bottom(list, 8);
    gtk_widget_set_margin_start(list, 8);
    gtk_widget_set_margin_end(list, 8);
    gtk_box_append(GTK_BOX(list), label("Audio", "heading"));
    GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(choices, "audio-choices");
    popup->audio_choices = choices;
    popup->audio_checks = g_new0(GtkWidget *, mock_audio_devices_count);
    for (size_t i = 0; i < mock_audio_devices_count; i++) {
        GtkWidget *row = gtk_button_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");
        char name[32];
        g_snprintf(name, sizeof name, "audio-check-%zu", i);
        gtk_widget_set_name(check, name);
        popup->audio_checks[i] = check;
        gtk_box_append(GTK_BOX(content), check);
        GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(text), label(mock_audio_devices[i].name, NULL));
        gtk_box_append(GTK_BOX(text), label(mock_audio_devices[i].description, "dim-label"));
        gtk_box_append(GTK_BOX(content), text);
        gtk_button_set_child(GTK_BUTTON(row), content);
        g_snprintf(name, sizeof name, "audio-option-%zu", i);
        gtk_widget_set_name(row, name);
        g_object_set_data_full(G_OBJECT(row), "mock-index", g_strdup_printf("%zu", i), g_free);
        g_signal_connect(row, "clicked", G_CALLBACK(audio_clicked), popup);
        gtk_box_append(GTK_BOX(choices), row);
    }
    gtk_box_append(GTK_BOX(list), choices);
    popup->audio_empty = label("No audio devices", "dim-label");
    gtk_widget_set_name(popup->audio_empty, "audio-empty");
    gtk_box_append(GTK_BOX(list), popup->audio_empty);
    gtk_popover_set_child(GTK_POPOVER(popover), list);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
    return menu;
}

static gboolean on_close(GtkWindow *window, gpointer data)
{
    MockCallPopup *popup = data;
    dispatch(popup, popup->phase == MOCK_CALL_INCOMING ? "ignore" : "dismiss", "");
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void on_destroy(GtkWidget *window, gpointer data)
{
    (void)window;
    MockCallPopup *popup = data;
    if (popup->wave_tick) g_source_remove(popup->wave_tick);
#ifdef GDK_WINDOWING_X11
    if (popup->placement_tick) g_source_remove(popup->placement_tick);
#endif
    g_free(popup->audio_checks);
    g_free(popup);
}

#ifdef GDK_WINDOWING_X11
static void on_realize(GtkWidget *window, gpointer data)
{
    (void)data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (GDK_IS_X11_SURFACE(surface)) gdk_x11_surface_set_user_time(surface, 0);
}

static void place_x11(GtkWidget *window, gpointer data)
{
    (void)data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    GdkDisplay *gdk = gdk_surface_get_display(surface);
    GdkMonitor *monitor = gdk_display_get_monitor_at_surface(gdk, surface);
    if (!monitor) return;
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    Display *display = gdk_x11_display_get_xdisplay(gdk);
    Window xid = gdk_x11_surface_get_xid(surface);
    int width = gdk_surface_get_width(surface);
    if (width <= 0) width = 360;
    int x = geometry.x + geometry.width - width - 24, y = geometry.y + 24;
    int current_x, current_y;
    Window child;
    if (!XTranslateCoordinates(display, xid, DefaultRootWindow(display), 0, 0,
                               &current_x, &current_y, &child) || current_x != x || current_y != y) {
        XMoveWindow(display, xid, x, y);
        gtk_widget_queue_draw(window);
    }
    Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.window = xid;
    event.xclient.message_type = state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = above;
    event.xclient.data.l[3] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

static gboolean place_after_resize(gpointer data)
{
    MockCallPopup *popup = data;
    popup->placement_tick = 0;
    place_x11(popup->window, NULL);
    return G_SOURCE_REMOVE;
}
#endif

static gboolean animate_wave(gpointer data)
{
    MockCallPopup *popup = data;
    static const char *frames[] = {"▁▂▃▄▅▄▃▂▁", "▂▃▄▆▅▃▂▁▂", "▁▃▅▆▄▂▁▂▃", "▂▄▅▄▃▁▂▃▄"};
    static guint frame;
    gtk_label_set_text(GTK_LABEL(popup->waveform), frames[frame++ % G_N_ELEMENTS(frames)]);
    return G_SOURCE_CONTINUE;
}

MockCallPopup *mock_call_popup_new(GtkApplication *app, MockCallPopupAction action, gpointer user_data)
{
    MockCallPopup *popup = g_new0(MockCallPopup, 1);
    popup->action = action;
    popup->user_data = user_data;
    popup->phase = MOCK_CALL_IDLE;
    GtkWidget *window = adw_application_window_new(app);
    popup->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "IBlueConnect Call");
    gtk_window_set_default_size(GTK_WINDOW(window), 360, -1);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close), popup);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), popup);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), popup);
    gtk_widget_add_controller(window, keys);
#ifdef GDK_WINDOWING_X11
    g_signal_connect(window, "realize", G_CALLBACK(on_realize), popup);
    g_signal_connect(window, "map", G_CALLBACK(place_x11), popup);
#endif
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(body, 14);
    gtk_widget_set_margin_bottom(body, 14);
    gtk_widget_set_margin_start(body, 14);
    gtk_widget_set_margin_end(body, 14);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), body);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    popup->avatar = adw_avatar_new(40, NULL, TRUE);
    gtk_widget_set_name(popup->avatar, "caller-avatar");
    gtk_box_append(GTK_BOX(top), popup->avatar);
    GtkWidget *identity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(identity_box, TRUE);
    popup->identity = label("", "heading");
    gtk_widget_set_name(popup->identity, "caller-identity");
    gtk_box_append(GTK_BOX(identity_box), popup->identity);
    popup->subtitle = label("via iPhone", "dim-label");
    gtk_box_append(GTK_BOX(identity_box), popup->subtitle);
    gtk_box_append(GTK_BOX(top), identity_box);
    popup->timer = label("", "dim-label");
    gtk_widget_set_name(popup->timer, "call-timer");
    gtk_box_append(GTK_BOX(top), popup->timer);
    gtk_box_append(GTK_BOX(body), top);

    popup->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    popup->number = label("", "dim-label");
    gtk_box_append(GTK_BOX(body), popup->number);
    popup->waveform = label("▁▂▃▄▅▄▃▂▁", "dim-label");
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->waveform), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Audio activity", -1);
    gtk_box_append(GTK_BOX(popup->details), popup->waveform);
    popup->keypad = make_keypad(popup);
    gtk_box_append(GTK_BOX(popup->details), popup->keypad);
    gtk_box_append(GTK_BOX(body), popup->details);

    popup->incoming = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(popup->incoming, "incoming-actions");
    gtk_box_append(GTK_BOX(popup->incoming), action_button(popup, "Ignore", NULL, "ignore", ""));
    gtk_box_append(GTK_BOX(popup->incoming), action_button(popup, "Decline", NULL, "decline", ""));
    GtkWidget *answer = action_button(popup, "Answer", NULL, "answer", "");
    gtk_widget_add_css_class(answer, "suggested-action");
    gtk_box_append(GTK_BOX(popup->incoming), answer);
    gtk_box_append(GTK_BOX(body), popup->incoming);

    popup->outgoing = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(popup->outgoing, "outgoing-actions");
    GtkWidget *end_outgoing = action_button(popup, "End call", "call-stop-symbolic", "end-outgoing", "");
    g_object_set_data(G_OBJECT(end_outgoing), "mock-action", "end");
    gtk_widget_add_css_class(end_outgoing, "destructive-action");
    gtk_box_append(GTK_BOX(popup->outgoing), end_outgoing);
    gtk_box_append(GTK_BOX(body), popup->outgoing);

    popup->active = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(popup->active, "active-actions");
    popup->mute_button = action_button(popup, "Mute", "microphone-sensitivity-high-symbolic", "mute", "");
    popup->mute_content = labeled(popup->mute_button, "microphone-sensitivity-high-symbolic", "Mute");
    gtk_box_append(GTK_BOX(popup->active), popup->mute_button);
    popup->audio_button = make_audio(popup);
    popup->audio_content = labeled(popup->audio_button, "audio-speakers-symbolic", "Audio");
    gtk_box_append(GTK_BOX(popup->active), popup->audio_button);
    GtkWidget *keypad_button = button("Keypad", "input-dialpad-symbolic", "keypad-toggle");
    popup->keypad_button = keypad_button;
    g_signal_connect(keypad_button, "clicked", G_CALLBACK(toggle_keypad), popup);
    gtk_box_append(GTK_BOX(popup->active), keypad_button);
    popup->compact_button = action_button(popup, "Compact", "view-restore-symbolic", "compact", "1");
    gtk_box_append(GTK_BOX(popup->active), popup->compact_button);
    popup->expand_button = action_button(popup, "Expand", "view-fullscreen-symbolic", "expand", "0");
    g_object_set_data(G_OBJECT(popup->expand_button), "mock-action", "compact");
    gtk_box_append(GTK_BOX(popup->active), popup->expand_button);
    GtkWidget *end = action_button(popup, "End call", "call-stop-symbolic", "end", "");
    gtk_widget_add_css_class(end, "destructive-action");
    popup->end_content = labeled(end, "call-stop-symbolic", "End");
    gtk_box_append(GTK_BOX(popup->active), end);
    gtk_box_append(GTK_BOX(body), popup->active);

    popup->terminal = label("", "heading");
    gtk_widget_set_name(popup->terminal, "terminal-label");
    gtk_box_append(GTK_BOX(body), popup->terminal);
    gtk_widget_set_visible(window, FALSE);
    return popup;
}

void mock_call_popup_update(MockCallPopup *popup, const MockUiModel *model)
{
    popup->phase = model->call;
    gboolean incoming = model->call == MOCK_CALL_INCOMING;
    gboolean outgoing = model->call == MOCK_CALL_OUTGOING || model->call == MOCK_CALL_CONNECTING;
    gboolean active = model->call == MOCK_CALL_ACTIVE;
    gboolean terminal = model->call == MOCK_CALL_DECLINED || model->call == MOCK_CALL_ENDING ||
                        model->call == MOCK_CALL_ENDED;
    if (!active) {
        popup->keypad_open = FALSE;
        gtk_label_set_text(GTK_LABEL(popup->tone_feedback), "");
    }
    gtk_widget_set_visible(popup->incoming, incoming);
    gtk_widget_set_visible(popup->outgoing, outgoing);
    gtk_widget_set_visible(popup->active, active);
    gtk_widget_set_visible(popup->terminal, terminal);
    gtk_widget_set_visible(popup->details, active && !model->compact);
    gtk_widget_set_visible(popup->compact_button, active && !model->compact);
    gtk_widget_set_visible(popup->expand_button, active && model->compact);
    gtk_widget_set_visible(popup->keypad_button, active && !model->compact);
    gtk_widget_set_visible(popup->number, (incoming || outgoing || active) && !model->compact);
    gtk_widget_set_visible(popup->waveform, active && !model->compact);
    gtk_widget_set_visible(popup->keypad, active && !model->compact && popup->keypad_open);
    gtk_widget_set_visible(popup->timer, active);
    if (active) {
        char duration[32];
        g_snprintf(duration, sizeof duration, "%02u:%02u", model->elapsed_seconds / 60, model->elapsed_seconds % 60);
        gtk_label_set_text(GTK_LABEL(popup->timer), duration);
    }
    const MockContact *contact = model->caller_index >= 0 &&
                                 (size_t)model->caller_index < mock_contacts_count
                                 ? &mock_contacts[model->caller_index] : NULL;
    gtk_label_set_text(GTK_LABEL(popup->identity), contact ? contact->name :
                       model->dialed_number[0] ? model->dialed_number : "Unknown caller");
    gtk_label_set_text(GTK_LABEL(popup->number), contact ? contact->mobile :
                       incoming ? "Unknown caller" : "");
    gtk_label_set_text(GTK_LABEL(popup->subtitle), incoming ? "Incoming call · via iPhone" :
                       outgoing ? "Calling… via iPhone" : "via iPhone");
    gtk_label_set_text(GTK_LABEL(popup->terminal), model->call == MOCK_CALL_DECLINED ? "Call Declined" :
                       model->call == MOCK_CALL_ENDING ? "Ending call…" : "Call Ended");
    gtk_widget_set_tooltip_text(popup->mute_button, model->muted ? "Unmute" : "Mute");
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(popup->mute_content),
                                     model->muted ? "microphone-sensitivity-muted-symbolic" :
                                     "microphone-sensitivity-high-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(popup->mute_content),
                                 model->compact ? "" : model->muted ? "Unmute" : "Mute");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(popup->audio_content), model->compact ? "" : "Audio");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(popup->end_content), model->compact ? "" : "End");
    adw_avatar_set_text(ADW_AVATAR(popup->avatar), contact ? contact->name : NULL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->mute_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   model->muted ? "Unmute" : "Mute", -1);
    for (size_t i = 0; i < mock_audio_devices_count; i++)
        gtk_widget_set_visible(popup->audio_checks[i], i == model->audio_index);
    gtk_widget_set_visible(popup->audio_choices, !model->empty_audio_devices);
    gtk_widget_set_visible(popup->audio_empty, model->empty_audio_devices);
    gtk_widget_set_visible(popup->window, model->popup_visible && (incoming || outgoing || active || terminal));
#ifdef GDK_WINDOWING_X11
    if (gtk_widget_get_visible(popup->window) && !popup->placement_tick)
        popup->placement_tick = g_timeout_add(50, place_after_resize, popup);
#endif
    if (active && !popup->wave_tick) popup->wave_tick = g_timeout_add(250, animate_wave, popup);
    else if (!active && popup->wave_tick) {
        g_source_remove(popup->wave_tick);
        popup->wave_tick = 0;
    }
}

GtkWindow *mock_call_popup_window(MockCallPopup *popup)
{
    return GTK_WINDOW(popup->window);
}
