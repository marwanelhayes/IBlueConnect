#include "call-popup.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

struct MockCallPopup {
    GtkWidget *window, *avatar, *avatar_ring, *audio_content, *end_content, *mute_content, *keypad_content, *identity, *number, *subtitle, *timer;
    GtkWidget *waveform, *levels, *wave_labels, *wave_you, *wave_caller;
    GtkWidget *incoming, *outgoing, *active, *call_management, *hold_button, *resume_button, *merge_button, *terminal, *details;
    GtkWidget *compact_button, *expand_button, *mute_button, *keypad_button, *keypad, *tone_feedback, *open_app_button, *end_button;
    GtkWidget *audio_button, *audio_popover, *audio_choices, *audio_empty;
    GtkWidget *waiting, *waiting_caller, *waiting_decline, *hold_answer, *release_answer;
    GtkWidget *call_list, *swap_button, *add_call_button;
    char *list_key; /* What call_list shows; rows are rebuilt only when it changes. */
    GtkWidget **audio_checks;
    MockCallPopupAction action;
    gpointer user_data;
    MockCall phase;
    gboolean held;
    gboolean keypad_open;
    double incoming_voice, outgoing_voice; /* Decaying levels 0..1. */
    guint wave_step; /* Advances only with new levels, so a silent call never animates. */
    gboolean microphone_enabled;
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

/* Icon over a small caption, like the strip mockup's tiles; narrow enough for the 360 px controller. */
static GtkWidget *tile(GtkWidget *button, const char *icon, const char *text)
{
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *image = gtk_image_new_from_icon_name(icon);
    GtkWidget *caption = gtk_label_new(text);
    gtk_widget_add_css_class(caption, "caption");
    gtk_box_append(GTK_BOX(content), image);
    gtk_box_append(GTK_BOX(content), caption);
    g_object_set_data(G_OBJECT(content), "tile-image", image);
    g_object_set_data(G_OBJECT(content), "tile-caption", caption);
    if (GTK_IS_MENU_BUTTON(button)) gtk_menu_button_set_child(GTK_MENU_BUTTON(button), content);
    else gtk_button_set_child(GTK_BUTTON(button), content);
    return content;
}

/* icon may be NULL to keep it; an empty caption (compact mode) leaves the icon alone. */
static void set_tile(GtkWidget *content, const char *icon, const char *text)
{
    if (icon) gtk_image_set_from_icon_name(GTK_IMAGE(g_object_get_data(G_OBJECT(content), "tile-image")), icon);
    GtkWidget *caption = g_object_get_data(G_OBJECT(content), "tile-caption");
    gtk_label_set_text(GTK_LABEL(caption), text);
    gtk_widget_set_visible(caption, *text != '\0');
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
    if (popup->phase != MOCK_CALL_ACTIVE || popup->held || modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
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
#ifdef GDK_WINDOWING_X11
    if (popup->placement_tick) g_source_remove(popup->placement_tick);
#endif
    g_free(popup->audio_checks);
    g_free(popup->list_key);
    g_free(popup);
}

#ifdef GDK_WINDOWING_X11
static void on_realize(GtkWidget *window, gpointer data)
{
    (void)data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    gdk_x11_surface_set_user_time(surface, 0);
    /* GNOME Shell maps windows to apps by these. Unclaimed, the popup no longer makes the app look
     * open, so its icon launches the app (the running instance opens the main window). */
    Display *display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    Window xid = gdk_x11_surface_get_xid(surface);
    XDeleteProperty(display, xid, XInternAtom(display, "_GTK_APPLICATION_ID", False));
    XClassHint hint = { "iblueconnect-call", "IBlueConnect-Call" };
    XSetClassHint(display, xid, &hint);
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

#define SPEAKING_LEVEL 0.3
#define WAVE_BARS 22
/* GNOME palette green for the other person; your side follows the system (Ubuntu) accent. */
static const GdkRGBA wave_green = { 0.180, 0.761, 0.494, 1.0 };
static const GdkRGBA wave_idle = { 0.6, 0.6, 0.62, 1.0 };

/* The avatar ring takes the colour of whoever is speaking louder. */
static void show_voice(MockCallPopup *popup)
{
    gboolean me = popup->outgoing_voice >= SPEAKING_LEVEL && popup->outgoing_voice >= popup->incoming_voice;
    gboolean them = popup->incoming_voice >= SPEAKING_LEVEL && !me;
    if (me) gtk_widget_add_css_class(popup->avatar_ring, "speaking-me");
    else gtk_widget_remove_css_class(popup->avatar_ring, "speaking-me");
    if (them) gtk_widget_add_css_class(popup->avatar_ring, "speaking-them");
    else gtk_widget_remove_css_class(popup->avatar_ring, "speaking-them");
    gtk_widget_queue_draw(popup->waveform);
}

void mock_call_popup_set_voice(MockCallPopup *popup, double level)
{
    popup->incoming_voice = MAX(CLAMP(level, 0.0, 1.0), popup->incoming_voice * 0.7);
    if (popup->incoming_voice < 0.02) popup->incoming_voice = 0;
    popup->wave_step++;
    show_voice(popup);
}

static void rounded_bar(cairo_t *cr, double x, double y, double w, double h)
{
    double r = w / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + r, y + r, r, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, x + r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_close_path(cr);
}

/* Capsule bars: your voice enters from the left, theirs from the right, fading toward the middle. */
static void draw_wave(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    MockCallPopup *popup = data;
    GdkRGBA accent;
    gtk_widget_get_color(GTK_WIDGET(area), &accent);
    double gap = (double)width / WAVE_BARS, bar = gap * 0.55, middle = height / 2.0;
    for (int i = 0; i < WAVE_BARS; i++) {
        double t = (double)i / (WAVE_BARS - 1), phase = popup->wave_step * 0.6;
        double me = popup->outgoing_voice * exp(-3.2 * t) * (0.55 + 0.45 * fabs(sin(i * 1.7 + phase)));
        double them = popup->incoming_voice * exp(-3.2 * (1 - t)) * (0.55 + 0.45 * fabs(sin(i * 2.3 + phase * 1.3)));
        double level = MAX(me, them);
        const GdkRGBA *color = me >= them ? &accent : &wave_green;
        if (level < 0.06) { color = &wave_idle; level = 0.06; }
        double h = MAX(bar, level * height);
        gdk_cairo_set_source_rgba(cr, color);
        rounded_bar(cr, i * gap + (gap - bar) / 2, middle - h / 2, bar, h);
        cairo_fill(cr);
    }
}

void mock_call_popup_set_microphone_level(MockCallPopup *popup, double level)
{
    if (level < 0 || !popup->microphone_enabled) {
        if (level < 0) popup->outgoing_voice = 0;
        show_voice(popup);
        return;
    }
    popup->outgoing_voice = MAX(CLAMP(level, 0.0, 1.0), popup->outgoing_voice * 0.7);
    if (popup->outgoing_voice < 0.02) popup->outgoing_voice = 0;
    popup->wave_step++;
    show_voice(popup);
}

static const char *line_caller(const MockLine *line)
{
    return line->caller[0] ? line->caller : line->number[0] ? line->number : "Unknown caller";
}

static const char *line_status(const MockLine *line)
{
    switch (line->phase) {
    case MOCK_LINE_ACTIVE: return "Active";
    case MOCK_LINE_HELD: return "On hold";
    case MOCK_LINE_DIALING: return "Calling…";
    case MOCK_LINE_WAITING: return "Waiting";
    case MOCK_LINE_INCOMING: return "Incoming";
    default: return "";
    }
}

static void set_argument(GtkWidget *widget, const char *argument)
{
    g_object_set_data_full(G_OBJECT(widget), "mock-argument", g_strdup(argument), g_free);
}

static GtkWidget *row_button(MockCallPopup *popup, const char *text, const char *icon, const char *format,
                             unsigned index, const char *action, const MockLine *line)
{
    const char *id = line->id;
    char name[40];
    g_snprintf(name, sizeof name, format, index);
    GtkWidget *widget = action_button(popup, text, icon, g_intern_string(name), NULL);
    g_object_set_data(G_OBJECT(widget), "mock-action", (gpointer)action);
    set_argument(widget, id);
    char *spoken = g_strdup_printf("%s · %s", text, line_caller(line)); /* Rows share a tooltip; say whose. */
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, spoken, -1);
    g_free(spoken);
    return widget;
}

static GtkWidget *call_row(MockCallPopup *popup, const MockLine *line, unsigned index, gboolean can_private)
{
    char name[40];
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (line->multiparty) gtk_widget_set_margin_start(row, 12);
    GtkWidget *caller = label(line_caller(line), NULL);
    g_snprintf(name, sizeof name, "call-row-caller-%u", index);
    gtk_widget_set_name(caller, name);
    gtk_widget_set_hexpand(caller, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(caller), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), caller);
    GtkWidget *status = label(line_status(line), "dim-label");
    g_snprintf(name, sizeof name, "call-row-status-%u", index);
    gtk_widget_set_name(status, name);
    gtk_box_append(GTK_BOX(row), status);
    if (line->multiparty) {
        GtkWidget *split = row_button(popup, "Private", NULL, "call-row-private-%u", index, "private", line);
        gtk_widget_set_visible(split, can_private);
        gtk_box_append(GTK_BOX(row), split);
    }
    GtkWidget *end = row_button(popup, "End call", "call-stop-symbolic", "call-row-end-%u", index, "end-line", line);
    gtk_widget_add_css_class(end, "destructive-action");
    gtk_box_append(GTK_BOX(row), end);
    return row;
}

static void render_calls(MockCallPopup *popup, const MockUiModel *model)
{
    GString *key = g_string_new(NULL);
    unsigned members = 0;
    for (unsigned i = 0; i < model->call_count; i++) {
        const MockLine *line = &model->lines[i];
        members += line->multiparty;
        g_string_append_printf(key, "%s\n%s\n%s\n%d%d\n", line->id, line->caller, line->number,
                               line->phase, line->multiparty);
    }
    g_string_append_c(key, model->can_private ? 'p' : '-');
    if (popup->list_key && strcmp(popup->list_key, key->str) == 0) {
        g_string_free(key, TRUE);
        return;
    }
    g_free(popup->list_key);
    popup->list_key = g_string_free(key, FALSE);
    for (GtkWidget *child; (child = gtk_widget_get_first_child(popup->call_list));)
        gtk_box_remove(GTK_BOX(popup->call_list), child);
    if (members) {
        char title[32];
        g_snprintf(title, sizeof title, "Conference (%u)", members);
        GtkWidget *header = label(title, "heading");
        gtk_widget_set_name(header, "conference-header");
        gtk_box_append(GTK_BOX(popup->call_list), header);
    }
    for (int conference = 1; conference >= 0; conference--)
        for (unsigned i = 0; i < model->call_count; i++)
            if (model->lines[i].multiparty == conference)
                gtk_box_append(GTK_BOX(popup->call_list), call_row(popup, &model->lines[i], i, model->can_private));
}

static void install_css(void)
{
    static gboolean installed;
    if (installed) return;
    installed = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    /* Your voice uses the system accent, the caller's voice green, as on the iPhone. */
    gtk_css_provider_load_from_string(css,
        ".avatar-ring { padding: 3px; border-radius: 999px; }"
        ".avatar-ring.speaking-me { background-color: @accent_color; }"
        ".avatar-ring.speaking-them { background-color: #2ec27e; }"
        "#voice-waveform { color: @accent_color; }"
        ".voice-strip { background-color: alpha(@window_fg_color, 0.07); border-radius: 999px; padding: 4px 14px; }"
        "label.wave-you { color: @accent_color; font-weight: bold; font-size: 0.8em; }"
        "label.wave-you.muted { color: alpha(@window_fg_color, 0.5); }"
        "label.wave-caller { color: #2ec27e; font-weight: bold; font-size: 0.8em; }"
        /* The rest of the controller follows the voice strip: soft tinted tiles and full pills. */
        "window.call-controller button { border-radius: 12px; min-height: 40px; padding: 4px 10px; box-shadow: none;"
        " background-image: none; background-color: alpha(@window_fg_color, 0.07); }"
        "window.call-controller button:hover { background-color: alpha(@window_fg_color, 0.12); }"
        "window.call-controller button:active { background-color: alpha(@window_fg_color, 0.18); }"
        "window.call-controller button.call-answer, window.call-controller button.call-decline,"
        " window.call-controller button.destructive-action { border-radius: 999px; padding: 0 18px; font-weight: bold; }"
        "window.call-controller button.destructive-action { background-color: #c01c28; color: #fff; }"
        "window.call-controller button.destructive-action:hover { background-color: #d02030; }"
        "window.call-controller button.destructive-action:active { background-color: #a01824; }"
        "window.call-controller button.call-answer { background-color: #187833; background-image: none; color: #fff; }"
        "window.call-controller button.call-answer:hover { background-color: #1b803a; }"
        "window.call-controller button.call-answer:active { background-color: #14652b; }"
        "window.call-controller button.call-decline { background-color: #c01c28; background-image: none; color: #fff; }"
        "window.call-controller button.call-decline:hover { background-color: #d02030; }"
        "window.call-controller button.call-decline:active { background-color: #a01824; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

MockCallPopup *mock_call_popup_new(GtkApplication *app, MockCallPopupAction action, gpointer user_data)
{
    install_css();
    MockCallPopup *popup = g_new0(MockCallPopup, 1);
    popup->action = action;
    popup->user_data = user_data;
    popup->phase = MOCK_CALL_IDLE;
    GtkWidget *window = adw_application_window_new(app);
    popup->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "IBlueConnect Call");
    gtk_widget_add_css_class(window, "call-controller");
    gtk_window_set_default_size(GTK_WINDOW(window), 360, -1);
    /* Adwaita windows default to a 200 px minimum height; hug the content instead (no empty band). */
    gtk_widget_set_size_request(window, 360, 1);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close), popup);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), popup);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), popup);
    gtk_widget_add_controller(window, keys);
#ifdef GDK_WINDOWING_X11
    g_signal_connect(window, "realize", G_CALLBACK(on_realize), popup);
#endif
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(body, 14);
    gtk_widget_set_margin_bottom(body, 14);
    gtk_widget_set_margin_start(body, 14);
    gtk_widget_set_margin_end(body, 14);
    GtkWidget *drag_handle = gtk_window_handle_new(); /* Drag the popup by any non-button area. */
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(drag_handle), body);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), drag_handle);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    popup->avatar = adw_avatar_new(40, NULL, TRUE);
    gtk_widget_set_name(popup->avatar, "caller-avatar");
    /* The avatar clips anything drawn outside its circle, so the speaking ring is a padded round backdrop. */
    popup->avatar_ring = adw_bin_new();
    gtk_widget_set_name(popup->avatar_ring, "caller-ring");
    gtk_widget_add_css_class(popup->avatar_ring, "avatar-ring");
    gtk_widget_set_valign(popup->avatar_ring, GTK_ALIGN_CENTER);
    adw_bin_set_child(ADW_BIN(popup->avatar_ring), popup->avatar);
    gtk_box_append(GTK_BOX(top), popup->avatar_ring);
    GtkWidget *identity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(identity_box, TRUE);
    popup->identity = label("", "heading");
    gtk_widget_set_name(popup->identity, "caller-identity");
    gtk_box_append(GTK_BOX(identity_box), popup->identity);
    popup->subtitle = label("via iPhone", "dim-label");
    gtk_widget_set_name(popup->subtitle, "call-subtitle");
    gtk_box_append(GTK_BOX(identity_box), popup->subtitle);
    gtk_box_append(GTK_BOX(top), identity_box);
    popup->timer = label("", "heading");
    gtk_widget_set_name(popup->timer, "call-timer");
    gtk_box_append(GTK_BOX(top), popup->timer);
    popup->open_app_button = action_button(popup, "Open app", NULL, "open-app", "");
    gtk_box_append(GTK_BOX(top), popup->open_app_button);
    gtk_box_append(GTK_BOX(body), top);

    popup->number = label("", "dim-label");
    gtk_widget_set_name(popup->number, "caller-number");
    gtk_box_append(GTK_BOX(body), popup->number);
    popup->levels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(strip, "voice-strip");
    popup->waveform = gtk_drawing_area_new();
    gtk_widget_set_name(popup->waveform, "voice-waveform");
    gtk_widget_set_hexpand(popup->waveform, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(popup->waveform), 56);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(popup->waveform), draw_wave, popup, NULL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->waveform), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Voice activity: your voice from the left, the caller's from the right", -1);
    gtk_widget_set_tooltip_text(popup->waveform, "Audio on the call path; reception is not confirmed");
    gtk_box_append(GTK_BOX(strip), popup->waveform);
    gtk_box_append(GTK_BOX(popup->levels), strip);
    popup->wave_labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    popup->wave_you = label("You", "wave-you");
    gtk_widget_set_name(popup->wave_you, "wave-you");
    gtk_widget_set_hexpand(popup->wave_you, TRUE);
    gtk_box_append(GTK_BOX(popup->wave_labels), popup->wave_you);
    popup->wave_caller = label("", "wave-caller");
    gtk_widget_set_name(popup->wave_caller, "wave-caller");
    gtk_label_set_xalign(GTK_LABEL(popup->wave_caller), 1);
    gtk_label_set_ellipsize(GTK_LABEL(popup->wave_caller), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(popup->wave_labels), popup->wave_caller);
    gtk_box_append(GTK_BOX(popup->levels), popup->wave_labels);
    gtk_box_append(GTK_BOX(body), popup->levels);
    popup->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    popup->keypad = make_keypad(popup);
    gtk_box_append(GTK_BOX(popup->details), popup->keypad);
    gtk_box_append(GTK_BOX(body), popup->details);

    popup->waiting = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(popup->waiting, "waiting-banner");
    GtkWidget *waiting_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    popup->waiting_caller = label("", "heading");
    gtk_widget_set_name(popup->waiting_caller, "waiting-caller");
    gtk_widget_set_hexpand(popup->waiting_caller, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(popup->waiting_caller), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(waiting_top), popup->waiting_caller);
    GtkWidget *waiting_status = label("Waiting", "dim-label");
    gtk_widget_set_name(waiting_status, "waiting-status");
    gtk_box_append(GTK_BOX(waiting_top), waiting_status);
    gtk_box_append(GTK_BOX(popup->waiting), waiting_top);
    GtkWidget *waiting_actions = gtk_grid_new(); /* Two by two: four labels don't fit one 360 px row. */
    gtk_grid_set_row_spacing(GTK_GRID(waiting_actions), 6);
    gtk_grid_set_column_spacing(GTK_GRID(waiting_actions), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(waiting_actions), TRUE);
    GtkWidget *waiting_ignore = action_button(popup, "Ignore", NULL, "waiting-ignore", "");
    g_object_set_data(G_OBJECT(waiting_ignore), "mock-action", "ignore-waiting");
    gtk_grid_attach(GTK_GRID(waiting_actions), waiting_ignore, 0, 0, 1, 1);
    popup->waiting_decline = action_button(popup, "Decline", NULL, "waiting-decline", "");
    g_object_set_data(G_OBJECT(popup->waiting_decline), "mock-action", "end-line");
    gtk_widget_add_css_class(popup->waiting_decline, "call-decline");
    gtk_grid_attach(GTK_GRID(waiting_actions), popup->waiting_decline, 1, 0, 1, 1);
    popup->release_answer = action_button(popup, "End & Accept", NULL, "release-and-answer", "");
    gtk_grid_attach(GTK_GRID(waiting_actions), popup->release_answer, 0, 1, 1, 1);
    popup->hold_answer = action_button(popup, "Hold & Accept", NULL, "hold-and-answer", "");
    gtk_widget_add_css_class(popup->hold_answer, "call-answer");
    gtk_grid_attach(GTK_GRID(waiting_actions), popup->hold_answer, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(popup->waiting), waiting_actions);
    gtk_box_append(GTK_BOX(body), popup->waiting);

    popup->call_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(popup->call_list, "call-list");
    gtk_box_append(GTK_BOX(body), popup->call_list);

    popup->incoming = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(popup->incoming, "incoming-actions");
    gtk_box_set_homogeneous(GTK_BOX(popup->incoming), TRUE); /* Equal pills across the row. */
    gtk_box_append(GTK_BOX(popup->incoming), action_button(popup, "Ignore", NULL, "ignore", ""));
    GtkWidget *decline = action_button(popup, "Decline", NULL, "decline", "");
    gtk_widget_add_css_class(decline, "call-decline");
    gtk_box_append(GTK_BOX(popup->incoming), decline);
    GtkWidget *answer = action_button(popup, "Answer", NULL, "answer", "");
    gtk_widget_add_css_class(answer, "call-answer");
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
    popup->mute_button = action_button(popup, "Mute", "audio-input-microphone-symbolic", "mute", "");
    popup->mute_content = tile(popup->mute_button, "audio-input-microphone-symbolic", "Mute");
    gtk_box_append(GTK_BOX(popup->active), popup->mute_button);
    popup->audio_button = make_audio(popup);
    popup->audio_content = tile(popup->audio_button, "audio-speakers-symbolic", "Audio");
    gtk_box_append(GTK_BOX(popup->active), popup->audio_button);
    GtkWidget *keypad_button = button("Keypad", "input-dialpad-symbolic", "keypad-toggle");
    popup->keypad_button = keypad_button;
    popup->keypad_content = tile(keypad_button, "input-dialpad-symbolic", "Keypad");
    g_signal_connect(keypad_button, "clicked", G_CALLBACK(toggle_keypad), popup);
    gtk_box_append(GTK_BOX(popup->active), keypad_button);
    popup->compact_button = action_button(popup, "Compact", "view-restore-symbolic", "compact", "1");
    gtk_box_append(GTK_BOX(popup->active), popup->compact_button);
    popup->expand_button = action_button(popup, "Expand", "view-fullscreen-symbolic", "expand", "0");
    g_object_set_data(G_OBJECT(popup->expand_button), "mock-action", "compact");
    gtk_box_append(GTK_BOX(popup->active), popup->expand_button);
    GtkWidget *end = action_button(popup, "End call", "call-stop-symbolic", "end", "");
    popup->end_button = end;
    gtk_widget_add_css_class(end, "destructive-action");
    gtk_widget_set_hexpand(end, TRUE); /* A wide End pill, as in the strip mockup. */
    popup->end_content = labeled(end, "call-stop-symbolic", "End");
    gtk_box_append(GTK_BOX(popup->active), end);
    gtk_box_append(GTK_BOX(body), popup->active);

    popup->call_management = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    popup->hold_button = action_button(popup, "Hold", NULL, "hold", "");
    popup->resume_button = action_button(popup, "Resume", NULL, "resume", "");
    popup->merge_button = action_button(popup, "Merge calls", NULL, "merge", "");
    gtk_box_append(GTK_BOX(popup->call_management), popup->hold_button);
    gtk_box_append(GTK_BOX(popup->call_management), popup->resume_button);
    gtk_box_append(GTK_BOX(popup->call_management), popup->merge_button);
    popup->swap_button = action_button(popup, "Swap", NULL, "swap", "");
    gtk_box_append(GTK_BOX(popup->call_management), popup->swap_button);
    popup->add_call_button = action_button(popup, "Add call", NULL, "add-call", "");
    gtk_box_append(GTK_BOX(popup->call_management), popup->add_call_button);
    gtk_box_append(GTK_BOX(body), popup->call_management);

    popup->terminal = label("", "heading");
    gtk_widget_set_name(popup->terminal, "terminal-label");
    gtk_box_append(GTK_BOX(body), popup->terminal);
    show_voice(popup);
    gtk_widget_set_visible(window, FALSE);
    return popup;
}

void mock_call_popup_update(MockCallPopup *popup, const MockUiModel *model)
{
    popup->phase = model->call;
    popup->held = model->call_held;
    gboolean incoming = model->call == MOCK_CALL_INCOMING;
    gboolean outgoing = model->call == MOCK_CALL_OUTGOING || model->call == MOCK_CALL_CONNECTING;
    gboolean active = model->call == MOCK_CALL_ACTIVE;
    popup->microphone_enabled = active && !model->muted && !model->call_held &&
                                (!model->live || model->call_audio);
    if (!popup->microphone_enabled) {
        popup->outgoing_voice = 0;
        show_voice(popup);
    }
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(popup->waveform), model->compact ? 20 : 56);
    gtk_widget_set_visible(popup->wave_labels, !model->compact);
    if (model->muted) gtk_widget_add_css_class(popup->wave_you, "muted");
    else gtk_widget_remove_css_class(popup->wave_you, "muted");
    gtk_widget_set_visible(popup->levels, active);
    gboolean terminal = model->call == MOCK_CALL_DECLINED || model->call == MOCK_CALL_ENDING ||
                        model->call == MOCK_CALL_ENDED;
    if (!active || model->call_held) {
        popup->keypad_open = FALSE;
        gtk_label_set_text(GTK_LABEL(popup->tone_feedback), "");
    }
    gtk_widget_set_visible(popup->incoming, incoming);
    gtk_widget_set_visible(popup->outgoing, outgoing);
    gtk_widget_set_visible(popup->active, active);
    gtk_widget_set_visible(popup->call_management, active && (model->can_hold || model->can_resume ||
                           model->can_merge || model->can_swap || model->can_add_call));
    gtk_widget_set_visible(popup->hold_button, active && model->can_hold);
    gtk_widget_set_visible(popup->resume_button, active && model->can_resume && !model->can_swap); /* Swap resumes too. */
    gtk_widget_set_visible(popup->merge_button, active && model->can_merge);
    gtk_widget_set_visible(popup->swap_button, active && model->can_swap);
    gtk_widget_set_visible(popup->add_call_button, active && model->can_add_call);
    const MockLine *waiting = NULL;
    gboolean conference = FALSE;
    for (unsigned i = 0; i < model->call_count; i++) {
        if (strcmp(model->lines[i].id, model->waiting_call_id) == 0) waiting = &model->lines[i];
        conference |= model->lines[i].multiparty;
    }
    gtk_widget_set_visible(popup->waiting, active && model->waiting_visible && waiting);
    if (waiting) {
        gtk_label_set_text(GTK_LABEL(popup->waiting_caller), line_caller(waiting));
        set_argument(popup->waiting_decline, waiting->id);
        set_argument(popup->hold_answer, waiting->id);
        set_argument(popup->release_answer, waiting->id);
    }
    gtk_widget_set_visible(popup->hold_answer, model->can_hold_and_answer);
    gtk_widget_set_visible(popup->release_answer, model->can_release_and_answer);
    render_calls(popup, model);
    gtk_widget_set_visible(popup->call_list, (active || outgoing) && !model->compact &&
                           (model->call_count >= 2 || conference));
    gtk_widget_set_visible(popup->mute_button, !model->live || model->call_audio);
    /* No audio routing backend yet. */
    gtk_widget_set_visible(popup->audio_button, !model->live);
    gtk_widget_set_visible(popup->terminal, terminal);
    gtk_widget_set_visible(popup->details, active && !model->compact);
    gtk_widget_set_visible(popup->compact_button, active && !model->compact);
    gtk_widget_set_visible(popup->expand_button, active && model->compact);
    gtk_widget_set_visible(popup->keypad_button, active && !model->compact && !model->call_held);
    gtk_widget_set_visible(popup->number, (incoming || outgoing || active) && !model->compact);
    gtk_widget_set_visible(popup->keypad, active && !model->compact && !model->call_held && popup->keypad_open);
    gtk_widget_set_visible(popup->timer, active);
    gtk_widget_set_visible(popup->open_app_button, incoming || active);
    if (active) {
        char duration[32];
        g_snprintf(duration, sizeof duration, "%02u:%02u", model->elapsed_seconds / 60, model->elapsed_seconds % 60);
        gtk_label_set_text(GTK_LABEL(popup->timer), duration);
    }
    const MockContact *contact = model->caller_index >= 0 &&
                                 (size_t)model->caller_index < mock_contacts_count
                                 ? &mock_contacts[model->caller_index] : NULL;
    gtk_label_set_text(GTK_LABEL(popup->identity), contact ? contact->name :
                       model->live && model->caller_name[0] ? model->caller_name :
                       model->dialed_number[0] ? model->dialed_number : "Unknown caller");
    gtk_label_set_text(GTK_LABEL(popup->wave_caller), gtk_label_get_text(GTK_LABEL(popup->identity)));
    gtk_label_set_text(GTK_LABEL(popup->number), model->live ? model->dialed_number :
                       contact ? contact->numbers[0] : incoming ? "Unknown caller" : "");
    if (active && model->call_held)
        gtk_label_set_text(GTK_LABEL(popup->subtitle), "On hold · via iPhone");
    else if (active && model->call_count > 1) {
        char subtitle[64];
        g_snprintf(subtitle, sizeof subtitle, "%u calls · via iPhone", model->call_count);
        gtk_label_set_text(GTK_LABEL(popup->subtitle), subtitle);
    } else
        gtk_label_set_text(GTK_LABEL(popup->subtitle), incoming ? "Incoming call · via iPhone" :
                           outgoing ? "Calling… via iPhone" : "via iPhone");
    gtk_label_set_text(GTK_LABEL(popup->terminal), model->call == MOCK_CALL_DECLINED ? "Call Declined" :
                       model->call == MOCK_CALL_ENDING ? "Ending call…" : "Call Ended");
    gtk_widget_set_tooltip_text(popup->mute_button, model->muted ? "Unmute" : "Mute");
    set_tile(popup->mute_content, model->muted ? "microphone-sensitivity-muted-symbolic" :
             "audio-input-microphone-symbolic", model->compact ? "" : model->muted ? "Muted" : "Mute");
    set_tile(popup->keypad_content, NULL, model->compact ? "" : "Keypad");
    set_tile(popup->audio_content, NULL, model->compact ? "" : "Audio");
    const char *end_label = active && model->can_end_conference ? "End conference" : "End call";
    g_object_set_data(G_OBJECT(popup->end_button), "mock-action",
                      (gpointer)(active && model->can_end_conference ? "end-conference" : "end"));
    gtk_widget_set_tooltip_text(popup->end_button, end_label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->end_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   end_label, -1);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(popup->end_content),
                                 model->compact ? "" : active && model->can_end_conference ? "End conference" : "End");
    adw_avatar_set_text(ADW_AVATAR(popup->avatar), contact ? contact->name : NULL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(popup->mute_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   model->muted ? "Unmute" : "Mute", -1);
    for (size_t i = 0; i < mock_audio_devices_count; i++)
        gtk_widget_set_visible(popup->audio_checks[i], i == model->audio_index);
    gtk_widget_set_visible(popup->audio_choices, !model->empty_audio_devices);
    gtk_widget_set_visible(popup->audio_empty, model->empty_audio_devices);
    gboolean was_visible = gtk_widget_get_visible(popup->window);
    gtk_widget_set_visible(popup->window, model->popup_visible && (incoming || outgoing || active || terminal));
#ifdef GDK_WINDOWING_X11
    if (gtk_widget_get_visible(popup->window) && !was_visible && !popup->placement_tick)
        popup->placement_tick = g_timeout_add(50, place_after_resize, popup);
    else if (!gtk_widget_get_visible(popup->window) && popup->placement_tick) {
        g_source_remove(popup->placement_tick);
        popup->placement_tick = 0;
    }
#endif
    if (!active || model->call_held || (model->live && !model->call_audio)) {
        popup->incoming_voice = 0;
        show_voice(popup);
    }
}

GtkWindow *mock_call_popup_window(MockCallPopup *popup)
{
    return GTK_WINDOW(popup->window);
}
