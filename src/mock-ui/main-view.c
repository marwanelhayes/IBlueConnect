#include "main-view.h"
#include <string.h>

typedef struct {
    MockUiModel *model;
    MockMainCallback callback;
    gpointer user_data;
    GtkWidget *window, *screen, *pages, *device, *device_status;
    GtkWidget *device_calls, *device_contacts, *device_audio;
    GtkWidget *recents_box, *contacts_box, *detail, *contact_search;
    GtkWidget *home_search, *dial_entry, *dial_match, *dial_button;
    GtkWidget *split, *sync_error, *calls_error, *connection_card, *link_status;
    int selected_contact;
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
    return self->model->connection == MOCK_CONNECTION_CONNECTED && !self->model->calls_unavailable;
}

static void emit(MainView *self, MockMainAction action, int index, const char *value)
{
    if (action == MOCK_MAIN_CALL && !can_call(self)) return;
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
    emit(self, MOCK_MAIN_CALL, index, NULL);
}

static GtkWidget *call_button(MainView *self, int index, const char *label)
{
    GtkWidget *button = gtk_button_new_with_label(label);
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
    return contains(contact->name, query) || number_contains(contact->mobile, query) ||
           number_contains(contact->work, query);
}

static gint contact_index_cmp(gconstpointer a, gconstpointer b)
{
    const size_t left = *(const size_t *)a, right = *(const size_t *)b;
    return g_utf8_collate(mock_contacts[left].name, mock_contacts[right].name);
}

static GtkWidget *person_row(MainView *self, size_t index, const char *subtitle, gboolean clickable)
{
    const MockContact *contact = &mock_contacts[index];
    GtkWidget *row = hbox(12);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    pad(row, 6);
    gtk_box_append(GTK_BOX(row), avatar(contact->name, 40));
    GtkWidget *copy = vbox(2);
    gtk_widget_set_hexpand(copy, TRUE);
    gtk_box_append(GTK_BOX(copy), text_label(contact->name, "heading"));
    gtk_box_append(GTK_BOX(copy), text_label(subtitle, "dim-label"));
    gtk_box_append(GTK_BOX(row), copy);
    if (clickable) gtk_box_append(GTK_BOX(row), call_button(self, (int)index, "Call"));
    return row;
}

static GtkWidget *section(const char *title)
{
    GtkWidget *label = text_label(title, "title-3");
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 6);
    return label;
}

static void show_contact(GtkButton *button, gpointer user_data);

static void render_contacts(MainView *self)
{
    clear_box(self->contacts_box);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->contact_search));
    size_t found = 0;
    char last_letter = 0;
    GArray *sorted = g_array_new(FALSE, FALSE, sizeof(size_t));
    for (size_t i = 0; i < mock_contacts_count; i++) g_array_append_val(sorted, i);
    g_array_sort(sorted, contact_index_cmp);
    for (size_t n = 0; n < sorted->len && !self->model->empty_contacts; n++) {
        size_t i = g_array_index(sorted, size_t, n);
        if (!contact_matches(&mock_contacts[i], query)) continue;
        char letter[2] = { g_ascii_toupper(mock_contacts[i].name[0]), 0 };
        if (last_letter != letter[0]) {
            gtk_box_append(GTK_BOX(self->contacts_box), section(letter));
            last_letter = letter[0];
        }
        GtkWidget *button = gtk_button_new();
        gtk_widget_add_css_class(button, "flat");
        gtk_button_set_child(GTK_BUTTON(button), person_row(self, i, mock_contacts[i].mobile, FALSE));
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
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), FALSE);
}

static void render_detail(MainView *self)
{
    clear_box(self->detail);
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(back, "Back to contacts");
    gtk_accessible_update_property(GTK_ACCESSIBLE(back), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Back to contacts", -1);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(on_back_contact), self);
    gtk_box_append(GTK_BOX(self->detail), back);
    if (self->selected_contact < 0 || (size_t)self->selected_contact >= mock_contacts_count) {
        gtk_box_append(GTK_BOX(self->detail), empty_state("avatar-default-symbolic",
            "Select a Contact", "Choose someone to see their numbers and recent calls."));
        return;
    }
    const MockContact *contact = &mock_contacts[self->selected_contact];
    GtkWidget *top = vbox(12);
    gtk_widget_set_halign(top, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top), avatar(contact->name, 80));
    GtkWidget *name = text_label(contact->name, "title-1");
    gtk_label_set_xalign(GTK_LABEL(name), 0.5);
    gtk_box_append(GTK_BOX(top), name);
    GtkWidget *call = call_button(self, self->selected_contact, "Call");
    gtk_widget_add_css_class(call, "suggested-action");
    gtk_box_append(GTK_BOX(top), call);
    gtk_box_append(GTK_BOX(self->detail), top);
    gtk_box_append(GTK_BOX(self->detail), section("Phone Numbers"));
    gtk_box_append(GTK_BOX(self->detail), text_label("Mobile", "dim-label"));
    gtk_box_append(GTK_BOX(self->detail), text_label(contact->mobile, NULL));
    if (contact->work) {
        gtk_box_append(GTK_BOX(self->detail), text_label("Work", "dim-label"));
        gtk_box_append(GTK_BOX(self->detail), text_label(contact->work, NULL));
    }
    gtk_box_append(GTK_BOX(self->detail), section("Recent Calls"));
    size_t count = 0;
    for (size_t i = 0; i < mock_recents_count; i++) {
        if (mock_recents[i].contact_index != (size_t)self->selected_contact) continue;
        const char *duration = mock_recents[i].duration ? mock_recents[i].duration : "";
        char *line = g_strdup_printf("%s  ·  %s%s%s", mock_recents[i].time,
            mock_recents[i].kind, *duration ? "  ·  " : "", duration);
        gtk_box_append(GTK_BOX(self->detail), text_label(line, NULL));
        g_free(line);
        count++;
    }
    if (!count) gtk_box_append(GTK_BOX(self->detail), text_label("No recent calls", "dim-label"));
}

static void show_contact(GtkButton *button, gpointer user_data)
{
    MainView *self = user_data;
    self->selected_contact = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "contact-index")) - 1;
    render_detail(self);
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

static void render_recents(MainView *self)
{
    clear_box(self->recents_box);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->home_search));
    if (!*query) {
        gtk_box_append(GTK_BOX(self->recents_box), section("Favorites"));
        GtkWidget *favorites = hbox(8);
        for (size_t i = 0; i < mock_contacts_count && !self->model->empty_favorites; i++) {
            GtkWidget *button = gtk_button_new();
            GtkWidget *body = vbox(4);
            gtk_widget_set_halign(body, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(body), avatar(mock_contacts[i].name, 48));
            GtkWidget *name = gtk_label_new(mock_contacts[i].name);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(body), name);
            gtk_button_set_child(GTK_BUTTON(button), body);
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_hexpand(button, TRUE);
            gtk_widget_set_sensitive(button, can_call(self));
            gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                mock_contacts[i].name, -1);
            g_object_set_data(G_OBJECT(button), "contact-index", GINT_TO_POINTER((int)i + 1));
            g_signal_connect(button, "clicked", G_CALLBACK(call_contact), self);
            gtk_box_append(GTK_BOX(favorites), button);
        }
        if (self->model->empty_favorites)
            gtk_box_append(GTK_BOX(favorites), text_label("No Favorites", "dim-label"));
        gtk_box_append(GTK_BOX(self->recents_box), favorites);
    }
    gtk_box_append(GTK_BOX(self->recents_box), section(*query ? "Search Results" : "Recent Calls"));
    size_t found = 0;
    if (looks_like_number(query)) {
        char *label = g_strdup_printf("Call %s", query);
        GtkWidget *call = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(call, "suggested-action");
        gtk_widget_set_sensitive(call, can_call(self));
        g_signal_connect(call, "clicked", G_CALLBACK(call_typed_number), self);
        gtk_box_append(GTK_BOX(self->recents_box), call);
        g_free(label);
        found++;
    }
    if (*query) {
        for (size_t i = 0; i < mock_contacts_count && !self->model->empty_contacts; i++) {
            if (!contact_matches(&mock_contacts[i], query)) continue;
            gtk_box_append(GTK_BOX(self->recents_box), person_row(self, i, mock_contacts[i].mobile, TRUE));
            found++;
        }
    } else {
        for (size_t i = 0; i < mock_recents_count && !self->model->empty_recents; i++) {
            const MockRecent *recent = &mock_recents[i];
            if (recent->contact_index >= mock_contacts_count) continue;
            char *summary = recent->duration ? g_strdup_printf("%s · %s", recent->kind, recent->duration)
                                              : g_strdup(recent->kind);
            GtkWidget *row = person_row(self, recent->contact_index, summary, TRUE);
            g_free(summary);
            GtkWidget *when = text_label(recent->time, "dim-label");
            gtk_box_append(GTK_BOX(row), when);
            if (g_ascii_strcasecmp(recent->kind, "Missed") == 0) {
                GtkWidget *copy = gtk_widget_get_next_sibling(gtk_widget_get_first_child(row));
                for (GtkWidget *l = gtk_widget_get_first_child(copy); l; l = gtk_widget_get_next_sibling(l))
                    gtk_widget_add_css_class(l, "error");
            }
            gtk_box_append(GTK_BOX(self->recents_box), row);
            found++;
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
    if (*value) for (size_t i = 0; i < mock_contacts_count; i++)
        if (number_contains(mock_contacts[i].mobile, value)) { match = mock_contacts[i].name; break; }
    gtk_label_set_text(GTK_LABEL(self->dial_match), match);
}

static GtkWidget *build_dial(MainView *self)
{
    GtkWidget *page = vbox(12);
    pad(page, 20);
    self->dial_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->dial_entry), "Enter a number");
    gtk_entry_set_input_purpose(GTK_ENTRY(self->dial_entry), GTK_INPUT_PURPOSE_PHONE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->dial_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Phone number", -1);
    gtk_box_append(GTK_BOX(page), self->dial_entry);
    self->dial_match = text_label("", "dim-label");
    gtk_box_append(GTK_BOX(page), self->dial_match);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_vexpand(grid, TRUE);
    static const char *digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    for (int i = 0; i < 12; i++) {
        GtkWidget *key = gtk_button_new_with_label(digits[i]);
        gtk_widget_add_css_class(key, "title-3");
        gtk_widget_set_hexpand(key, TRUE);
        gtk_widget_set_vexpand(key, TRUE);
        gtk_widget_set_size_request(key, 72, 42);
        g_object_set_data(G_OBJECT(key), "digit", (gpointer)digits[i]);
        g_signal_connect(key, "clicked", G_CALLBACK(digit_clicked), self);
        gtk_grid_attach(GTK_GRID(grid), key, i % 3, i / 3, 1, 1);
    }
    gtk_box_append(GTK_BOX(page), grid);
    GtkWidget *actions = hbox(8);
    self->dial_button = gtk_button_new_with_label("Call");
    gtk_widget_add_css_class(self->dial_button, "suggested-action");
    gtk_widget_set_hexpand(self->dial_button, TRUE);
    gtk_widget_set_sensitive(self->dial_button, FALSE);
    g_signal_connect(self->dial_button, "clicked", G_CALLBACK(dial_clicked), self);
    g_signal_connect(self->dial_entry, "activate", G_CALLBACK(dial_clicked), self);
    gtk_box_append(GTK_BOX(actions), self->dial_button);
    GtkWidget *delete = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(delete, "Delete last digit");
    gtk_accessible_update_property(GTK_ACCESSIBLE(delete), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Delete last digit", -1);
    g_signal_connect(delete, "clicked", G_CALLBACK(backspace_clicked), self);
    gtk_box_append(GTK_BOX(actions), delete);
    gtk_box_append(GTK_BOX(page), actions);
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
    g_signal_connect(self->contact_search, "changed", G_CALLBACK(search_changed), self);
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
    g_signal_connect(self->home_search, "changed", G_CALLBACK(search_changed), self);
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
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600);
    gtk_widget_set_margin_start(clamp, 16);
    gtk_widget_set_margin_end(clamp, 16);
    gtk_widget_set_margin_top(clamp, 8);
    gtk_widget_set_margin_bottom(clamp, 4);
    self->connection_card = vbox(10);
    gtk_widget_add_css_class(self->connection_card, "card");
    gtk_widget_add_css_class(self->connection_card, "connection-card");
    GtkWidget *devices = hbox(16);
    gtk_widget_set_halign(devices, GTK_ALIGN_CENTER);
    GtkWidget *phone = gtk_image_new_from_icon_name("phone-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(phone), 48);
    gtk_box_append(GTK_BOX(devices), phone);
    self->link_status = text_label("━━━━━━", "accent");
    gtk_widget_add_css_class(self->link_status, "title-2");
    gtk_widget_set_valign(self->link_status, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(devices), self->link_status);
    GtkWidget *laptop = gtk_image_new_from_icon_name("computer-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(laptop), 48);
    gtk_box_append(GTK_BOX(devices), laptop);
    gtk_box_append(GTK_BOX(self->connection_card), devices);
    GtkWidget *caption = text_label("iPhone connected · Calls ready", "dim-label");
    gtk_label_set_xalign(GTK_LABEL(caption), 0.5);
    g_object_set_data(G_OBJECT(self->connection_card), "caption", caption);
    gtk_box_append(GTK_BOX(self->connection_card), caption);
    adw_clamp_set_child(ADW_CLAMP(clamp), self->connection_card);
    gtk_box_append(GTK_BOX(body), clamp);
    self->pages = adw_view_stack_new();
    gtk_widget_set_vexpand(self->pages, TRUE);
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->pages), build_recents(self), "recents", "Recents", "document-open-recent-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->pages), build_contacts(self), "contacts", "Contacts", "avatar-default-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->pages), build_dial(self), "dial", "Dial", "call-start-symbolic");
    gtk_box_append(GTK_BOX(body), self->pages);
    GtkWidget *switcher = adw_view_switcher_bar_new();
    adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(switcher), ADW_VIEW_STACK(self->pages));
    adw_view_switcher_bar_set_reveal(ADW_VIEW_SWITCHER_BAR(switcher), FALSE);
    g_object_set_data(G_OBJECT(self->window), "mock-switcher-bar", switcher);
    gtk_box_append(GTK_BOX(body), switcher);
    return body;
}

static GtkWidget *status_screen(MainView *self, const char *icon, const char *title,
                                const char *description, const char *action,
                                MockMainAction code, gboolean spinner)
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
    GtkWidget *state = empty_state(icon, title, description);
    gtk_widget_set_vexpand(state, FALSE);
    gtk_box_append(GTK_BOX(box), state);
    if (action) {
        GtkWidget *button = action_button(action, code, self);
        gtk_widget_add_css_class(button, "suggested-action");
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

static GtkWidget *build_device_menu(MainView *self)
{
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = vbox(8);
    pad(box, 16);
    gtk_box_append(GTK_BOX(box), text_label("Marwan's iPhone", "title-3"));
    self->device_status = text_label("Connected", "dim-label");
    gtk_box_append(GTK_BOX(box), self->device_status);
    self->device_calls = text_label("Calls · Ready", NULL);
    self->device_contacts = text_label("Contacts · Synced", NULL);
    self->device_audio = text_label("Audio · Ready", NULL);
    gtk_box_append(GTK_BOX(box), self->device_calls);
    gtk_box_append(GTK_BOX(box), self->device_contacts);
    gtk_box_append(GTK_BOX(box), self->device_audio);
    gtk_box_append(GTK_BOX(box), text_label("Battery · 72%", NULL));
    gtk_box_append(GTK_BOX(box), text_label("Last contact sync · 2 minutes ago", NULL));
    gtk_box_append(GTK_BOX(box), action_button("Sync Contacts", MOCK_MAIN_SYNC, self));
    gtk_box_append(GTK_BOX(box), action_button("Disconnect", MOCK_MAIN_DISCONNECT, self));
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
    else if (adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(self->split)))
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(self->split), FALSE);
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
            "viewswitcherbar image { -gtk-icon-size: 24px; }"
            ".comfy-search { min-height: 44px; font-size: 1.1em; }"
            ".connection-card { padding: 20px; }");
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    MainView *self = g_new0(MainView, 1);
    self->model = model;
    self->callback = callback;
    self->user_data = user_data;
    self->selected_contact = -1;
    self->window = adw_application_window_new(app);
    g_object_set_data_full(G_OBJECT(self->window), "mock-main-view", self, g_free);
    gtk_window_set_title(GTK_WINDOW(self->window), "IBlueConnect");
    gtk_window_set_default_size(GTK_WINDOW(self->window), 480, 700);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *title_stack = gtk_stack_new();
    GtkWidget *header_switcher = adw_view_switcher_new();
    gtk_stack_add_named(GTK_STACK(title_stack), header_switcher, "switcher");
    gtk_stack_add_named(GTK_STACK(title_stack), text_label("IBlueConnect", "heading"), "title");
    gtk_stack_set_visible_child_name(GTK_STACK(title_stack), "switcher");
    GtkWidget *header_bin = adw_breakpoint_bin_new();
    gtk_widget_set_size_request(header_bin, 140, 32);
    adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(header_bin), title_stack);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), header_bin);
    self->device = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(self->device), "Marwan's iPhone ●");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->device), build_device_menu(self));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), self->device);
    GtkWidget *menu = gtk_button_new_from_icon_name("open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu, "Preferences");
    gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Preferences", -1);
    g_signal_connect(menu, "clicked", G_CALLBACK(preferences_clicked), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    self->screen = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->screen), build_connected(self), "connected");
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(header_switcher), ADW_VIEW_STACK(self->pages));
    GtkWidget *switcher_bar = g_object_get_data(G_OBJECT(self->window), "mock-switcher-bar");
    AdwBreakpoint *narrow_nav = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 420px"));
    GValue title = G_VALUE_INIT, reveal = G_VALUE_INIT;
    g_value_init(&title, G_TYPE_STRING);
    g_value_set_string(&title, "title");
    adw_breakpoint_add_setter(narrow_nav, G_OBJECT(title_stack), "visible-child-name", &title);
    g_value_unset(&title);
    g_value_init(&reveal, G_TYPE_BOOLEAN);
    g_value_set_boolean(&reveal, TRUE);
    adw_breakpoint_add_setter(narrow_nav, G_OBJECT(switcher_bar), "reveal", &reveal);
    g_value_unset(&reveal);
    adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(header_bin), narrow_nav);
    gtk_stack_add_named(GTK_STACK(self->screen), status_screen(self, "phone-symbolic",
        "Connect your iPhone", "Connect your iPhone to make and receive calls from your GNOME desktop.\nPreviously connected: Marwan's iPhone",
        "Find iPhone", MOCK_MAIN_CONNECT, FALSE), "disconnected");
    gtk_stack_add_named(GTK_STACK(self->screen), status_screen(self, "phone-symbolic",
        "Connecting to Marwan's iPhone…", "Preparing calls…\nLoading contacts…",
        NULL, MOCK_MAIN_CONNECT, TRUE), "connecting");
    gtk_stack_add_named(GTK_STACK(self->screen), status_screen(self, "dialog-error-symbolic",
        "Couldn't Connect", "IBlueConnect couldn't connect to Marwan's iPhone.",
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
    gtk_menu_button_set_label(GTK_MENU_BUTTON(self->device),
        self->model->connection == MOCK_CONNECTION_CONNECTED ? "Marwan's iPhone ●" : "iPhone ×");
    gtk_label_set_text(GTK_LABEL(self->device_status),
        self->model->connection == MOCK_CONNECTION_CONNECTED ? "Connected" : "Disconnected");
    gtk_label_set_text(GTK_LABEL(self->device_calls),
        self->model->connection != MOCK_CONNECTION_CONNECTED || self->model->calls_unavailable ?
        "Calls · Unavailable" : "Calls · Ready");
    gtk_label_set_text(GTK_LABEL(self->device_contacts),
        self->model->contact_sync_error ? "Contacts · Update failed" : "Contacts · Synced");
    gtk_label_set_text(GTK_LABEL(self->device_audio),
        self->model->connection == MOCK_CONNECTION_CONNECTED ? "Audio · Ready" : "Audio · Unavailable");
    adw_banner_set_revealed(ADW_BANNER(self->calls_error), self->model->calls_unavailable);
    adw_banner_set_revealed(ADW_BANNER(self->sync_error), self->model->contact_sync_error);
    gboolean ready = self->model->connection == MOCK_CONNECTION_CONNECTED && !self->model->calls_unavailable;
    gtk_label_set_text(GTK_LABEL(self->link_status), ready ? "━━━━━━" : "━━ × ━━");
    gtk_widget_remove_css_class(self->link_status, ready ? "error" : "accent");
    gtk_widget_add_css_class(self->link_status, ready ? "accent" : "error");
    GtkWidget *caption = g_object_get_data(G_OBJECT(self->connection_card), "caption");
    gtk_label_set_text(GTK_LABEL(caption), ready ? "iPhone connected · Calls ready" :
                       "iPhone connected · Calls unavailable");
    render_recents(self);
    render_contacts(self);
    render_detail(self);
    dial_changed(GTK_EDITABLE(self->dial_entry), self);
}
