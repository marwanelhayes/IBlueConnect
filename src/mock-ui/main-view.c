#include "main-view.h"
#include <string.h>

#define INK "alpha(@view_fg_color, 1.25)"
#define HAIRLINE "alpha(@view_fg_color, 0.13)"
#define LINK_BLUE "#2f6fff"

typedef struct {
    MockUiModel *model;
    MockMainCallback callback;
    gpointer user_data;
    GtkWidget *window, *screen, *pages, *device, *device_status;
    GtkWidget *device_calls, *device_contacts, *device_audio, *device_battery, *device_sync;
    GtkWidget *recents_box, *contacts_box, *detail, *contact_search;
    GtkWidget *home_search, *dial_entry, *dial_match, *dial_button, *siri_button;
    GtkWidget *split, *sync_error, *calls_error, *link, *tabs[3];
    int selected_contact;
    gboolean caller_card, card_from_recents; /* caller_card: not a contact, shown by selected_number alone. */
    char selected_number[64];
    size_t recents_limit;
    GtkWidget *appearance[3]; /* System, Light, Dark toggles (MockAppearance order). */
    gboolean syncing_appearance;
    char lists_key[1024]; /* ponytail: queries past ~1000 chars may skip a rebuild; fine for a search box. */
} MainView;

static GtkWidget *text_label(const char *text, const char *css)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    if (css) gtk_widget_add_css_class(label, css);
    return label;
}

static GtkWidget *vbox(int spacing)
{
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

static GtkWidget *hbox(int spacing)
{
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}

static void pad(GtkWidget *widget, int amount)
{
    gtk_widget_set_margin_top(widget, amount);
    gtk_widget_set_margin_bottom(widget, amount);
    gtk_widget_set_margin_start(widget, amount);
    gtk_widget_set_margin_end(widget, amount);
}

static void clear_box(GtkWidget *box)
{
    while (gtk_widget_get_first_child(box))
        gtk_box_remove(GTK_BOX(box), gtk_widget_get_first_child(box));
}

static GtkWidget *avatar(const char *name, int size)
{
    return adw_avatar_new(size, name, TRUE);
}

static GtkWidget *empty_state(const char *icon, const char *title, const char *description)
{
    GtkWidget *page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon);
    adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
    adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
    gtk_widget_set_vexpand(page, TRUE);
    return page;
}

static gboolean can_call(const MainView *self)
{
    return self->model->connection == MOCK_CONNECTION_CONNECTED && !self->model->calls_unavailable &&
           (!self->model->live || self->model->call_count == 0 || self->model->can_add_call);
}

/* Siri can start calls (including FaceTime Audio), so it needs a ready link and no call in progress. */
static gboolean can_ask_siri(const MainView *self)
{
    return self->model->connection == MOCK_CONNECTION_CONNECTED && !self->model->calls_unavailable &&
           self->model->call_count == 0 && self->model->siri == MOCK_SIRI_IDLE;
}

static void sync_siri(MainView *self)
{
    static const char *labels[] = { "Ask Siri", "Starting Siri…", "Listening — speak now" };
    AdwButtonContent *content = ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(self->siri_button)));
    adw_button_content_set_label(content, labels[self->model->siri]);
    gtk_widget_set_sensitive(self->siri_button, can_ask_siri(self));
}

static void emit(MainView *self, MockMainAction action, int index, const char *value)
{
    if (action == MOCK_MAIN_CALL && !can_call(self)) return;
    if (action == MOCK_MAIN_SIRI && !can_ask_siri(self)) return;
    if (self->callback) self->callback(action, index, value, self->user_data);
}

static void action_clicked(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    MockMainAction action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mock-action"));
    emit(self, action, -1, NULL);
}

static GtkWidget *action_button(const char *label, MockMainAction action, MainView *self)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "mock-action", GINT_TO_POINTER(action));
    g_signal_connect(button, "clicked", G_CALLBACK(action_clicked), self);
    return button;
}

static void call_contact(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "contact-index")) - 1;
    emit(self, MOCK_MAIN_CALL, index, g_object_get_data(G_OBJECT(button), "number"));
}

/* Calls number; index is the matching contact or -1. */
static GtkWidget *call_button(MainView *self, int index, const char *number, const char *label)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_object_set_data_full(G_OBJECT(button), "number", g_strdup(number), g_free);
    gtk_widget_set_tooltip_text(button, label);
    gtk_widget_set_sensitive(button, can_call(self));
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_object_set_data(G_OBJECT(button), "contact-index", GINT_TO_POINTER(index + 1));
    g_signal_connect(button, "clicked", G_CALLBACK(call_contact), self);
    return button;
}

static gboolean contains(const char *haystack, const char *query)
{
    if (!query || !*query) return TRUE;
    char *a = g_utf8_casefold(haystack, -1);
    char *b = g_utf8_casefold(query, -1);
    gboolean matched = strstr(a, b) != NULL;
    g_free(a);
    g_free(b);
    return matched;
}

static char *digits_only(const char *text)
{
    GString *digits = g_string_new(NULL);
    for (const char *p = text; *p; p++) if (g_ascii_isdigit(*p)) g_string_append_c(digits, *p);
    return g_string_free(digits, FALSE);
}

static gboolean number_contains(const char *number, const char *query)
{
    if (!number || !query || !*query) return FALSE;
    char *a = digits_only(number), *b = digits_only(query);
    gboolean match = *b && strstr(a, b) != NULL;
    g_free(a);
    g_free(b);
    return match;
}

static gboolean contact_matches(const MockContact *contact, const char *query)
{
    if (contains(contact->name, query)) return TRUE;
    for (size_t n = 0; n < MOCK_CONTACT_NUMBERS && contact->numbers[n]; n++)
        if (number_contains(contact->numbers[n], query)) return TRUE;
    return FALSE;
}

static gint contact_index_cmp(gconstpointer a, gconstpointer b)
{
    const size_t left = *(const size_t *)a, right = *(const size_t *)b;
    return g_utf8_collate(mock_contacts[left].name, mock_contacts[right].name);
}

static void solid_pill(GtkWidget *button)
{
    gtk_widget_add_css_class(button, "pill");
    gtk_widget_add_css_class(button, "solid-pill");
}

/* "Incoming, 12 min", or "Missed" when the call has no duration. */
static char *recent_kind(const MockRecent *recent)
{
    return recent->duration ? g_strdup_printf("%s, %s", recent->kind, recent->duration) : g_strdup(recent->kind);
}

/* A NULL number means the row has no Call button. */
static GtkWidget *person_row(MainView *self, const char *name, const char *subtitle, int index, const char *number)
{
    GtkWidget *row = hbox(12);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    pad(row, 6);
    GtkWidget *copy = vbox(2);
    gtk_widget_set_hexpand(copy, TRUE);
    GtkWidget *title = text_label(name, "person-name");
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(copy), title);
    gtk_box_append(GTK_BOX(copy), text_label(subtitle, "dim-label"));
    gtk_box_append(GTK_BOX(row), copy);
    if (number) gtk_box_append(GTK_BOX(row), call_button(self, index, number, "Call"));
    return row;
}

static GtkWidget *section(const char *title)
{
    GtkWidget *label = text_label(title, "section-title");
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 6);
    return label;
}

static void show_contact(GtkButton *button, gpointer user_data);

/* A thin line across the middle 90% of the list width (follows window size), like the iPhone's Recents. */
static void draw_separator(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)data;
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_rectangle(cr, width * 0.05, (height - 1) / 2, width * 0.9, 1);
    cairo_fill(cr);
}

static GtkWidget *recent_separator(void)
{
    GtkWidget *line = gtk_drawing_area_new();
    gtk_widget_add_css_class(line, "recent-separator");
    gtk_widget_set_hexpand(line, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(line), 1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(line), draw_separator, NULL, NULL);
    return line;
}

#define CONTACT_MARGIN 44

static void render_contacts(MainView *self)
{
    clear_box(self->contacts_box);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->contact_search));
    size_t found = 0;
    gunichar last_letter = 0;
    GArray *sorted = g_array_new(FALSE, FALSE, sizeof(size_t));
    for (size_t i = 0; i < mock_contacts_count; i++) g_array_append_val(sorted, i);
    g_array_sort(sorted, contact_index_cmp);
    for (size_t n = 0; n < sorted->len && !self->model->empty_contacts; n++) {
        size_t i = g_array_index(sorted, size_t, n);
        if (!contact_matches(&mock_contacts[i], query)) continue;
        gunichar first = g_unichar_toupper(g_utf8_get_char_validated(mock_contacts[i].name, -1));
        char letter[8] = {0};
        g_unichar_to_utf8(first, letter);
        /* One button and label per name, plus a margin letter per group: this list can hold ~1000 people. */
        GtkWidget *button = gtk_button_new();
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_add_css_class(button, "contact-row");
        GtkWidget *name = text_label(mock_contacts[i].name, "person-name");
        gtk_label_set_wrap(GTK_LABEL(name), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        if (last_letter != first) {
            if (last_letter) gtk_box_append(GTK_BOX(self->contacts_box), recent_separator());
            last_letter = first;
            GtkWidget *group = hbox(0);
            GtkWidget *margin = text_label(letter, "contact-letter");
            gtk_widget_set_size_request(margin, CONTACT_MARGIN, -1);
            gtk_box_append(GTK_BOX(group), margin);
            gtk_box_append(GTK_BOX(group), name);
            gtk_button_set_child(GTK_BUTTON(button), group);
        } else {
            gtk_widget_set_margin_start(name, CONTACT_MARGIN);
            gtk_button_set_child(GTK_BUTTON(button), name);
        }
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       mock_contacts[i].name, -1);
        g_object_set_data(G_OBJECT(button), "contact-index", GINT_TO_POINTER((int)i + 1));
        g_signal_connect(button, "clicked", G_CALLBACK(show_contact), self);
        gtk_box_append(GTK_BOX(self->contacts_box), button);
        found++;
    }
    g_array_free(sorted, TRUE);
    if (!found) gtk_box_append(GTK_BOX(self->contacts_box),
        empty_state("system-search-symbolic", *query && !self->model->empty_contacts ? "No Search Results" : "No Contacts",
                    *query && !self->model->empty_contacts ? "Try another name or number." : "Your iPhone contacts will appear here."));
}

static void search_changed(GtkEditable *editable, gpointer user_data)
{
    MainView *self = user_data;
    if (GTK_WIDGET(editable) == self->contact_search) render_contacts(self);
    else mock_main_window_update(self->window);
}

static void on_back_contact(MainView *self)
{
    if (self->card_from_recents) adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->pages), "recents");
    else adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), FALSE);
}

static GtkWidget *number_line(MainView *self, int index, const char *label, const char *number)
{
    GtkWidget *line = hbox(8);
    GtkWidget *text = vbox(2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(text), text_label(label ? label : "Phone", "dim-label"));
    gtk_box_append(GTK_BOX(text), text_label(number, "contact-number"));
    gtk_box_append(GTK_BOX(line), text);
    GtkWidget *dial = gtk_button_new_from_icon_name("call-start-symbolic");
    char *tip = g_strdup_printf("Call %s", number);
    gtk_widget_set_tooltip_text(dial, tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(dial), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
    g_free(tip);
    gtk_widget_add_css_class(dial, "flat");
    gtk_widget_set_valign(dial, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(dial, can_call(self));
    g_object_set_data(G_OBJECT(dial), "contact-index", GINT_TO_POINTER(index + 1));
    g_object_set_data_full(G_OBJECT(dial), "number", g_strdup(number), g_free);
    g_signal_connect(dial, "clicked", G_CALLBACK(call_contact), self);
    gtk_box_append(GTK_BOX(line), dial);
    return line;
}

/* A contact's calls by name, anyone else's by the number they called from. */
static void append_calls(MainView *self, const char *name, const char *number)
{
    gtk_box_append(GTK_BOX(self->detail), section("Recent calls"));
    size_t count = 0;
    for (size_t i = 0; i < mock_recents_count; i++) {
        const char *from = mock_recents[i].number ? mock_recents[i].number : "";
        if (name ? g_strcmp0(mock_recents[i].name, name) != 0 : g_strcmp0(from, number) != 0) continue;
        if (count) gtk_box_append(GTK_BOX(self->detail), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        char *kind = recent_kind(&mock_recents[i]);
        GtkWidget *line = hbox(8);
        GtkWidget *what = text_label(kind, NULL);
        gtk_widget_set_hexpand(what, TRUE);
        gtk_box_append(GTK_BOX(line), what);
        gtk_box_append(GTK_BOX(line), text_label(mock_recents[i].time, "dim-label"));
        gtk_box_append(GTK_BOX(self->detail), line);
        g_free(kind);
        count++;
    }
    if (!count) gtk_box_append(GTK_BOX(self->detail), text_label("No recent calls", "dim-label"));
}

static const char *caller_title(const char *number)
{
    return number && *number ? number : "Unknown caller";
}

static void render_detail(MainView *self)
{
    clear_box(self->detail);
    GtkWidget *back = gtk_button_new();
    GtkWidget *back_content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(back_content), "go-previous-symbolic");
    const char *back_tip = self->card_from_recents ? "Back to recents" : "Back to contacts";
    adw_button_content_set_label(ADW_BUTTON_CONTENT(back_content), self->card_from_recents ? "Recents" : "Contacts");
    gtk_button_set_child(GTK_BUTTON(back), back_content);
    gtk_widget_add_css_class(back, "flat");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(back, back_tip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(back), GTK_ACCESSIBLE_PROPERTY_LABEL, back_tip, -1);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(on_back_contact), self);
    gtk_box_append(GTK_BOX(self->detail), back);
    if (self->caller_card) {
        const char *number = self->selected_number;
        gtk_box_append(GTK_BOX(self->detail), text_label(caller_title(number), "contact-name"));
        if (*number) { /* A withheld number has nothing to call. */
            GtkWidget *call = call_button(self, -1, number, "Call");
            solid_pill(call);
            gtk_widget_set_halign(call, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(self->detail), call);
            gtk_box_append(GTK_BOX(self->detail), section("Phone numbers"));
            gtk_box_append(GTK_BOX(self->detail), number_line(self, -1, NULL, number));
        }
        append_calls(self, NULL, number);
        return;
    }
    if (self->selected_contact < 0 || (size_t)self->selected_contact >= mock_contacts_count) {
        gtk_box_append(GTK_BOX(self->detail), empty_state("avatar-default-symbolic",
            "Select a Contact", "Choose someone to see their numbers and recent calls."));
        return;
    }
    const MockContact *contact = &mock_contacts[self->selected_contact];
    gtk_box_append(GTK_BOX(self->detail), text_label(contact->name, "contact-name"));
    GtkWidget *call = call_button(self, self->selected_contact, contact->numbers[0], "Call");
    solid_pill(call);
    gtk_widget_set_halign(call, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->detail), call);
    gtk_box_append(GTK_BOX(self->detail), section("Phone numbers"));
    for (size_t n = 0; n < MOCK_CONTACT_NUMBERS && contact->numbers[n]; n++) {
        if (n) gtk_box_append(GTK_BOX(self->detail), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(self->detail), number_line(self, self->selected_contact, contact->labels[n], contact->numbers[n]));
    }
    append_calls(self, contact->name, NULL);
}

static void show_contact(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    self->selected_contact = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "contact-index")) - 1;
    render_detail(self);
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), TRUE);
}

/* The "!" on a Recents row: the caller's card, like a phone's info button. It never calls. */
static void show_recent_info(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "contact-index")) - 1;
    self->selected_contact = index;
    self->caller_card = index < 0;
    g_strlcpy(self->selected_number, g_object_get_data(G_OBJECT(button), "number"), sizeof self->selected_number);
    self->card_from_recents = TRUE;
    render_detail(self);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->pages), "contacts");
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), TRUE);
}

static void call_typed_number(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    emit(self, MOCK_MAIN_CALL, -1, gtk_editable_get_text(GTK_EDITABLE(self->home_search)));
    (void)button;
}

static gboolean looks_like_number(const char *text)
{
    gboolean digit = FALSE;
    for (const char *p = text; *p; p++) {
        if (g_ascii_isdigit(*p)) digit = TRUE;
        else if (!strchr(" +-()*#", *p)) return FALSE;
    }
    return digit;
}

/* The whole row calls the person back; there is no separate Call button. */
static GtkWidget *recent_row(MainView *self, const MockRecent *recent)
{
    const char *name = recent->name ? recent->name : caller_title(recent->number);
    const char *number = recent->number ? recent->number : "";
    int contact = mock_ui_contact_named(recent->name);
    char *kind = recent_kind(recent);
    gboolean missed = g_ascii_strcasecmp(recent->kind, "Missed") == 0;
    GtkWidget *row = person_row(self, name, kind, -1, NULL);
    g_free(kind);
    GtkWidget *copy = gtk_widget_get_first_child(row);
    if (missed)
        for (GtkWidget *l = gtk_widget_get_first_child(copy); l; l = gtk_widget_get_next_sibling(l)) {
            gtk_widget_remove_css_class(l, "dim-label");
            gtk_widget_add_css_class(l, "error");
        }
    gtk_box_append(GTK_BOX(row), text_label(recent->time, "dim-label"));
    GtkWidget *button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), row);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "recent-row");
    char *label = g_strdup_printf("Call back %s", name);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_free(label);
    g_object_set_data(G_OBJECT(button), "contact-index", GINT_TO_POINTER(contact + 1));
    g_object_set_data_full(G_OBJECT(button), "number", g_strdup(number), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(call_contact), self);
    gtk_widget_set_sensitive(button, *number != '\0'); /* A withheld number cannot be called back. */
    gtk_widget_set_hexpand(button, TRUE);
    GtkWidget *info = gtk_button_new_with_label("!");
    gtk_widget_set_name(info, "recent-info");
    gtk_widget_add_css_class(info, "flat");
    gtk_widget_add_css_class(info, "recent-info");
    gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
    char *details = g_strdup_printf("Details for %s", name);
    gtk_widget_set_tooltip_text(info, details);
    gtk_accessible_update_property(GTK_ACCESSIBLE(info), GTK_ACCESSIBLE_PROPERTY_LABEL, details, -1);
    g_free(details);
    g_object_set_data(G_OBJECT(info), "contact-index", GINT_TO_POINTER(contact + 1));
    g_object_set_data_full(G_OBJECT(info), "number", g_strdup(number), g_free);
    g_signal_connect(info, "clicked", G_CALLBACK(show_recent_info), self);
    GtkWidget *line = hbox(4);
    gtk_box_append(GTK_BOX(line), button);
    gtk_box_append(GTK_BOX(line), info);
    return line;
}

static void show_more_recents(MainView *self);

static void render_recents(MainView *self)
{
    clear_box(self->recents_box);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->home_search));
    if (!*query) {
        gtk_box_append(GTK_BOX(self->recents_box), section("Favorites"));
        GtkWidget *favorites = hbox(8);
        for (size_t f = 0; f < mock_favorites_count && !self->model->empty_favorites; f++) {
            size_t i = mock_favorites[f];
            if (i >= mock_contacts_count) continue;
            GtkWidget *button = gtk_button_new();
            GtkWidget *body = vbox(4);
            gtk_widget_set_halign(body, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(body), avatar(mock_contacts[i].name, 48));
            const char *full = mock_contacts[i].name, *space = strchr(full, ' ');
            char *first_name = space ? g_strndup(full, space - full) : g_strdup(full);
            GtkWidget *name = gtk_label_new(first_name);
            g_free(first_name);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(body), name);
            gtk_button_set_child(GTK_BUTTON(button), body);
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_hexpand(button, TRUE);
            gtk_widget_set_sensitive(button, can_call(self));
            gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                mock_contacts[i].name, -1);
            g_object_set_data(G_OBJECT(button), "contact-index", GINT_TO_POINTER((int)i + 1));
            g_object_set_data_full(G_OBJECT(button), "number", g_strdup(mock_contacts[i].numbers[0]), g_free);
            g_signal_connect(button, "clicked", G_CALLBACK(call_contact), self);
            gtk_box_append(GTK_BOX(favorites), button);
        }
        if (self->model->empty_favorites || !mock_favorites_count)
            gtk_box_append(GTK_BOX(favorites), text_label("No Favorites", "dim-label"));
        gtk_box_append(GTK_BOX(self->recents_box), favorites);
    }
    gtk_box_append(GTK_BOX(self->recents_box), section(*query ? "Search Results" : "Recent calls"));
    size_t found = 0;
    if (looks_like_number(query)) {
        char *label = g_strdup_printf("Call %s", query);
        GtkWidget *call = gtk_button_new_with_label(label);
        solid_pill(call);
        gtk_widget_set_sensitive(call, can_call(self));
        g_signal_connect(call, "clicked", G_CALLBACK(call_typed_number), self);
        gtk_box_append(GTK_BOX(self->recents_box), call);
        g_free(label);
        found++;
    }
    if (*query) {
        for (size_t i = 0; i < mock_contacts_count && !self->model->empty_contacts; i++) {
            if (!contact_matches(&mock_contacts[i], query)) continue;
            gtk_box_append(GTK_BOX(self->recents_box), person_row(self, mock_contacts[i].name, mock_contacts[i].numbers[0], (int)i, mock_contacts[i].numbers[0]));
            found++;
        }
    } else {
        for (size_t i = 0; i < mock_recents_count && i < self->recents_limit && !self->model->empty_recents; i++) {
            if (i) gtk_box_append(GTK_BOX(self->recents_box), recent_separator());
            gtk_box_append(GTK_BOX(self->recents_box), recent_row(self, &mock_recents[i]));
            found++;
        }
        if (!self->model->empty_recents && mock_recents_count > self->recents_limit) {
            GtkWidget *more = gtk_button_new_with_label("Show more");
            gtk_widget_add_css_class(more, "flat");
            gtk_widget_set_halign(more, GTK_ALIGN_CENTER);
            g_signal_connect_swapped(more, "clicked", G_CALLBACK(show_more_recents), self);
            gtk_box_append(GTK_BOX(self->recents_box), more);
        }
    }
    if (!found) gtk_box_append(GTK_BOX(self->recents_box),
        empty_state(*query ? "system-search-symbolic" : "view-list-symbolic",
            *query && !self->model->empty_contacts ? "No Search Results" : "No Recent Calls",
            *query && !self->model->empty_contacts ? "Try another name or number." : "Your recent iPhone calls will appear here."));
}

static void dial_clicked(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    const char *number = gtk_editable_get_text(GTK_EDITABLE(self->dial_entry));
    if (*number) emit(self, MOCK_MAIN_CALL, -1, number);
    (void)button;
}

static void digit_clicked(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    const char *key = g_object_get_data(G_OBJECT(button), "digit");
    const char *old = gtk_editable_get_text(GTK_EDITABLE(self->dial_entry));
    char *new_value = g_strconcat(old, key, NULL);
    gtk_editable_set_text(GTK_EDITABLE(self->dial_entry), new_value);
    g_free(new_value);
}

static void backspace_clicked(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    const char *old = gtk_editable_get_text(GTK_EDITABLE(self->dial_entry));
    gsize len = strlen(old);
    if (len) {
        char *next = g_strndup(old, len - 1);
        gtk_editable_set_text(GTK_EDITABLE(self->dial_entry), next);
        g_free(next);
    }
    (void)button;
}

static void dial_changed(GtkEditable *editable, gpointer user_data)
{
    MainView *self = user_data;
    const char *value = gtk_editable_get_text(editable);
    gtk_widget_set_sensitive(self->dial_button, *value != '\0' && can_call(self));
    const char *match = "";
    for (size_t i = 0; *value && !*match && i < mock_contacts_count; i++)
        for (size_t n = 0; n < MOCK_CONTACT_NUMBERS && mock_contacts[i].numbers[n]; n++)
            if (number_contains(mock_contacts[i].numbers[n], value)) { match = mock_contacts[i].name; break; }
    gtk_label_set_text(GTK_LABEL(self->dial_match), match);
}

static GtkWidget *dial_key(MainView *self, const char *digit, const char *letters)
{
    GtkWidget *key = gtk_button_new();
    char *name = g_strdup_printf("dial-key-%s", digit);
    gtk_widget_set_name(key, name);
    g_free(name);
    gtk_widget_add_css_class(key, "dial-key");
    gtk_widget_set_halign(key, GTK_ALIGN_CENTER);
    GtkWidget *content = vbox(0);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    GtkWidget *number = gtk_label_new(digit);
    gtk_widget_add_css_class(number, "key-digit");
    gtk_box_append(GTK_BOX(content), number);
    GtkWidget *under = gtk_label_new(letters);
    gtk_widget_add_css_class(under, "key-letters");
    gtk_box_append(GTK_BOX(content), under);
    gtk_button_set_child(GTK_BUTTON(key), content);
    gtk_accessible_update_property(GTK_ACCESSIBLE(key), GTK_ACCESSIBLE_PROPERTY_LABEL, digit, -1);
    g_object_set_data(G_OBJECT(key), "digit", (gpointer)digit);
    g_signal_connect(key, "clicked", G_CALLBACK(digit_clicked), self);
    return key;
}

static GtkWidget *build_dial(MainView *self)
{
    GtkWidget *page = vbox(0);
    gtk_widget_set_margin_start(page, 44);
    gtk_widget_set_margin_end(page, 44);
    gtk_widget_set_valign(page, GTK_ALIGN_CENTER);
    self->dial_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->dial_entry), "Enter a number");
    gtk_entry_set_alignment(GTK_ENTRY(self->dial_entry), 0.5);
    gtk_widget_add_css_class(self->dial_entry, "dial-number");
    gtk_entry_set_input_purpose(GTK_ENTRY(self->dial_entry), GTK_INPUT_PURPOSE_PHONE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->dial_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Phone number", -1);
    gtk_widget_set_margin_start(self->dial_entry, 36);
    gtk_widget_set_margin_end(self->dial_entry, 36);
    /* Beside the number but outside its centring, so the digits line up with the keys. */
    GtkWidget *number = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(number), self->dial_entry);
    GtkWidget *delete = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_add_css_class(delete, "flat");
    gtk_widget_set_tooltip_text(delete, "Delete last digit");
    gtk_accessible_update_property(GTK_ACCESSIBLE(delete), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Delete last digit", -1);
    gtk_widget_set_halign(delete, GTK_ALIGN_END);
    gtk_widget_set_valign(delete, GTK_ALIGN_CENTER);
    g_signal_connect(delete, "clicked", G_CALLBACK(backspace_clicked), self);
    gtk_overlay_add_overlay(GTK_OVERLAY(number), delete);
    gtk_box_append(GTK_BOX(page), number);
    self->dial_match = text_label("", "dim-label");
    gtk_widget_add_css_class(self->dial_match, "dial-match");
    gtk_label_set_xalign(GTK_LABEL(self->dial_match), 0.5);
    gtk_box_append(GTK_BOX(page), self->dial_match);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    static const char *digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    static const char *letters[] = { "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", "" };
    for (int i = 0; i < 12; i++)
        gtk_grid_attach(GTK_GRID(grid), dial_key(self, digits[i], letters[i]), i % 3, i / 3, 1, 1);
    gtk_box_append(GTK_BOX(page), grid);
    self->dial_button = gtk_button_new_with_label("Call");
    solid_pill(self->dial_button);
    gtk_widget_add_css_class(self->dial_button, "dial-call");
    gtk_widget_set_halign(self->dial_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(self->dial_button, 8);
    gtk_widget_set_sensitive(self->dial_button, FALSE);
    g_signal_connect(self->dial_button, "clicked", G_CALLBACK(dial_clicked), self);
    g_signal_connect(self->dial_entry, "activate", G_CALLBACK(dial_clicked), self);
    gtk_box_append(GTK_BOX(page), self->dial_button);
    g_signal_connect(self->dial_entry, "changed", G_CALLBACK(dial_changed), self);
    return page;
}

static GtkWidget *build_contacts(MainView *self)
{
    self->split = adw_navigation_split_view_new();
    adw_navigation_split_view_set_min_sidebar_width(ADW_NAVIGATION_SPLIT_VIEW(self->split), 240);
    GtkWidget *sidebar = vbox(8);
    pad(sidebar, 12);
    self->contact_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->contact_search), "Search contacts");
    gtk_widget_add_css_class(self->contact_search, "comfy-search");
    gtk_box_append(GTK_BOX(sidebar), self->contact_search);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    self->contacts_box = vbox(2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->contacts_box);
    gtk_box_append(GTK_BOX(sidebar), scroll);
    GtkWidget *detail_scroll = gtk_scrolled_window_new();
    self->detail = vbox(12);
    pad(self->detail, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scroll), self->detail);
    adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(self->split),
        adw_navigation_page_new(sidebar, "Contacts"));
    adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(self->split),
        adw_navigation_page_new(detail_scroll, "Contact"));
    GtkWidget *breakpoints = adw_breakpoint_bin_new();
    gtk_widget_set_size_request(breakpoints, 280, 200);
    adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(breakpoints), self->split);
    AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 650px"));
    GValue collapsed = G_VALUE_INIT;
    g_value_init(&collapsed, G_TYPE_BOOLEAN);
    g_value_set_boolean(&collapsed, TRUE);
    adw_breakpoint_add_setter(narrow, G_OBJECT(self->split), "collapsed", &collapsed);
    g_value_unset(&collapsed);
    adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(breakpoints), narrow);
    g_signal_connect(self->contact_search, "search-changed", G_CALLBACK(search_changed), self);
    render_contacts(self);
    render_detail(self);
    return breakpoints;
}

static GtkWidget *build_recents(MainView *self)
{
    GtkWidget *page = vbox(8);
    pad(page, 16);
    self->home_search = gtk_search_entry_new();
    gtk_widget_add_css_class(self->home_search, "comfy-search");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->home_search),
                                          "Search contacts or enter a number");
    gtk_box_append(GTK_BOX(page), self->home_search);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    self->recents_box = vbox(4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->recents_box);
    gtk_box_append(GTK_BOX(page), scroll);
    g_signal_connect(self->home_search, "search-changed", G_CALLBACK(search_changed), self);
    render_recents(self);
    return page;
}

static void on_retry_banner(MainView *self)
{
    emit(self, MOCK_MAIN_RETRY, -1, NULL);
}

static void on_sync_banner(MainView *self)
{
    emit(self, MOCK_MAIN_SYNC, -1, NULL);
}

/* Siri has its own outlined pill in its own row under the link, on every tab. */
static GtkWidget *siri_button_new(MainView *self)
{
    self->siri_button = gtk_button_new();
    GtkWidget *siri_content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(siri_content), "audio-input-microphone-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(siri_content), "Ask Siri");
    gtk_button_set_child(GTK_BUTTON(self->siri_button), siri_content);
    gtk_widget_set_name(self->siri_button, "siri-button");
    gtk_widget_set_tooltip_text(self->siri_button,
        "Talk to Siri on your iPhone through this laptop, for example \"FaceTime audio Mom\"");
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->siri_button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Siri", -1);
    g_object_set_data(G_OBJECT(self->siri_button), "mock-action", GINT_TO_POINTER(MOCK_MAIN_SIRI));
    g_signal_connect(self->siri_button, "clicked", G_CALLBACK(action_clicked), self);
    gtk_widget_add_css_class(self->siri_button, "pill");
    gtk_widget_add_css_class(self->siri_button, "siri-button");
    gtk_widget_set_halign(self->siri_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(self->siri_button, 6);
    gtk_widget_set_margin_bottom(self->siri_button, 2);
    return self->siri_button;
}

static void draw_glyph_box(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

/* Fine outlines in the text colour, centred in LINK_GLYPH so both sides of the wire weigh the same. */
#define LINK_GLYPH 46

static void draw_link_glyph(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer laptop)
{
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_set_line_width(cr, 1.25);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double x = (width - 46) / 2.0, y = (height - 36) / 2.0;
    if (laptop) {
        draw_glyph_box(cr, x + 8, y + 4.5, 30, 24, 3);
        cairo_stroke(cr);
        draw_glyph_box(cr, x + 2.5, y + 28.5, 41, 3.5, 1.5);
    } else {
        draw_glyph_box(cr, x + 14, y + 1.5, 18, 33, 4);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 20.5, y + 4.5);
        cairo_line_to(cr, x + 25.5, y + 4.5);
    }
    cairo_stroke(cr);
}

static GtkWidget *link_glyph(gboolean laptop)
{
    GtkWidget *glyph = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(glyph), LINK_GLYPH);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(glyph), 36);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(glyph), draw_link_glyph, GINT_TO_POINTER(laptop), NULL);
    return glyph;
}

static GtkWidget *link_wire(const char *name)
{
    GtkWidget *wire = hbox(0);
    gtk_widget_set_name(wire, name);
    gtk_widget_add_css_class(wire, "link-wire");
    gtk_widget_set_hexpand(wire, TRUE);
    gtk_widget_set_valign(wire, GTK_ALIGN_CENTER);
    return wire;
}

/* Blue while ready; otherwise the wire breaks at an X and turns red. */
static void link_set_ready(GtkWidget *link, gboolean ready, const char *caption)
{
    if (ready) gtk_widget_remove_css_class(link, "unavailable");
    else gtk_widget_add_css_class(link, "unavailable");
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(link), "link-x"), !ready);
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(link), "link-far"), !ready);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(link), "caption")), caption);
}

static GtkWidget *link_new(const char *name, const char *caption)
{
    GtkWidget *link = vbox(0);
    gtk_widget_set_name(link, name);
    gtk_widget_add_css_class(link, "link-row");
    GtkWidget *devices = hbox(8);
    gtk_box_append(GTK_BOX(devices), link_glyph(FALSE));
    gtk_box_append(GTK_BOX(devices), link_wire("link-wire"));
    GtkWidget *x = gtk_image_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_name(x, "link-x");
    gtk_accessible_update_property(GTK_ACCESSIBLE(x), GTK_ACCESSIBLE_PROPERTY_LABEL, "Not connected", -1);
    gtk_box_append(GTK_BOX(devices), x);
    GtkWidget *far = link_wire("link-wire-far");
    gtk_box_append(GTK_BOX(devices), far);
    gtk_box_append(GTK_BOX(devices), link_glyph(TRUE));
    gtk_box_append(GTK_BOX(link), devices);
    GtkWidget *label = text_label(caption, "link-caption");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_box_append(GTK_BOX(link), label);
    g_object_set_data(G_OBJECT(link), "link-x", x);
    g_object_set_data(G_OBJECT(link), "link-far", far);
    g_object_set_data(G_OBJECT(link), "caption", label);
    return link;
}

static void tab_toggled(GtkToggleButton *tab, gpointer user_data)
{
    MainView *self = user_data;
    if (gtk_toggle_button_get_active(tab))
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->pages),
                                              g_object_get_data(G_OBJECT(tab), "page"));
}

/* The other direction: Ctrl+D and friends change the page without touching a tab. */
static void page_changed(MainView *self)
{
    const char *page = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(self->pages));
    if (self->card_from_recents && g_strcmp0(page, "contacts") != 0) { /* Contacts opens on its list again. */
        self->card_from_recents = self->caller_card = FALSE;
        self->selected_contact = -1;
        render_detail(self);
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), FALSE);
    }
    for (guint i = 0; i < G_N_ELEMENTS(self->tabs); i++)
        if (g_strcmp0(g_object_get_data(G_OBJECT(self->tabs[i]), "page"), page) == 0)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tabs[i]), TRUE);
}

static GtkWidget *build_tabs(MainView *self)
{
    static const char *const labels[] = { "Recents", "Keypad", "Contacts" };
    static const char *const pages[] = { "recents", "dial", "contacts" };
    GtkWidget *bar = hbox(0);
    gtk_widget_add_css_class(bar, "tab-bar");
    gtk_box_set_homogeneous(GTK_BOX(bar), TRUE);
    for (guint i = 0; i < G_N_ELEMENTS(self->tabs); i++) {
        GtkWidget *tab = gtk_toggle_button_new();
        char *name = g_strdup_printf("tab-%s", pages[i]);
        gtk_widget_set_name(tab, name);
        g_free(name);
        gtk_widget_add_css_class(tab, "flat");
        gtk_widget_add_css_class(tab, "tab");
        GtkWidget *content = vbox(4);
        gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(content), gtk_label_new(labels[i]));
        GtkWidget *mark = hbox(0);
        gtk_widget_add_css_class(mark, "tab-mark");
        gtk_widget_set_halign(mark, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(content), mark);
        gtk_button_set_child(GTK_BUTTON(tab), content);
        g_object_set_data(G_OBJECT(tab), "page", (gpointer)pages[i]);
        if (i) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tab), GTK_TOGGLE_BUTTON(self->tabs[0]));
        g_signal_connect(tab, "toggled", G_CALLBACK(tab_toggled), self);
        self->tabs[i] = tab;
        gtk_box_append(GTK_BOX(bar), tab);
    }
    g_signal_connect_swapped(self->pages, "notify::visible-child-name", G_CALLBACK(page_changed), self);
    page_changed(self);
    return bar;
}

static GtkWidget *build_connected(MainView *self)
{
    GtkWidget *body = vbox(0);
    self->calls_error = adw_banner_new("Calls Unavailable — your iPhone is connected, but calling is currently unavailable.");
    adw_banner_set_button_label(ADW_BANNER(self->calls_error), "Retry");
    g_signal_connect_swapped(self->calls_error, "button-clicked", G_CALLBACK(on_retry_banner), self);
    gtk_box_append(GTK_BOX(body), self->calls_error);
    self->sync_error = adw_banner_new("Contacts Couldn't Be Updated — last successful sync: 14 minutes ago.");
    adw_banner_set_button_label(ADW_BANNER(self->sync_error), "Try Again");
    g_signal_connect_swapped(self->sync_error, "button-clicked", G_CALLBACK(on_sync_banner), self);
    gtk_box_append(GTK_BOX(body), self->sync_error);
    self->link = link_new("link-status", "Connected, calls ready");
    gtk_box_append(GTK_BOX(body), self->link);
    gtk_box_append(GTK_BOX(body), siri_button_new(self));
    self->pages = adw_view_stack_new();
#if ADW_CHECK_VERSION(1, 7, 0)
    adw_view_stack_set_enable_transitions(ADW_VIEW_STACK(self->pages), TRUE);
#endif
    gtk_widget_set_vexpand(self->pages, TRUE);
    adw_view_stack_add_named(ADW_VIEW_STACK(self->pages), build_recents(self), "recents");
    adw_view_stack_add_named(ADW_VIEW_STACK(self->pages), build_dial(self), "dial");
    adw_view_stack_add_named(ADW_VIEW_STACK(self->pages), build_contacts(self), "contacts");
    gtk_box_append(GTK_BOX(body), self->pages);
    gtk_box_append(GTK_BOX(body), build_tabs(self));
    return body;
}

static GtkWidget *status_screen(MainView *self, const char *title, const char *description,
                                const char *action, MockMainAction code, gboolean spinner)
{
    GtkWidget *box = vbox(12);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    pad(box, 24);
    if (spinner) {
        GtkWidget *busy = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(busy));
        gtk_widget_set_halign(busy, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), busy);
    }
    GtkWidget *heading = text_label(title, "status-title");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.5);
    gtk_label_set_justify(GTK_LABEL(heading), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), heading);
    GtkWidget *text = text_label(description, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(text), 0.5);
    gtk_label_set_justify(GTK_LABEL(text), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 42);
    gtk_box_append(GTK_BOX(box), text);
    if (action) {
        GtkWidget *button = action_button(action, code, self);
        solid_pill(button);
        gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), button);
    }
    return box;
}

static void preferences_clicked(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    GtkWidget *prefs = adw_preferences_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(prefs), GTK_WINDOW(self->window));
    gtk_window_set_title(GTK_WINDOW(prefs), "Preferences");
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *groups[3] = { adw_preferences_group_new(), adw_preferences_group_new(), adw_preferences_group_new() };
    const char *titles[3] = { "General", "Contacts", "Calls" };
    const char *rows[3][3] = {
        { "Start IBlueConnect automatically", "Reconnect automatically", "Show incoming-call notifications" },
        { "Sync contacts automatically", "Show contact photos", NULL },
        { "Show floating call controller", "Keep call controller visible during calls", "Show audio activity visualization" }
    };
    for (int i = 0; i < 3; i++) {
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(groups[i]), titles[i]);
        for (int j = 0; j < 3 && rows[i][j]; j++) {
            GtkWidget *row = adw_switch_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), rows[i][j]);
            adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(groups[i]), row);
        }
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(groups[i]));
    }
    GtkWidget *appearance = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(appearance), "Appearance");
    GtkWidget *system = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(system), "Follow system appearance");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(appearance), system);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(appearance));
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(prefs), ADW_PREFERENCES_PAGE(page));
    gtk_window_present(GTK_WINDOW(prefs));
    (void)button;
}

static const char *device_name(const MainView *self)
{
    return self->model->live ? "iPhone" : "Marwan's iPhone";
}

static void appearance_toggled(GtkToggleButton *button, gpointer user_data)
{
    MainView *self = user_data;
    if (self->syncing_appearance || !gtk_toggle_button_get_active(button)) return;
    emit(self, MOCK_MAIN_APPEARANCE, -1, g_object_get_data(G_OBJECT(button), "appearance"));
}

/* System / Light / Dark as one linked row of toggles (AdwToggleGroup needs libadwaita 1.7; 24.04 has 1.5). */
static GtkWidget *appearance_row(MainView *self)
{
    static const char *const labels[] = { "System", "Light", "Dark" };
    static const char *const values[] = { "system", "light", "dark" };
    GtkWidget *row = hbox(0);
    gtk_widget_add_css_class(row, "linked");
    gtk_widget_set_halign(row, GTK_ALIGN_FILL);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    for (int i = 0; i < 3; i++) {
        GtkWidget *toggle = gtk_toggle_button_new_with_label(labels[i]);
        char *name = g_strdup_printf("appearance-%s", values[i]);
        gtk_widget_set_name(toggle, name);
        g_free(name);
        g_object_set_data(G_OBJECT(toggle), "appearance", (gpointer)values[i]);
        if (i) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(toggle), GTK_TOGGLE_BUTTON(self->appearance[0]));
        g_signal_connect(toggle, "toggled", G_CALLBACK(appearance_toggled), self);
        self->appearance[i] = toggle;
        gtk_box_append(GTK_BOX(row), toggle);
    }
    return row;
}

static void sync_appearance(MainView *self)
{
    self->syncing_appearance = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->appearance[self->model->appearance]), TRUE);
    self->syncing_appearance = FALSE;
}

static GtkWidget *build_device_menu(MainView *self)
{
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = vbox(8);
    pad(box, 16);
    gtk_box_append(GTK_BOX(box), text_label(device_name(self), "title-3"));
    self->device_status = text_label("Connected", "dim-label");
    gtk_box_append(GTK_BOX(box), self->device_status);
    self->device_calls = text_label("Calls · Ready", NULL);
    self->device_contacts = text_label("Contacts · Synced", NULL);
    self->device_audio = text_label("Audio · Ready", NULL);
    gtk_box_append(GTK_BOX(box), self->device_calls);
    gtk_box_append(GTK_BOX(box), self->device_contacts);
    gtk_box_append(GTK_BOX(box), self->device_audio);
    self->device_battery = text_label("", NULL);
    gtk_box_append(GTK_BOX(box), self->device_battery);
    self->device_sync = text_label("", NULL);
    gtk_label_set_wrap(GTK_LABEL(self->device_sync), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(self->device_sync), 32);
    gtk_box_append(GTK_BOX(box), self->device_sync);
    gtk_box_append(GTK_BOX(box), text_label("Appearance", "heading"));
    gtk_box_append(GTK_BOX(box), appearance_row(self));
    sync_appearance(self);
    gtk_box_append(GTK_BOX(box), action_button("Sync Contacts", MOCK_MAIN_SYNC, self));
    GtkWidget *disconnect = action_button("Disconnect", MOCK_MAIN_DISCONNECT, self);
    gtk_widget_set_name(disconnect, "safe-disconnect");
    gtk_widget_set_tooltip_text(disconnect, "Disconnect only when your call is safe; this may be blocked until the call ends");
    gtk_box_append(GTK_BOX(box), disconnect);
    GtkWidget *resume = action_button("Resume call monitoring", MOCK_MAIN_RESUME, self);
    gtk_widget_set_name(resume, "resume-monitoring");
    gtk_widget_set_tooltip_text(resume, "Resume monitoring an already-connected phone; connect Bluetooth manually");
    gtk_widget_set_visible(resume, self->model->live);
    gtk_box_append(GTK_BOX(box), resume);
    GtkWidget *forget = action_button("Forget phone…", MOCK_MAIN_FORGET, self);
    gtk_widget_set_name(forget, "safe-forget");
    gtk_widget_set_tooltip_text(forget, "Remove app-owned phone data after confirmation; this may be blocked until the call ends");
    gtk_widget_set_visible(forget, self->model->live);
    gtk_box_append(GTK_BOX(box), forget);
    GtkWidget *quit = action_button("Quit IBlueConnect", MOCK_MAIN_QUIT, self);
    gtk_widget_set_name(quit, "safe-quit");
    gtk_widget_set_tooltip_text(quit, "Stop IBlueConnect safely; this may be blocked until the call ends. Closing the window leaves monitoring running");
    gtk_widget_set_visible(quit, self->model->live);
    gtk_box_append(GTK_BOX(box), quit);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    return popover;
}

static gboolean focus_search(GtkWidget *widget, GVariant *args, gpointer user_data)
{
    MainView *self = user_data;
    const char *name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(self->pages));
    if (g_strcmp0(name, "dial") == 0)
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->pages), "recents");
    gtk_widget_grab_focus(g_strcmp0(name, "contacts") == 0 ? self->contact_search : self->home_search);
    (void)widget;
    (void)args;
    return TRUE;
}

static gboolean focus_dial(GtkWidget *widget, GVariant *args, gpointer user_data)
{
    MainView *self = user_data;
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->pages), "dial");
    gtk_widget_grab_focus(self->dial_entry);
    (void)widget;
    (void)args;
    return TRUE;
}

static gboolean close_page(GtkWidget *widget, GVariant *args, gpointer user_data)
{
    MainView *self = user_data;
    if (gtk_menu_button_get_active(GTK_MENU_BUTTON(self->device)))
        gtk_menu_button_set_active(GTK_MENU_BUTTON(self->device), FALSE);
    else if (self->card_from_recents || adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(self->split)))
        on_back_contact(self);
    (void)widget;
    (void)args;
    return TRUE;
}

GtkWidget *mock_main_window_new(GtkApplication *app, MockUiModel *model,
                                MockMainCallback callback, gpointer user_data)
{
    static GtkCssProvider *css;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
            /* Ink is the theme's text colour at full strength: alpha() clamps, so 1.25 undoes its 80%. */
            "window { font-family: \"Hanken Grotesk\", sans-serif; }"
            "window.iblue-surface { background-color: @view_bg_color; color: " INK "; }"
            "window.iblue-surface headerbar { background-color: transparent; box-shadow: none; color: " INK "; }"
            "button.flat { font-weight: normal; }"
            "window.iblue-surface button { transition: background-color 150ms ease-out, color 150ms ease-out; }"
            "window.iblue-surface separator { background-color: " HAIRLINE "; }"
            "avatar { background-color: transparent; background-image: none; color: " INK ";"
            " box-shadow: inset 0 0 0 1px alpha(@view_fg_color, 0.3);"
            " font-family: \"Spectral\", serif; font-weight: 300; }"
            ".wordmark { font-family: \"Spectral\", serif; font-size: 15px; letter-spacing: 0.2px; }"
            ".link-row { margin: 2px 32px 0; }"
            ".link-wire { min-height: 2px; border-radius: 2px; color: " LINK_BLUE "; background-color: currentColor; }"
            ".link-row.unavailable .link-wire { color: @error_color; }"
            "#link-x { color: @error_color; }"
            ".link-caption { font-size: 12.5px; color: alpha(@view_fg_color, 0.6); }"
            ".link-row.unavailable .link-caption { color: @error_color; }"
            ".siri-button { min-width: 200px; min-height: 26px; font-size: 14.5px; font-weight: 500; background: none;"
            " box-shadow: inset 0 0 0 1px " INK "; color: " INK "; }"
            ".siri-button:hover { background-color: alpha(@view_fg_color, 0.07); }"
            ".siri-button:active { background-color: alpha(@view_fg_color, 0.14); }"
            ".solid-pill { min-height: 26px; padding-left: 28px; padding-right: 28px; font-size: 15px; font-weight: 500;"
            " background-color: " INK "; color: @view_bg_color; }"
            ".solid-pill:hover { background-color: alpha(@view_fg_color, 1.0); }"
            ".solid-pill:active { background-color: alpha(@view_fg_color, 0.7); }"
            ".tab-bar { border-top: 1px solid " HAIRLINE "; min-height: 53px; }"
            ".tab { border-radius: 0; font-size: 13.5px; color: alpha(@view_fg_color, 0.6); background: none; }"
            ".tab:hover { color: " INK "; background: none; }"
            ".tab:checked { color: " INK "; font-weight: 500; background: none; }"
            ".tab-mark { min-width: 18px; min-height: 2px; border-radius: 2px; }"
            ".tab:checked .tab-mark { background-color: " INK "; }"
            ".dial-number { font-family: \"Spectral\", serif; font-weight: 200; font-size: 26px; min-height: 46px;"
            " background: none; box-shadow: none; outline: none; }"
            ".dial-match { min-height: 18px; font-size: 12.5px; }"
            ".dial-call { min-width: 144px; }"
            ".dial-key { min-width: 84px; min-height: 56px; padding: 0; border-radius: 14px; background: none; box-shadow: none; }"
            ".dial-key:hover { background-color: alpha(@view_fg_color, 0.06); }"
            ".dial-key:active { background-color: alpha(@view_fg_color, 0.12); }"
            ".key-digit { font-family: \"Spectral\", serif; font-weight: 300; font-size: 27px; }"
            ".key-letters { margin-top: -5px; min-height: 11px; font-size: 8px; font-weight: 500; letter-spacing: 1.5px; color: alpha(@view_fg_color, 0.6); }"
            ".recent-info { min-width: 24px; min-height: 24px; padding: 0; border-radius: 999px; font-family: \"Spectral\", serif;"
            " font-size: 14px; box-shadow: inset 0 0 0 1px alpha(@view_fg_color, 0.3); }"
            ".comfy-search { min-height: 44px; font-size: 14px; border-radius: 12px; background-color: alpha(@view_fg_color, 0.05); box-shadow: none; }"
            ".section-title { font-size: 13px; font-weight: 500; }"
            ".person-name { font-family: \"Spectral\", serif; font-weight: 300; font-size: 18.5px; }"
            ".contact-row { padding: 8px 0; border-radius: 10px; }"
            ".contact-letter { font-family: \"Spectral\", serif; font-weight: 200; font-size: 24px; color: alpha(@view_fg_color, 0.6); }"
            ".contact-name { font-family: \"Spectral\", serif; font-weight: 200; font-size: 42px; }"
            ".contact-number { font-family: \"Spectral\", serif; font-weight: 300; font-size: 21px; }"
            ".status-title { font-family: \"Spectral\", serif; font-weight: 200; font-size: 34px; }"
            ".recent-separator { color: " HAIRLINE "; }"
            ".recent-row { padding: 0; }");
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    MainView *self = g_new0(MainView, 1);
    self->model = model;
    self->callback = callback;
    self->user_data = user_data;
    self->selected_contact = -1;
    self->recents_limit = 50;
    self->window = adw_application_window_new(app);
    g_object_set_data_full(G_OBJECT(self->window), "mock-main-view", self, g_free);
    gtk_window_set_title(GTK_WINDOW(self->window), "IBlueConnect");
    gtk_window_set_default_size(GTK_WINDOW(self->window), 480, 700);
    gtk_widget_add_css_class(self->window, "iblue-surface");
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_centering_policy(ADW_HEADER_BAR(header), ADW_CENTERING_POLICY_STRICT);
    GtkWidget *app_title = gtk_label_new("IBlueConnect");
    gtk_widget_add_css_class(app_title, "wordmark");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), app_title);
    self->device = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(self->device), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(self->device, "Phone menu");
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->device), GTK_ACCESSIBLE_PROPERTY_LABEL, "Phone menu", -1);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->device), build_device_menu(self));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->device);
    GtkWidget *menu = gtk_button_new_from_icon_name("preferences-system-symbolic");
    gtk_widget_set_tooltip_text(menu, "Preferences");
    gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Preferences", -1);
    g_signal_connect(menu, "clicked", G_CALLBACK(preferences_clicked), self);
    gtk_widget_set_visible(menu, !self->model->live); /* Preferences are not stored yet. */
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    self->screen = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->screen), build_connected(self), "connected");
    GtkWidget *lost = vbox(0);
    GtkWidget *lost_link = link_new("link-lost", "iPhone not connected");
    link_set_ready(lost_link, FALSE, "iPhone not connected");
    gtk_box_append(GTK_BOX(lost), lost_link);
    gtk_box_append(GTK_BOX(lost), status_screen(self,
        "Connect your iPhone", self->model->live
            ? "Connect your iPhone in Bluetooth settings to make and receive calls from your GNOME desktop."
            : "Connect your iPhone to make and receive calls from your GNOME desktop.\nPreviously connected: Marwan's iPhone",
        self->model->live ? "Open Bluetooth Settings" : "Find iPhone", MOCK_MAIN_CONNECT, FALSE));
    gtk_stack_add_named(GTK_STACK(self->screen), lost, "disconnected");
    gtk_stack_add_named(GTK_STACK(self->screen), status_screen(self,
        "Connecting to Marwan's iPhone…", "Preparing calls…\nLoading contacts…",
        NULL, MOCK_MAIN_CONNECT, TRUE), "connecting");
    gtk_stack_add_named(GTK_STACK(self->screen), status_screen(self,
        "Couldn't Connect", self->model->live ? "IBlueConnect couldn't reach its background service."
                                              : "IBlueConnect couldn't connect to Marwan's iPhone.",
        "Try Again", MOCK_MAIN_RETRY, FALSE), "error");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->screen);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self->window), toolbar);
    GtkShortcutController *keys = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(keys, GTK_SHORTCUT_SCOPE_GLOBAL);
    GtkShortcutAction *action = gtk_callback_action_new(focus_search, self, NULL);
    gtk_shortcut_controller_add_shortcut(keys, gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("<Control>f"), action));
    action = gtk_callback_action_new(focus_dial, self, NULL);
    gtk_shortcut_controller_add_shortcut(keys, gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("<Control>d"), action));
    action = gtk_callback_action_new(focus_dial, self, NULL);
    gtk_shortcut_controller_add_shortcut(keys, gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("<Control>n"), action));
    action = gtk_callback_action_new(close_page, self, NULL);
    gtk_shortcut_controller_add_shortcut(keys, gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("Escape"), action));
    gtk_widget_add_controller(self->window, GTK_EVENT_CONTROLLER(keys));
    mock_main_window_update(self->window);
    return self->window;
}

/* Rebuilding ~1000 rows is slow, so lists are rebuilt only when what they show changes. */
static void render_lists(MainView *self)
{
    char key[sizeof self->lists_key];
    g_snprintf(key, sizeof key, "%u|%d|%d|%d|%d|%d|%zu|%s", mock_data_version, can_call(self),
               self->model->empty_recents, self->model->empty_contacts, self->model->empty_favorites,
               self->selected_contact, self->recents_limit,
               gtk_editable_get_text(GTK_EDITABLE(self->home_search)));
    if (strcmp(key, self->lists_key) == 0) return;
    g_strlcpy(self->lists_key, key, sizeof self->lists_key);
    render_recents(self);
    render_contacts(self);
    render_detail(self);
}

static void show_more_recents(MainView *self)
{
    self->recents_limit += 50;
    render_lists(self);
}

void mock_main_window_update(GtkWidget *window)
{
    MainView *self = g_object_get_data(G_OBJECT(window), "mock-main-view");
    if (!self) return;
    const char *name = "connected";
    switch (self->model->connection) {
    case MOCK_CONNECTION_DISCONNECTED: name = "disconnected"; break;
    case MOCK_CONNECTION_CONNECTING: name = "connecting"; break;
    case MOCK_CONNECTION_ERROR: name = "error"; break;
    case MOCK_CONNECTION_CONNECTED: break;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(self->screen), name);
    gtk_label_set_text(GTK_LABEL(self->device_status),
        self->model->connection == MOCK_CONNECTION_CONNECTED ? "Connected" : "Disconnected");
    gtk_label_set_text(GTK_LABEL(self->device_calls),
        self->model->connection != MOCK_CONNECTION_CONNECTED || self->model->calls_unavailable ?
        "Calls · Unavailable" : "Calls · Ready");
    gtk_label_set_text(GTK_LABEL(self->device_contacts),
        self->model->contact_sync_error ? "Contacts · Update failed" : "Contacts · Synced");
    gtk_label_set_text(GTK_LABEL(self->device_audio),
        self->model->connection == MOCK_CONNECTION_CONNECTED ? "Audio · Ready" : "Audio · Unavailable");
    gtk_label_set_text(GTK_LABEL(self->device_battery), self->model->battery);
    gtk_label_set_text(GTK_LABEL(self->device_sync), self->model->sync_label);
    adw_banner_set_revealed(ADW_BANNER(self->calls_error), self->model->calls_unavailable);
    if (self->model->live) adw_banner_set_title(ADW_BANNER(self->sync_error), self->model->sync_label);
    adw_banner_set_revealed(ADW_BANNER(self->sync_error), self->model->contact_sync_error);
    gboolean ready = self->model->connection == MOCK_CONNECTION_CONNECTED && !self->model->calls_unavailable;
    link_set_ready(self->link, ready, ready ? "Connected, calls ready" : "Calls unavailable");
    render_lists(self);
    sync_appearance(self);
    gtk_button_set_label(GTK_BUTTON(self->dial_button), self->model->can_add_call ? "Add call" : "Call");
    sync_siri(self);
    dial_changed(GTK_EDITABLE(self->dial_entry), self);
}

void mock_main_window_show_dial(GtkWidget *window)
{
    MainView *self = g_object_get_data(G_OBJECT(window), "mock-main-view");
    if (self) focus_dial(window, NULL, self);
}
