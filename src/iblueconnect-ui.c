#include <adwaita.h>

static const char *status_text(const char *value)
{
    if (g_strcmp0(value, "ready") == 0) return "Ready for calls";
    if (g_strcmp0(value, "manage calls on phone") == 0) return "Manage calls on phone";
    return "Calling unavailable";
}

static const char *phase_text(const char *value)
{
    if (g_strcmp0(value, "idle") == 0) return "No current call";
    if (g_strcmp0(value, "incoming") == 0) return "Incoming call";
    if (g_strcmp0(value, "dialing") == 0) return "Dialing";
    if (g_strcmp0(value, "alerting") == 0) return "Ringing";
    if (g_strcmp0(value, "active") == 0) return "Active call";
    if (g_strcmp0(value, "ending") == 0) return "Ending call";
    if (g_strcmp0(value, "ended") == 0) return "Call ended";
    return "No call details";
}

static const char *audio_text(const char *value)
{
    if (g_strcmp0(value, "phone") == 0) return "Phone";
    if (g_strcmp0(value, "laptop") == 0) return "Laptop";
    if (g_strcmp0(value, "switching") == 0) return "Switching audio";
    return "Unknown";
}

enum { ACT_DIAL = 1, ACT_ANSWER = 2, ACT_DECLINE = 4, ACT_HANG_UP = 8, ACT_IGNORE = 16 };

static unsigned actions_for(const char *status, const char *phase, gboolean pending)
{
    if (pending || g_strcmp0(status, "ready") != 0) return 0;
    if (g_strcmp0(phase, "idle") == 0) return ACT_DIAL;
    if (g_strcmp0(phase, "incoming") == 0) return ACT_ANSWER | ACT_DECLINE | ACT_IGNORE;
    if (g_strcmp0(phase, "dialing") == 0 || g_strcmp0(phase, "alerting") == 0 ||
        g_strcmp0(phase, "active") == 0) return ACT_HANG_UP;
    return 0;
}

/* Loose pre-check to enable Call; the daemon validates the number again. */
static gboolean dial_input_ok(const char *text)
{
    gboolean digit = FALSE;
    for (const char *c = text; *c; c++) {
        if (g_ascii_isdigit(*c)) digit = TRUE;
        else if (*c == '+' && c == text) continue;
        else if (*c != '*' && *c != '#' && *c != ' ') return FALSE;
    }
    return digit;
}

static char *dial_digits(const char *text)
{
    GString *out = g_string_new(NULL);
    for (const char *c = text; *c; c++)
        if (*c != ' ') g_string_append_c(out, *c);
    return g_string_free(out, FALSE);
}

/* The popup shows a ringing call, then stays as call controls only if answered from it. */
static gboolean popup_visible(const char *status, const char *phase, gboolean answered_here)
{
    if (g_strcmp0(status, "ready") != 0) return FALSE;
    if (g_strcmp0(phase, "incoming") == 0) return TRUE;
    return answered_here && (actions_for(status, phase, FALSE) & ACT_HANG_UP);
}

/* Only a ringing popup is kept on top and uncloseable; after answering it is an ordinary window. */
static gboolean popup_pinned(const char *phase)
{
    return g_strcmp0(phase, "incoming") == 0;
}

/* Keypad presses become DTMF only during an active call; otherwise they edit the number. */
static gboolean keypad_sends_tone(const char *status, const char *phase)
{
    return g_strcmp0(status, "ready") == 0 && g_strcmp0(phase, "active") == 0;
}

/* Case-insensitive name match, or digit match ignoring spaces in the number. */
static gboolean contact_matches(const char *name, const char *number, const char *query)
{
    if (!*query) return TRUE;
    char *folded_query = g_utf8_casefold(query, -1), *folded_name = g_utf8_casefold(name, -1);
    gboolean hit = strstr(folded_name, folded_query) != NULL;
    g_free(folded_name);
    g_free(folded_query);
    if (hit || !dial_input_ok(query)) return hit;
    char *digits = dial_digits(query), *stored = dial_digits(number);
    hit = strstr(stored, digits) != NULL;
    g_free(digits);
    g_free(stored);
    return hit;
}

static const char *recent_kind_text(const char *kind)
{
    if (g_strcmp0(kind, "missed") == 0) return "Missed";
    if (g_strcmp0(kind, "received") == 0) return "Incoming";
    if (g_strcmp0(kind, "dialed") == 0) return "Outgoing";
    if (g_strcmp0(kind, "declined") == 0) return "Declined";
    if (g_strcmp0(kind, "failed") == 0) return "Failed";
    return "Call";
}

static const char *recent_title(const char *name, const char *number)
{
    return *name ? name : *number ? number : "Unknown caller";
}

static guint local_day(GDateTime *time)
{
    int year, month, day;
    g_date_time_get_ymd(time, &year, &month, &day);
    GDate *date = g_date_new_dmy(day, month, year);
    guint julian = g_date_get_julian(date);
    g_date_free(date);
    return julian;
}

/* Time for today's calls, "Yesterday", weekday within a week, otherwise the date. */
static char *recent_when(gint64 started, GDateTime *now)
{
    GDateTime *then = g_date_time_new_from_unix_local(started);
    guint days = local_day(now) - local_day(then);
    char *text = g_date_time_format(then, days == 0 ? "%H:%M" : days < 7 ? "%A" : "%Y-%m-%d");
    if (days == 1) {
        g_free(text);
        text = g_strdup("Yesterday");
    }
    g_date_time_unref(then);
    return text;
}

#ifndef IBLUECONNECT_UI_TEST
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

typedef struct {
    gboolean popup;
    GtkWidget *window, *status, *phase, *audio, *caller, *error;
    GtkWidget *entry, *call, *answer, *decline, *hang_up, *ignore;
    GtkWidget *stack, *contacts, *search, *sync_status, *sync, *actions, *recents, *recents_status;
    GDBusProxy *daemon;
    char *state, *stage, *call_id;
    const char *method;
    gboolean pending, answered_here, shown, ignored, pinned;
} View;

static View view;
static void load_contacts(View *self);
static void load_recents(View *self);

/* Empty labels are hidden so they do not leave gaps in the layout. */
static void set_text(GtkWidget *label, const char *text)
{
    if (!label) return;
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_visible(label, *text != '\0');
}

static void update_controls(View *self)
{
    if (!self->window) return;
    unsigned acts = actions_for(self->state, self->stage, self->pending);
    if (self->answer) gtk_widget_set_visible(self->answer, acts & ACT_ANSWER);
    if (self->decline) gtk_widget_set_visible(self->decline, acts & ACT_DECLINE);
    if (self->hang_up) gtk_widget_set_visible(self->hang_up, acts & ACT_HANG_UP);
    if (self->ignore) gtk_widget_set_visible(self->ignore, acts & ACT_IGNORE);
    if (self->actions) gtk_widget_set_visible(self->actions, acts & (ACT_ANSWER | ACT_DECLINE | ACT_HANG_UP));
    if (self->call && self->entry)
        gtk_widget_set_sensitive(self->call, (acts & ACT_DIAL) &&
            dial_input_ok(gtk_editable_get_text(GTK_EDITABLE(self->entry))));
}

static void set_above(GtkWidget *window, gboolean above)
{
#ifdef GDK_WINDOWING_X11
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!GDK_IS_X11_SURFACE(surface)) return;
    Display *display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.window = gdk_x11_surface_get_xid(surface);
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = above ? 1 : 0; /* _NET_WM_STATE_ADD / _REMOVE */
    event.xclient.data.l[1] = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    event.xclient.data.l[3] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
#else
    (void)window;
    (void)above;
#endif
}

static void on_popup_realize(GtkWidget *window, gpointer data)
{
    (void)data;
#ifdef GDK_WINDOWING_X11
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (GDK_IS_X11_SURFACE(surface)) {
        /* Map without taking keyboard focus. The popup keeps its taskbar/Alt+Tab entry so a
         * minimized call window can always be found again. */
        gdk_x11_surface_set_user_time(surface, 0);
    }
#else
    (void)window;
#endif
}

static void on_popup_map(GtkWidget *window, gpointer data)
{
    View *self = data;
    set_above(window, self->pinned);
}

static void show_state(View *self, const char *status, const char *id, const char *phase,
                       const char *audio, const char *caller)
{
    g_free(self->state);
    g_free(self->stage);
    self->state = g_strdup(status);
    self->stage = g_strdup(phase);
    if (self->popup) {
        if (!self->call_id && g_strcmp0(phase, "incoming") == 0) self->call_id = g_strdup(id);
        if (g_strcmp0(self->call_id, id) != 0 || !popup_visible(status, phase, self->answered_here)) {
            gtk_window_destroy(GTK_WINDOW(self->window)); /* hides UI only; never ends the call */
            return;
        }
    } else {
        g_free(self->call_id);
        self->call_id = g_strdup(id);
    }
    set_text(self->status, status_text(status));
    set_text(self->phase, phase_text(phase));
    set_text(self->audio, audio_text(audio));
    set_text(self->caller, caller);
    update_controls(self);
    if (self->popup && self->pinned != popup_pinned(phase)) {
        self->pinned = popup_pinned(phase);
        gtk_window_set_deletable(GTK_WINDOW(self->window), !self->pinned);
        if (gtk_widget_get_mapped(self->window)) set_above(self->window, self->pinned);
    }
    if (self->popup && !self->shown && !self->ignored) {
        self->shown = TRUE;
        gtk_widget_set_visible(self->window, TRUE);
    }
}

static void show_unavailable(View *self)
{
    show_state(self, "calling unavailable", "", "idle", "unknown", "");
}

static void on_snapshot(GObject *source, GAsyncResult *result, gpointer data)
{
    View *self = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (!self->window) {
    } else if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(sssss)"))) {
        show_unavailable(self);
    } else {
        const char *status, *id, *phase, *audio, *caller;
        g_variant_get(reply, "(&s&s&s&s&s)", &status, &id, &phase, &audio, &caller);
        show_state(self, status, id, phase, audio, caller);
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
}

static void refresh(View *self)
{
    if (!self->window) return;
    if (!self->daemon || !g_dbus_proxy_get_name_owner(self->daemon)) {
        show_unavailable(self);
        return;
    }
    g_dbus_proxy_call(self->daemon, "Snapshot", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      3000, NULL, on_snapshot, self);
}

static void on_action_done(GObject *source, GAsyncResult *result, gpointer data)
{
    View *self = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    self->pending = FALSE;
    if (error) {
        g_dbus_error_strip_remote_error(error);
        set_text(self->error, error->message);
    } else {
        set_text(self->error, "");
        if (g_strcmp0(self->method, "Answer") == 0) self->answered_here = TRUE;
        if (g_strcmp0(self->method, "Dial") == 0 && self->entry)
            gtk_editable_set_text(GTK_EDITABLE(self->entry), "");
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
    update_controls(self);
    refresh(self);
}

static void send_action(View *self, const char *method, const char *argument)
{
    if (self->pending || !self->daemon) return;
    self->pending = TRUE;
    self->method = method;
    set_text(self->error, "");
    update_controls(self);
    g_dbus_proxy_call(self->daemon, method, g_variant_new("(s)", argument),
                      G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000, NULL, on_action_done, self);
}

static void on_call_action(GtkButton *button, gpointer data)
{
    View *self = &view;
    const char *method = data;
    unsigned needed = g_strcmp0(method, "Answer") == 0 ? ACT_ANSWER
        : g_strcmp0(method, "Decline") == 0 ? ACT_DECLINE : ACT_HANG_UP;
    (void)button;
    if (self->call_id && (actions_for(self->state, self->stage, self->pending) & needed))
        send_action(self, method, self->call_id);
}

static void on_dial(GtkWidget *widget, gpointer data)
{
    View *self = data;
    (void)widget;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->entry));
    if (!(actions_for(self->state, self->stage, self->pending) & ACT_DIAL) || !dial_input_ok(text)) return;
    char *digits = dial_digits(text);
    send_action(self, "Dial", digits);
    g_free(digits);
}

/* Ignore only hides the popup; the call keeps ringing on the iPhone. The process stays
 * alive (hidden) until ringing ends, so a daemon re-show for this call stays hidden. */
static void on_ignore(GtkButton *button, gpointer data)
{
    View *self = data;
    (void)button;
    self->ignored = TRUE;
    gtk_widget_set_visible(self->window, FALSE);
}

static void on_entry_changed(GtkEditable *editable, gpointer data)
{
    (void)editable;
    update_controls(data);
}

static gboolean on_close_request(GtkWindow *window, gpointer data)
{
    View *self = data;
    (void)window;
    /* Escape/Alt+F4 must not hide a ringing popup; later, closing only hides the UI. */
    return self->popup && popup_pinned(self->stage);
}

static void on_tone_done(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (error) {
        g_dbus_error_strip_remote_error(error);
        set_text(((View *)data)->error, error->message);
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
}

static void on_key(GtkButton *button, gpointer data)
{
    View *self = data;
    const char *key = gtk_button_get_label(button);
    if (keypad_sends_tone(self->state, self->stage)) {
        if (self->daemon && self->call_id && g_strcmp0(key, "+") != 0)
            g_dbus_proxy_call(self->daemon, "SendTone", g_variant_new("(ss)", self->call_id, key),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_tone_done, self);
        return;
    }
    int position = -1;
    gtk_editable_insert_text(GTK_EDITABLE(self->entry), key, -1, &position);
}

static void on_backspace(GtkButton *button, gpointer data)
{
    View *self = data;
    (void)button;
    int length = (int)g_utf8_strlen(gtk_editable_get_text(GTK_EDITABLE(self->entry)), -1);
    if (length > 0) gtk_editable_delete_text(GTK_EDITABLE(self->entry), length - 1, length);
}

static void on_contact_call(GtkButton *button, gpointer data)
{
    View *self = data;
    const char *number = g_object_get_data(G_OBJECT(button), "number");
    if (!(actions_for(self->state, self->stage, self->pending) & ACT_DIAL) || !dial_input_ok(number)) {
        set_text(self->sync_status, "Calling is available when the phone is ready and no call is active");
        return;
    }
    char *digits = dial_digits(number);
    send_action(self, "Dial", digits);
    g_free(digits);
}

static gboolean filter_contact(GtkListBoxRow *row, gpointer data)
{
    View *self = data;
    if (!self->search) return TRUE;
    return contact_matches(g_object_get_data(G_OBJECT(row), "name"),
                           g_object_get_data(G_OBJECT(row), "number"),
                           gtk_editable_get_text(GTK_EDITABLE(self->search)));
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    View *self = data;
    (void)entry;
    if (self->contacts) gtk_list_box_invalidate_filter(GTK_LIST_BOX(self->contacts));
}

static GtkWidget *contact_row(View *self, const char *name, const char *kind, const char *number)
{
    GtkWidget *row = adw_action_row_new();
    char *subtitle = g_strdup_printf("%s · %s", kind, number);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name), g_free);
    g_object_set_data_full(G_OBJECT(row), "number", g_strdup(number), g_free);
    GtkWidget *call = gtk_button_new_from_icon_name("call-start-symbolic");
    char *accessible = g_strdup_printf("Call %s, %s", name, kind);
    gtk_accessible_update_property(GTK_ACCESSIBLE(call), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
    gtk_widget_set_tooltip_text(call, "Call");
    gtk_widget_add_css_class(call, "answer");
    gtk_widget_add_css_class(call, "circular");
    gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(call), "number", g_strdup(number), g_free);
    g_signal_connect(call, "clicked", G_CALLBACK(on_contact_call), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), call);
    g_free(accessible);
    g_free(subtitle);
    return row;
}

static void on_contacts(GObject *source, GAsyncResult *result, gpointer data)
{
    View *self = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (!self->contacts) {
    } else if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(sa(sss))"))) {
        set_text(self->sync_status, "Contacts unavailable");
    } else {
        const char *status, *name, *kind, *number;
        GVariantIter *entries;
        g_variant_get(reply, "(&sa(sss))", &status, &entries);
        set_text(self->sync_status, status);
        gtk_list_box_remove_all(GTK_LIST_BOX(self->contacts));
        while (g_variant_iter_next(entries, "(&s&s&s)", &name, &kind, &number))
            gtk_list_box_append(GTK_LIST_BOX(self->contacts), contact_row(self, name, kind, number));
        g_variant_iter_free(entries);
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
}

static void load_contacts(View *self)
{
    if (self->popup || !self->contacts || !self->daemon || !g_dbus_proxy_get_name_owner(self->daemon)) return;
    g_dbus_proxy_call(self->daemon, "Contacts", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      10000, NULL, on_contacts, self);
}

static void on_sync_done(GObject *source, GAsyncResult *result, gpointer data)
{
    View *self = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (error) {
        g_dbus_error_strip_remote_error(error);
        set_text(self->sync_status, error->message);
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
}

static void on_sync(GtkButton *button, gpointer data)
{
    View *self = data;
    (void)button;
    if (!self->daemon || !g_dbus_proxy_get_name_owner(self->daemon)) {
        set_text(self->sync_status, "Calling service unavailable");
        return;
    }
    g_dbus_proxy_call(self->daemon, "SyncContacts", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      10000, NULL, on_sync_done, self);
}

static GtkWidget *recent_row(View *self, const char *name, const char *number, const char *kind,
                            gint64 started, GDateTime *now)
{
    GtkWidget *row = adw_action_row_new();
    char *when = recent_when(started, now);
    char *subtitle = g_strdup_printf("%s · %s", recent_kind_text(kind), when);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), recent_title(name, number));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    const char *icon = g_strcmp0(kind, "missed") == 0 ? "call-missed-symbolic"
        : g_strcmp0(kind, "dialed") == 0 ? "call-outgoing-symbolic" : "call-incoming-symbolic";
    GtkWidget *image = gtk_image_new_from_icon_name(icon);
    gtk_accessible_update_property(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_PROPERTY_LABEL, recent_kind_text(kind), -1);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
    if (g_strcmp0(kind, "missed") == 0) gtk_widget_add_css_class(row, "missed");
    if (*number) {
        GtkWidget *call = gtk_button_new_from_icon_name("call-start-symbolic");
        char *accessible = g_strdup_printf("Call back %s", recent_title(name, number));
        gtk_accessible_update_property(GTK_ACCESSIBLE(call), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
        gtk_widget_set_tooltip_text(call, "Call back");
        gtk_widget_add_css_class(call, "answer");
        gtk_widget_add_css_class(call, "circular");
        gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
        g_object_set_data_full(G_OBJECT(call), "number", g_strdup(number), g_free);
        g_signal_connect(call, "clicked", G_CALLBACK(on_contact_call), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), call);
        g_free(accessible);
    }
    g_free(subtitle);
    g_free(when);
    return row;
}

static void on_recents(GObject *source, GAsyncResult *result, gpointer data)
{
    View *self = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (!self->recents) {
    } else if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(sa(sssx))"))) {
        set_text(self->recents_status, "Recent calls unavailable");
    } else {
        const char *status, *name, *number, *kind;
        gint64 started;
        GVariantIter *entries;
        GDateTime *now = g_date_time_new_now_local();
        g_variant_get(reply, "(&sa(sssx))", &status, &entries);
        set_text(self->recents_status, status);
        gtk_list_box_remove_all(GTK_LIST_BOX(self->recents));
        while (g_variant_iter_next(entries, "(&s&s&sx)", &name, &number, &kind, &started))
            gtk_list_box_append(GTK_LIST_BOX(self->recents), recent_row(self, name, number, kind, started, now));
        g_variant_iter_free(entries);
        g_date_time_unref(now);
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
}

static void load_recents(View *self)
{
    if (self->popup || !self->recents || !self->daemon || !g_dbus_proxy_get_name_owner(self->daemon)) return;
    g_dbus_proxy_call(self->daemon, "Recents", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      10000, NULL, on_recents, self);
}

static void on_changed(GDBusProxy *proxy, const char *sender, const char *signal,
                       GVariant *parameters, gpointer data)
{
    (void)proxy;
    (void)sender;
    (void)parameters;
    if (g_strcmp0(signal, "Changed") == 0) refresh(data);
    if (g_strcmp0(signal, "ContactsChanged") == 0) load_contacts(data);
    if (g_strcmp0(signal, "RecentsChanged") == 0) load_recents(data);
}

static void on_owner(GObject *proxy, GParamSpec *property, gpointer data)
{
    (void)proxy;
    (void)property;
    refresh(data);
    load_contacts(data);
    load_recents(data);
}

static void on_proxy(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    View *self = data;
    GError *error = NULL;
    self->daemon = g_dbus_proxy_new_for_bus_finish(result, &error);
    g_clear_error(&error);
    if (!self->daemon) {
        show_unavailable(self);
        return;
    }
    g_signal_connect(self->daemon, "g-signal", G_CALLBACK(on_changed), self);
    g_signal_connect(self->daemon, "notify::g-name-owner", G_CALLBACK(on_owner), self);
    refresh(self);
    load_contacts(self);
    load_recents(self);
}

static GtkWidget *label(const char *text, const char *style)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_wrap(GTK_LABEL(widget), TRUE);
    if (style) gtk_widget_add_css_class(widget, style);
    return widget;
}

static GtkWidget *track(GtkWidget *widget, GtkWidget **slot)
{
    *slot = widget;
    g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)slot);
    return widget;
}

static GtkWidget *detail_row(const char *heading, GtkWidget **value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(row), label(heading, "dim-label"));
    gtk_box_append(GTK_BOX(row), track(label(NULL, "title-3"), value));
    return row;
}

static GtkWidget *action_button(const char *text, const char *style, const char *method)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(button, style);
    gtk_widget_add_css_class(button, "pill");
    gtk_widget_set_hexpand(button, TRUE);
    g_signal_connect(button, "clicked", G_CALLBACK(on_call_action), (gpointer)method);
    return button;
}

static GtkWidget *call_actions(View *self)
{
    GtkWidget *row = track(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), &self->actions);
    gtk_box_append(GTK_BOX(row), track(action_button("Decline", "destructive-action", "Decline"), &self->decline));
    gtk_box_append(GTK_BOX(row), track(action_button("Answer", "answer", "Answer"), &self->answer));
    gtk_box_append(GTK_BOX(row), track(action_button("Hang up", "destructive-action", "HangUp"), &self->hang_up));
    return row;
}

static GtkWidget *dial_row(View *self)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = track(gtk_entry_new(), &self->entry);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Enter number");
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PHONE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Phone number", -1);
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), self);
    g_signal_connect(entry, "activate", G_CALLBACK(on_dial), self);
    GtkWidget *call = track(gtk_button_new_with_label("Call"), &self->call);
    gtk_widget_add_css_class(call, "answer");
    g_signal_connect(call, "clicked", G_CALLBACK(on_dial), self);
    gtk_box_append(GTK_BOX(row), entry);
    gtk_box_append(GTK_BOX(row), call);
    return row;
}

static void load_style(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.answer { background: @success_bg_color; color: @success_fg_color; }"
        "row.missed .title { color: @error_color; }"
        "button.keypad { min-width: 72px; min-height: 56px; font-size: 1.4em; border-radius: 28px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static GtkWidget *build_keypad(View *self)
{
    static const char *keys[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_box_append(GTK_BOX(page), dial_row(self));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    for (int i = 0; i < 12; i++) {
        GtkWidget *key = gtk_button_new_with_label(keys[i]);
        gtk_widget_add_css_class(key, "keypad");
        g_signal_connect(key, "clicked", G_CALLBACK(on_key), self);
        gtk_grid_attach(GTK_GRID(grid), key, i % 3, i / 3, 1, 1);
    }
    GtkWidget *plus = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(plus, "keypad");
    g_signal_connect(plus, "clicked", G_CALLBACK(on_key), self);
    gtk_grid_attach(GTK_GRID(grid), plus, 0, 4, 1, 1);
    GtkWidget *back = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_add_css_class(back, "keypad");
    gtk_widget_add_css_class(back, "flat");
    gtk_accessible_update_property(GTK_ACCESSIBLE(back), GTK_ACCESSIBLE_PROPERTY_LABEL, "Delete digit", -1);
    g_signal_connect(back, "clicked", G_CALLBACK(on_backspace), self);
    gtk_grid_attach(GTK_GRID(grid), back, 2, 4, 1, 1);
    gtk_box_append(GTK_BOX(page), grid);
    return page;
}

static GtkWidget *scrolled_list(GtkWidget *list, const char *empty_text)
{
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    GtkWidget *empty = label(empty_text, "dim-label");
    gtk_widget_set_margin_top(empty, 24);
    gtk_widget_set_margin_bottom(empty, 24);
    gtk_widget_set_margin_start(empty, 12);
    gtk_widget_set_margin_end(empty, 12);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(list), empty);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static GtkWidget *build_contacts(View *self)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *search = track(gtk_search_entry_new(), &self->search);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search contacts or numbers");
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), self);
    gtk_box_append(GTK_BOX(page), search);
    GtkWidget *sync_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *status = track(label("Not synced yet", "dim-label"), &self->sync_status);
    gtk_widget_set_hexpand(status, TRUE);
    gtk_box_append(GTK_BOX(sync_row), status);
    GtkWidget *sync = track(gtk_button_new_with_label("Sync now"), &self->sync);
    g_signal_connect(sync, "clicked", G_CALLBACK(on_sync), self);
    gtk_box_append(GTK_BOX(sync_row), sync);
    gtk_box_append(GTK_BOX(page), sync_row);
    GtkWidget *list = track(gtk_list_box_new(), &self->contacts);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), filter_contact, self, NULL);
    gtk_box_append(GTK_BOX(page), scrolled_list(list,
        "No contacts yet. On the iPhone, open Settings → Bluetooth → ⓘ next to this "
        "laptop, turn on Sync Contacts, then press Sync now."));
    return page;
}

static GtkWidget *build_recents(View *self)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *sync_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *status = track(label("Not synced yet", "dim-label"), &self->recents_status);
    gtk_widget_set_hexpand(status, TRUE);
    gtk_box_append(GTK_BOX(sync_row), status);
    GtkWidget *sync = gtk_button_new_with_label("Sync now");
    g_signal_connect(sync, "clicked", G_CALLBACK(on_sync), self);
    gtk_box_append(GTK_BOX(sync_row), sync);
    gtk_box_append(GTK_BOX(page), sync_row);
    gtk_box_append(GTK_BOX(page), scrolled_list(track(gtk_list_box_new(), &self->recents),
        "No recent calls yet. Calls from the iPhone appear here after syncing."));
    return page;
}

static GtkWidget *build_main(View *self)
{
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_box_set_homogeneous(GTK_BOX(details), TRUE);
    gtk_box_append(GTK_BOX(details), detail_row("Connection", &self->status));
    gtk_box_append(GTK_BOX(details), detail_row("Call", &self->phase));
    gtk_box_append(GTK_BOX(details), detail_row("Audio", &self->audio));
    gtk_box_append(GTK_BOX(body), details);
    gtk_box_append(GTK_BOX(body), track(label(NULL, "title-2"), &self->caller));
    gtk_box_append(GTK_BOX(body), call_actions(self));
    gtk_box_append(GTK_BOX(body), track(label(NULL, "error"), &self->error));
    GtkWidget *stack = track(adw_view_stack_new(), &self->stack);
    gtk_widget_set_vexpand(stack, TRUE);
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), build_recents(self), "recents", "Recents",
                                        "document-open-recent-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), build_keypad(self), "keypad", "Keypad",
                                        "input-dialpad-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), build_contacts(self), "contacts", "Contacts",
                                        "x-office-address-book-symbolic");
    gtk_box_append(GTK_BOX(body), stack);
    return body;
}

static GtkWidget *build_popup(View *self)
{
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(body), track(label(NULL, "dim-label"), &self->phase));
    gtk_box_append(GTK_BOX(body), track(label(NULL, "title-2"), &self->caller));
    gtk_box_append(GTK_BOX(body), call_actions(self));
    GtkWidget *ignore = track(gtk_button_new_with_label("Ignore"), &self->ignore);
    gtk_widget_add_css_class(ignore, "flat");
    g_signal_connect(ignore, "clicked", G_CALLBACK(on_ignore), self);
    gtk_box_append(GTK_BOX(body), ignore);
    gtk_box_append(GTK_BOX(body), track(label(NULL, "error"), &self->error));
    return body;
}

static void on_activate(GApplication *application, gpointer data)
{
    View *self = data;
    if (self->window) {
        if (self->popup) refresh(self); else gtk_window_present(GTK_WINDOW(self->window));
        return;
    }
    load_style();
    self->window = adw_application_window_new(GTK_APPLICATION(application));
    g_object_add_weak_pointer(G_OBJECT(self->window), (gpointer *)&self->window);
    gtk_window_set_title(GTK_WINDOW(self->window), self->popup ? "Incoming call" : "IBlueConnect");
    gtk_window_set_default_size(GTK_WINDOW(self->window), self->popup ? 360 : 500, self->popup ? 160 : 680);
    g_signal_connect(self->window, "close-request", G_CALLBACK(on_close_request), self);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *body = self->popup ? build_popup(self) : build_main(self);
    gtk_widget_set_margin_top(body, self->popup ? 12 : 24);
    gtk_widget_set_margin_bottom(body, 24);
    gtk_widget_set_margin_start(body, 24);
    gtk_widget_set_margin_end(body, 24);
    if (self->popup) {
        self->pinned = TRUE;
        gtk_window_set_deletable(GTK_WINDOW(self->window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(self->window), FALSE);
        g_signal_connect(self->window, "realize", G_CALLBACK(on_popup_realize), self);
        g_signal_connect(self->window, "map", G_CALLBACK(on_popup_map), self);
    }
    if (self->stack) {
        GtkWidget *switcher = adw_view_switcher_new();
        adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(self->stack));
        adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);
    }
    gtk_box_append(GTK_BOX(content), header);
    gtk_box_append(GTK_BOX(content), body);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self->window), content);
    /* The popup stays unmapped until the daemon confirms the call is still ringing. */
    if (self->popup) {
        self->state = g_strdup("calling unavailable");
        self->stage = g_strdup("incoming");
        update_controls(self);
    } else {
        show_unavailable(self);
        gtk_window_present(GTK_WINDOW(self->window));
    }
    if (self->daemon) {
        refresh(self);
        return;
    }
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        NULL, "org.iblueconnect.Daemon", "/org/iblueconnect/Daemon",
        "org.iblueconnect.Daemon", NULL, on_proxy, self);
}

int main(int argc, char **argv)
{
    /* The daemon launches `--incoming`; a separate app ID keeps it from opening the main window. */
    view.popup = argc > 1 && g_strcmp0(argv[1], "--incoming") == 0;
    AdwApplication *application = adw_application_new(
        view.popup ? "org.iblueconnect.IBlueConnect.Incoming" : "org.iblueconnect.IBlueConnect",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &view);
    int result = g_application_run(G_APPLICATION(application), 1, argv);
    g_clear_object(&view.daemon);
    g_object_unref(application);
    return result;
}
#else
int main(void)
{
    g_assert_cmpstr(status_text("calling unavailable"), ==, "Calling unavailable");
    g_assert_cmpstr(status_text("/private/path"), ==, "Calling unavailable");
    g_assert_cmpstr(phase_text("active"), ==, "Active call");
    g_assert_cmpstr(phase_text("/private/path"), ==, "No call details");
    g_assert_cmpstr(audio_text("laptop"), ==, "Laptop");
    g_assert_cmpstr(audio_text("/private/path"), ==, "Unknown");

    g_assert_cmpuint(actions_for("ready", "idle", FALSE), ==, ACT_DIAL);
    g_assert_cmpuint(actions_for("ready", "incoming", FALSE), ==, ACT_ANSWER | ACT_DECLINE | ACT_IGNORE);
    g_assert_cmpuint(actions_for("ready", "dialing", FALSE), ==, ACT_HANG_UP);
    g_assert_cmpuint(actions_for("ready", "alerting", FALSE), ==, ACT_HANG_UP);
    g_assert_cmpuint(actions_for("ready", "active", FALSE), ==, ACT_HANG_UP);
    g_assert_cmpuint(actions_for("ready", "ending", FALSE), ==, 0);
    g_assert_cmpuint(actions_for("ready", "incoming", TRUE), ==, 0);
    g_assert_cmpuint(actions_for("calling unavailable", "idle", FALSE), ==, 0);
    g_assert_cmpuint(actions_for("manage calls on phone", "incoming", FALSE), ==, 0);

    g_assert_true(dial_input_ok("+20 100 123"));
    g_assert_true(dial_input_ok("*#06#1"));
    g_assert_false(dial_input_ok(""));
    g_assert_false(dial_input_ok("+"));
    g_assert_false(dial_input_ok("12+3"));
    g_assert_false(dial_input_ok("12a"));

    char *digits = dial_digits("+20 100 123");
    g_assert_cmpstr(digits, ==, "+20100123");
    g_free(digits);

    g_assert_true(popup_visible("ready", "incoming", FALSE));
    g_assert_false(popup_visible("ready", "active", FALSE));
    g_assert_true(popup_visible("ready", "active", TRUE));
    g_assert_true(popup_visible("ready", "dialing", TRUE));
    g_assert_false(popup_visible("ready", "idle", TRUE));
    g_assert_false(popup_visible("ready", "ended", TRUE));
    g_assert_false(popup_visible("calling unavailable", "incoming", FALSE));

    g_assert_true(popup_pinned("incoming"));
    g_assert_false(popup_pinned("active"));
    g_assert_false(popup_pinned("dialing"));

    g_assert_true(keypad_sends_tone("ready", "active"));
    g_assert_false(keypad_sends_tone("ready", "idle"));
    g_assert_false(keypad_sends_tone("ready", "incoming"));
    g_assert_false(keypad_sends_tone("calling unavailable", "active"));

    g_assert_true(contact_matches("Émile Zola", "+20 100 555", ""));
    g_assert_true(contact_matches("Émile Zola", "+20 100 555", "émi"));
    g_assert_true(contact_matches("Émile Zola", "+20 100 555", "ZOLA"));
    g_assert_true(contact_matches("Émile Zola", "+20 100 555", "100 5"));
    g_assert_true(contact_matches("Émile Zola", "+20 100 555", "+20"));
    g_assert_false(contact_matches("Émile Zola", "+20 100 555", "hugo"));
    g_assert_false(contact_matches("Émile Zola", "+20 100 555", "999"));

    g_assert_cmpstr(recent_kind_text("missed"), ==, "Missed");
    g_assert_cmpstr(recent_kind_text("received"), ==, "Incoming");
    g_assert_cmpstr(recent_kind_text("dialed"), ==, "Outgoing");
    g_assert_cmpstr(recent_kind_text("declined"), ==, "Declined");
    g_assert_cmpstr(recent_kind_text("/private"), ==, "Call");

    g_assert_cmpstr(recent_title("Alice", "123"), ==, "Alice");
    g_assert_cmpstr(recent_title("", "123"), ==, "123");
    g_assert_cmpstr(recent_title("", ""), ==, "Unknown caller");

    GDateTime *now = g_date_time_new_local(2026, 9, 24, 15, 0, 0);
    struct { int day, hour, minute; const char *want; } whens[] = {
        { 24, 9, 5, "09:05" }, { 23, 22, 0, "Yesterday" }, { 20, 12, 0, "Sunday" }, { 10, 12, 0, "2026-09-10" },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(whens); i++) {
        GDateTime *then = g_date_time_new_local(2026, 9, whens[i].day, whens[i].hour, whens[i].minute, 0);
        char *text = recent_when(g_date_time_to_unix(then), now);
        g_assert_cmpstr(text, ==, whens[i].want);
        g_free(text);
        g_date_time_unref(then);
    }
    g_date_time_unref(now);
    return 0;
}
#endif
