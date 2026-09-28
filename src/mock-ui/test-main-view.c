#include "main-view.h"

static GtkWidget *find_label(GtkWidget *root, const char *text)
{
    if (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_label(child, text);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_button(GtkWidget *root, const char *text)
{
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_button(child, text);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_search(GtkWidget *root, const char *placeholder)
{
    if (GTK_IS_SEARCH_ENTRY(root) && g_strcmp0(
        gtk_search_entry_get_placeholder_text(GTK_SEARCH_ENTRY(root)), placeholder) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_search(child, placeholder);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_dial_entry(GtkWidget *root)
{
    if (GTK_IS_ENTRY(root) && g_strcmp0(
        gtk_entry_get_placeholder_text(GTK_ENTRY(root)), "Enter a number") == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_dial_entry(child);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_pages(GtkWidget *root)
{
    if (ADW_IS_VIEW_STACK(root)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_pages(child);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_split(GtkWidget *root)
{
    if (ADW_IS_NAVIGATION_SPLIT_VIEW(root)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_split(child);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_avatar(GtkWidget *root, const char *text)
{
    if (ADW_IS_AVATAR(root) && g_strcmp0(adw_avatar_get_text(ADW_AVATAR(root)), text) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_avatar(child, text);
        if (found) return found;
    }
    return NULL;
}

typedef struct { MockMainAction action; int index; char *value; int count; } Event;

static void on_action(MockMainAction action, int index, const char *value, gpointer user_data)
{
    Event *event = user_data;
    event->action = action;
    event->index = index;
    g_free(event->value);
    event->value = g_strdup(value);
    event->count++;
}

static void connected_home(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_assert_nonnull(find_label(window, "Favorites"));
    g_assert_nonnull(find_label(window, "Recent Calls"));
    g_assert_nonnull(find_label(window, "Contacts"));
    g_assert_nonnull(find_label(window, "Dial"));
    g_assert_nonnull(find_label(window, "iPhone connected · Calls ready"));
    g_assert_true(ADW_IS_AVATAR(find_avatar(window, "Ahmed Mohamed")));
    GtkWidget *missed = find_label(window, "Missed");
    g_assert_nonnull(missed);
    g_assert_true(gtk_widget_has_css_class(missed, "error"));
    for (GtkWidget *w = missed; w; w = gtk_widget_get_parent(w)) {
        if (gtk_widget_get_first_child(w) && ADW_IS_AVATAR(gtk_widget_get_first_child(w))) {
            GtkWidget *call = find_button(w, "Call");
            g_assert_nonnull(call);
            for (GtkWidget *a = call; a != w; a = gtk_widget_get_parent(a))
                g_assert_false(gtk_widget_has_css_class(a, "error"));
            g_assert_false(gtk_widget_has_css_class(w, "error"));
            break;
        }
    }
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void connection_states(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockStateTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    model.connection = MOCK_CONNECTION_DISCONNECTED;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "Connect your iPhone"));
    GtkWidget *find = find_button(window, "Find iPhone");
    g_assert_nonnull(find);
    g_signal_emit_by_name(find, "clicked");
    g_assert_cmpint(event.count, ==, 1);
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CONNECT);
    model.connection = MOCK_CONNECTION_CONNECTING;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "Connecting to Marwan's iPhone…"));
    model.connection = MOCK_CONNECTION_ERROR;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "Couldn't Connect"));
    model.connection = MOCK_CONNECTION_CONNECTED;
    model.calls_unavailable = true;
    model.contact_sync_error = true;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "iPhone connected · Calls unavailable"));
    g_assert_nonnull(find_label(window, "Calls · Unavailable"));
    g_assert_nonnull(find_label(window, "Contacts · Update failed"));
    GtkWidget *pages = find_pages(window);
    GtkWidget *recents = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    GtkWidget *call = find_button(recents, "Call");
    g_assert_nonnull(call);
    g_assert_false(gtk_widget_is_sensitive(call));
    g_signal_emit_by_name(call, "clicked");
    g_assert_cmpint(event.count, ==, 1);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "dial");
    GtkWidget *dial = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    gtk_editable_set_text(GTK_EDITABLE(find_dial_entry(dial)), "0101234");
    g_assert_false(gtk_widget_is_sensitive(find_button(dial, "Call")));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void search_dial_and_empty_states(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockSearchTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *search = find_search(window, "Search contacts");
    g_assert_nonnull(search);
    gtk_editable_set_text(GTK_EDITABLE(search), "zzzz");
    g_assert_nonnull(find_label(window, "No Search Results"));
    gtk_editable_set_text(GTK_EDITABLE(search), "0101234");
    g_assert_null(find_label(window, "No Search Results"));
    model.empty_contacts = true;
    model.empty_recents = true;
    model.empty_favorites = true;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "No Contacts"));
    g_assert_nonnull(find_label(window, "No Recent Calls"));
    g_assert_nonnull(find_label(window, "No Favorites"));
    model.empty_contacts = false;
    GtkWidget *entry = find_dial_entry(window);
    g_assert_nonnull(entry);
    gtk_editable_set_text(GTK_EDITABLE(entry), "0101234");
    g_assert_nonnull(find_label(window, "Ahmed Mohamed"));
    GtkWidget *dial_page = gtk_widget_get_parent(entry);
    GtkWidget *call = find_button(dial_page, "Call");
    g_assert_nonnull(call);
    g_signal_emit_by_name(call, "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CALL);
    g_assert_cmpint(event.index, ==, -1);
    g_assert_cmpstr(event.value, ==, "0101234");
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void default_window_keeps_keypad_visible(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockSizeTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *pages = find_pages(window);
    g_assert_nonnull(pages);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "dial");
    gtk_window_present(GTK_WINDOW(window));
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    g_assert_cmpint(gtk_widget_get_width(window), <=, 500);
    GtkWidget *dial = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    GtkWidget *last_key = find_button(dial, "#");
    graphene_rect_t bounds;
    g_assert_nonnull(last_key);
    g_assert_true(gtk_widget_compute_bounds(last_key, dial, &bounds));
    g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height(dial));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void contacts_adapt_to_window_width(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockAdaptiveTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *pages = find_pages(window);
    GtkWidget *split = find_split(window);
    g_assert_nonnull(split);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "contacts");
    gtk_window_present(GTK_WINDOW(window));
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    g_assert_true(adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(split)));
    gtk_window_destroy(GTK_WINDOW(window));
    window = mock_main_window_new(app, &model, NULL, NULL);
    pages = find_pages(window);
    split = find_split(window);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "contacts");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 750);
    gtk_window_present(GTK_WINDOW(window));
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    g_assert_false(adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(split)));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void home_search_calls_typed_number(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockNumberTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *search = find_search(window, "Search contacts or enter a number");
    g_assert_nonnull(search);
    gtk_editable_set_text(GTK_EDITABLE(search), "019 999");
    GtkWidget *call = find_button(window, "Call 019 999");
    g_assert_nonnull(call);
    g_signal_emit_by_name(call, "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CALL);
    g_assert_cmpint(event.index, ==, -1);
    g_assert_cmpstr(event.value, ==, "019 999");
    gtk_editable_set_text(GTK_EDITABLE(search), "Ahmed");
    g_assert_null(find_button(window, "Call Ahmed"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void contact_details(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockDetailTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *split = find_split(window);
    GtkWidget *list = (GtkWidget *)adw_navigation_split_view_get_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split));
    GtkWidget *detail = (GtkWidget *)adw_navigation_split_view_get_content(ADW_NAVIGATION_SPLIT_VIEW(split));
    GtkWidget *row = NULL;
    for (GtkWidget *w = find_label(GTK_WIDGET(list), "Ahmed Mohamed"); w; w = gtk_widget_get_parent(w))
        if (GTK_IS_BUTTON(w)) { row = w; break; }
    g_assert_nonnull(row);
    g_signal_emit_by_name(row, "clicked");
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "Mobile"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "010 1234 5678"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "Work"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "02 1234 5678"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "Recent Calls"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "17:42  ·  Incoming  ·  12 min"));
    g_signal_emit_by_name(find_button(GTK_WIDGET(detail), "Call"), "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CALL);
    g_assert_cmpint(event.index, ==, 0);
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static GtkWidget *find_tooltip(GtkWidget *root, const char *text)
{
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_widget_get_tooltip_text(root), text) == 0) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_tooltip(child, text);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_pref_row(GtkWidget *root, const char *title)
{
    if (ADW_IS_PREFERENCES_ROW(root) &&
        g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root)), title) == 0) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_pref_row(child, title);
        if (found) return found;
    }
    return NULL;
}

static void press(GtkWidget *window, const char *accel)
{
    GListModel *controllers = gtk_widget_observe_controllers(window);
    gboolean done = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(controllers) && !done; i++) {
        GObject *c = g_list_model_get_item(controllers, i);
        if (GTK_IS_SHORTCUT_CONTROLLER(c)) {
            for (guint j = 0; j < g_list_model_get_n_items(G_LIST_MODEL(c)) && !done; j++) {
                GtkShortcut *sc = g_list_model_get_item(G_LIST_MODEL(c), j);
                char *trigger = gtk_shortcut_trigger_to_string(gtk_shortcut_get_trigger(sc));
                if (g_strcmp0(trigger, accel) == 0)
                    done = gtk_shortcut_action_activate(gtk_shortcut_get_action(sc), 0, window, NULL);
                g_free(trigger);
                g_object_unref(sc);
            }
        }
        g_object_unref(c);
    }
    g_object_unref(controllers);
    g_assert_true(done);
}

static void device_popover(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockDeviceTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    g_assert_nonnull(find_label(window, "Connected"));
    g_assert_nonnull(find_label(window, "Calls · Ready"));
    g_assert_nonnull(find_label(window, "Contacts · Synced"));
    g_assert_nonnull(find_label(window, "Audio · Ready"));
    g_assert_nonnull(find_label(window, "Battery · 72%"));
    g_assert_nonnull(find_label(window, "Last contact sync · 2 minutes ago"));
    g_signal_emit_by_name(find_button(window, "Sync Contacts"), "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_SYNC);
    g_signal_emit_by_name(find_button(window, "Disconnect"), "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_DISCONNECT);
    model.connection = MOCK_CONNECTION_DISCONNECTED;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "Disconnected"));
    g_assert_nonnull(find_label(window, "Calls · Unavailable"));
    g_assert_nonnull(find_label(window, "Audio · Unavailable"));
    g_assert_nonnull(find_label(window, "iPhone ×"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void preferences(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockPrefsTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_signal_emit_by_name(find_tooltip(window, "Preferences"), "clicked");
    GtkWidget *prefs = NULL;
    GListModel *tops = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tops) && !prefs; i++) {
        GtkWindow *w = g_list_model_get_item(tops, i);
        if (ADW_IS_PREFERENCES_WINDOW(w)) prefs = GTK_WIDGET(w);
        g_object_unref(w);
    }
    g_assert_nonnull(prefs);
    const char *rows[] = { "Start IBlueConnect automatically", "Reconnect automatically",
        "Show incoming-call notifications", "Sync contacts automatically", "Show contact photos",
        "Show floating call controller", "Keep call controller visible during calls",
        "Show audio activity visualization", "Follow system appearance" };
    for (size_t i = 0; i < G_N_ELEMENTS(rows); i++)
        g_assert_nonnull(find_pref_row(prefs, rows[i]));
    GtkWidget *toggle = find_pref_row(prefs, "Show contact photos");
    g_assert_true(ADW_IS_SWITCH_ROW(toggle));
    adw_switch_row_set_active(ADW_SWITCH_ROW(toggle), FALSE);
    g_assert_false(adw_switch_row_get_active(ADW_SWITCH_ROW(toggle)));
    gtk_window_destroy(GTK_WINDOW(prefs));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void keyboard_shortcuts(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockKeysTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *pages = find_pages(window);
    press(window, "<Control>d");
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "dial");
    press(window, "<Control>f");
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "recents");
    press(window, "<Control>n");
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "dial");
    press(window, "Escape");
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static GtkWidget *find_type(GtkWidget *root, GType type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_type(child, type);
        if (found) return found;
    }
    return NULL;
}

static void comfortable_proportions(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockSizesTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    gtk_window_present(GTK_WINDOW(window));
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    g_assert_null(find_button(window, "2 ABC"));
    g_assert_nonnull(find_button(window, "2"));
    GtkWidget *search = find_search(window, "Search contacts or enter a number");
    g_assert_cmpint(gtk_widget_get_height(search), >=, 44);
    GtkWidget *card = gtk_widget_get_parent(find_label(window, "iPhone connected · Calls ready"));
    graphene_rect_t card_box;
    g_assert_true(gtk_widget_compute_bounds(card, window, &card_box));
    g_assert_cmpfloat(card_box.size.height, >=, 110);
    GtkWidget *bar = find_type(window, ADW_TYPE_VIEW_SWITCHER_BAR);
    GtkWidget *icon = find_type(bar, GTK_TYPE_IMAGE);
    g_assert_nonnull(icon);
    int icon_min = 0;
    gtk_widget_measure(icon, GTK_ORIENTATION_VERTICAL, -1, &icon_min, NULL, NULL, NULL);
    g_assert_cmpint(icon_min, >=, 32);
    GtkWidget *pages = find_pages(window);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "dial");
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    GtkWidget *dial = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    graphene_rect_t five;
    g_assert_true(gtk_widget_compute_bounds(find_button(dial, "5"), dial, &five));
    g_assert_cmpint(gtk_widget_get_height(window), <=, 700);
    g_assert_cmpfloat(five.size.width, <=, 80);
    g_assert_cmpfloat(five.size.height, <=, 80);
    g_assert_cmpfloat(ABS(five.origin.x + five.size.width / 2 - gtk_widget_get_width(dial) / 2.0), <=, 2);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    adw_init();
    g_test_add_func("/main/connected-home", connected_home);
    g_test_add_func("/main/connection-states", connection_states);
    g_test_add_func("/main/search-dial-empty", search_dial_and_empty_states);
    g_test_add_func("/main/default-keypad", default_window_keeps_keypad_visible);
    g_test_add_func("/main/adaptive-contacts", contacts_adapt_to_window_width);
    g_test_add_func("/main/search-number", home_search_calls_typed_number);
    g_test_add_func("/main/contact-details", contact_details);
    g_test_add_func("/main/device-popover", device_popover);
    g_test_add_func("/main/preferences", preferences);
    g_test_add_func("/main/keyboard-shortcuts", keyboard_shortcuts);
    g_test_add_func("/main/proportions", comfortable_proportions);
    return g_test_run();
}
