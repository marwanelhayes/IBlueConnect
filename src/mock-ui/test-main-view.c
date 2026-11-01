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

/* Search entries filter after a short typing pause (GtkSearchEntry's search delay). */
static void wait_search(void)
{
    gint64 deadline = g_get_monotonic_time() + 400000;
    while (g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(2000);
    }
}

static GtkWidget *find_named(GtkWidget *root, const char *name)
{
    if (g_strcmp0(gtk_widget_get_name(root), name) == 0) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_named(child, name);
        if (found) return found;
    }
    return NULL;
}

static void collect_class(GtkWidget *root, const char *css, GPtrArray *found)
{
    if (gtk_widget_has_css_class(root, css)) g_ptr_array_add(found, root);
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
        collect_class(child, css, found);
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

static void connected_home(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_assert_nonnull(find_label(window, "Favorites"));
    g_assert_nonnull(find_label(window, "Recent calls"));
    g_assert_nonnull(find_label(window, "Contacts"));
    g_assert_nonnull(find_label(window, "Keypad"));
    g_assert_nonnull(find_label(window, "Connected, calls ready"));
    g_assert_true(ADW_IS_AVATAR(find_avatar(window, "Ahmed Mohamed")));
    const MockRecent *first_missed = NULL;
    for (size_t i = 0; i < mock_recents_count && !first_missed; i++)
        if (g_ascii_strcasecmp(mock_recents[i].kind, "Missed") == 0 && !mock_recents[i].duration)
            first_missed = &mock_recents[i];
    g_assert_nonnull(first_missed);
    GtkWidget *missed = find_label(window, "Missed");
    g_assert_nonnull(missed);
    g_assert_true(gtk_widget_has_css_class(missed, "error"));
    GtkWidget *missed_row = gtk_widget_get_ancestor(missed, GTK_TYPE_BUTTON);
    g_assert_nonnull(missed_row);
    g_assert_true(gtk_widget_has_css_class(missed_row, "recent-row"));
    g_assert_false(gtk_widget_has_css_class(missed_row, "error"));
    g_assert_true(gtk_widget_has_css_class(find_label(missed_row, first_missed->name), "error"));
    g_assert_nonnull(find_label(missed_row, first_missed->time)); /* The time has its own right-hand label. */
    g_assert_null(find_type(missed_row, ADW_TYPE_AVATAR)); /* Recents rows carry no avatar. */
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
    g_assert_nonnull(find_label(window, "Calls unavailable"));
    g_assert_nonnull(find_label(window, "Calls · Unavailable"));
    g_assert_nonnull(find_label(window, "Contacts · Update failed"));
    GtkWidget *pages = find_pages(window);
    GtkWidget *recents = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    GPtrArray *rows = g_ptr_array_new();
    collect_class(recents, "recent-row", rows);
    g_assert_cmpuint(rows->len, >, 0);
    g_signal_emit_by_name(rows->pdata[0], "clicked");
    g_assert_cmpint(event.count, ==, 1); /* Calling back is refused while calls are unavailable. */
    g_ptr_array_unref(rows);
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
    wait_search();
    g_assert_nonnull(find_label(window, "No Search Results"));
    gtk_editable_set_text(GTK_EDITABLE(search), "0101234");
    wait_search();
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
    GtkWidget *dial_page = gtk_widget_get_ancestor(entry, ADW_TYPE_VIEW_STACK);
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
    GtkWidget *last_key = find_named(dial, "dial-key-#");
    graphene_rect_t bounds;
    g_assert_nonnull(last_key);
    g_assert_true(gtk_widget_compute_bounds(last_key, dial, &bounds));
    g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height(dial));
    g_assert_cmpfloat(bounds.size.height, <=, 60); /* User: the keypad looked too big at 72. */
    g_assert_cmpfloat(bounds.size.width, <=, 90);
    g_assert_cmpint(gtk_widget_get_width(find_button(dial, "Call")), <=, 240);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void add_call_opens_the_keypad(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.live = true;
    model.call_count = 3; /* An all-active conference can still add a call. */
    model.can_add_call = true;
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockAddCallTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *pages = find_pages(window);
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), !=, "dial");
    mock_main_window_show_dial(window);
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "dial");
    g_assert_nonnull(find_button(window, "Add call"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void siri_button_starts_siri_only_when_idle(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.live = true;
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockSiriTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *pages = find_pages(window);
    g_assert_null(find_named(pages, "siri-button")); /* Its own button, not part of the keypad. */
    GtkWidget *siri = find_named(window, "siri-button");
    g_assert_nonnull(siri);
    g_assert_null(gtk_widget_get_ancestor(siri, GTK_TYPE_OVERLAY)); /* Its own row, not tucked into the card. */
    g_assert_true(gtk_widget_has_css_class(siri, "pill"));
    g_assert_nonnull(find_label(siri, "Ask Siri"));
    g_assert_false(gtk_widget_has_css_class(siri, "suggested-action")); /* Outlined, not an accent fill. */
    g_assert_true(gtk_widget_is_sensitive(siri));
    g_signal_emit_by_name(siri, "clicked");
    g_assert_cmpint(event.count, ==, 1);
    g_assert_cmpint(event.action, ==, MOCK_MAIN_SIRI);
    /* The button tells the user when Siri is starting and when to speak, and cannot be pressed meanwhile. */
    model.siri = MOCK_SIRI_STARTING;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(siri, "Starting Siri…"));
    g_assert_false(gtk_widget_is_sensitive(siri));
    model.siri = MOCK_SIRI_LISTENING;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(siri, "Listening — speak now"));
    g_assert_false(gtk_widget_is_sensitive(siri));
    model.siri = MOCK_SIRI_IDLE;
    mock_main_window_update(window);
    g_assert_nonnull(find_label(siri, "Ask Siri"));
    g_assert_true(gtk_widget_is_sensitive(siri));
    model.call_count = 1;
    mock_main_window_update(window);
    g_assert_false(gtk_widget_is_sensitive(siri));
    model.call_count = 0;
    model.calls_unavailable = true;
    mock_main_window_update(window);
    g_assert_false(gtk_widget_is_sensitive(siri));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
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
    wait_search();
    GtkWidget *call = find_button(window, "Call 019 999");
    g_assert_nonnull(call);
    g_signal_emit_by_name(call, "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CALL);
    g_assert_cmpint(event.index, ==, -1);
    g_assert_cmpstr(event.value, ==, "019 999");
    gtk_editable_set_text(GTK_EDITABLE(search), "Ahmed");
    wait_search();
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
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "Recent calls"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "Incoming, 12 min"));
    g_assert_nonnull(find_label(GTK_WIDGET(detail), "17:42"));
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
    g_assert_nonnull(find_label(window, "iPhone not connected"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

/* System / Light / Dark lives in the phone menu and reports the choice; the app applies it. */
static void appearance_choice(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    g_assert_cmpint(model.appearance, ==, MOCK_APPEARANCE_SYSTEM);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockAppearanceTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *system = find_named(window, "appearance-system");
    GtkWidget *light = find_named(window, "appearance-light");
    GtkWidget *dark = find_named(window, "appearance-dark");
    g_assert_null(find_named(window, "appearance-black"));
    g_assert_nonnull(system);
    g_assert_nonnull(light);
    g_assert_nonnull(dark);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(system)));
    model.appearance = MOCK_APPEARANCE_LIGHT;
    mock_main_window_update(window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(light)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(system)));
    g_assert_cmpint(event.count, ==, 0); /* Reflecting the model does not echo a choice back. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark), TRUE);
    g_assert_cmpint(event.count, ==, 1);
    g_assert_cmpint(event.action, ==, MOCK_MAIN_APPEARANCE);
    g_assert_cmpstr(event.value, ==, "dark");
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

static void title_is_centered(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockTitleTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    for (int live = 0; live <= 1; live++) {
        model.live = live;
        GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
        GtkWidget *header = find_type(window, ADW_TYPE_HEADER_BAR);
        adw_header_bar_set_decoration_layout(ADW_HEADER_BAR(header), ":minimize,maximize,close");
        gtk_window_present(GTK_WINDOW(window));
        for (int i = 0; i < 50; i++) {
            while (g_main_context_iteration(NULL, FALSE));
            g_usleep(1000);
        }
        GtkWidget *title = find_label(header, "IBlueConnect");
        g_assert_nonnull(title);
        g_assert_true(gtk_widget_get_mapped(title));
        g_assert_cmpint(gtk_widget_get_width(window), <=, 500);
        graphene_rect_t bounds;
        g_assert_true(gtk_widget_compute_bounds(title, window, &bounds));
        PangoRectangle ink;
        pango_layout_get_pixel_extents(gtk_label_get_layout(GTK_LABEL(title)), &ink, NULL);
        int x, y;
        gtk_label_get_layout_offsets(GTK_LABEL(title), &x, &y);
        double center = bounds.origin.x + x + ink.x + ink.width / 2.0;
        double midpoint = gtk_widget_get_width(window) / 2.0;
        g_test_message("live=%d window midpoint=%.1f title box center=%.1f text center=%.1f",
                       live, midpoint, bounds.origin.x + bounds.size.width / 2.0, center);
        g_assert_cmpfloat(ABS(center - midpoint), <=, 2);
        gtk_window_destroy(GTK_WINDOW(window));
    }
    g_object_unref(app);
}

static guint8 pixel_alpha(cairo_surface_t *surface, int x, int y)
{
    return cairo_image_surface_get_data(surface)[y * cairo_image_surface_get_stride(surface) + x * 4 + 3];
}

/* iPhone-style Recents: the whole row calls back, no Call button, centred 90% separators between rows. */
/* The "!" on a Recents row opens that caller's card without calling, and Back returns to Recents. */
static void recent_info_opens_the_card(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockRecentInfoTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    GtkWidget *pages = find_pages(window);
    GtkWidget *info = find_named(window, "recent-info");
    g_assert_nonnull(info);
    g_assert_nonnull(find_label(info, "!"));
    g_assert_cmpstr(gtk_widget_get_tooltip_text(info), ==, "Details for Ahmed Mohamed");
    g_signal_emit_by_name(info, "clicked");
    g_assert_cmpint(event.count, ==, 0); /* Looks, never calls. */
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "contacts");
    GtkWidget *split = find_split(window);
    g_assert_true(adw_navigation_split_view_get_show_content(ADW_NAVIGATION_SPLIT_VIEW(split)));
    GtkWidget *detail = gtk_widget_get_ancestor(find_label(window, "Phone numbers"), GTK_TYPE_BOX);
    g_assert_nonnull(find_label(detail, "Ahmed Mohamed"));
    g_assert_nonnull(find_label(detail, "Recent calls"));
    g_assert_nonnull(find_label(detail, "Incoming, 12 min"));
    GtkWidget *back = find_tooltip(detail, "Back to recents");
    g_assert_nonnull(back);
    g_signal_emit_by_name(back, "clicked");
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "recents");
    g_assert_false(adw_navigation_split_view_get_show_content(ADW_NAVIGATION_SPLIT_VIEW(split)));
    /* Opening a contact from Contacts afterwards goes back to Contacts, not Recents. */
    g_assert_null(find_tooltip(window, "Back to recents"));
    g_assert_nonnull(find_tooltip(window, "Back to contacts"));
    /* A caller who is not a contact still gets a card: their number and their calls only. */
    const MockContact *saved_contacts = mock_contacts;
    const MockRecent *saved_recents = mock_recents;
    const size_t *saved_favorites = mock_favorites;
    size_t contacts_count = mock_contacts_count, recents_count = mock_recents_count, favorites_count = mock_favorites_count;
    static const MockContact contacts[] = { { "Ahmed Mohamed", { "010 1234 5678" }, { "Mobile" } } };
    static const MockRecent recents[] = {
        { NULL, "0100 000 0001", "Missed", "9:00", NULL },
        { "Ahmed Mohamed", "010 1234 5678", "Outgoing", "8:00", "5 min" },
        { NULL, "0100 000 0002", "Incoming", "7:00", "2 min" },
        { NULL, "", "Missed", "6:00", NULL },
    };
    mock_ui_set_data(contacts, 1, recents, 4, NULL, 0);
    mock_main_window_update(window);
    info = find_named(window, "recent-info");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(info), ==, "Details for 0100 000 0001");
    g_signal_emit_by_name(info, "clicked");
    detail = gtk_widget_get_ancestor(find_label(window, "Phone numbers"), GTK_TYPE_BOX);
    g_assert_nonnull(find_tooltip(detail, "Call 0100 000 0001"));
    g_assert_nonnull(find_label(detail, "Missed"));
    g_assert_null(find_label(detail, "Incoming, 2 min"));
    g_assert_null(find_label(detail, "Outgoing, 5 min"));
    g_signal_emit_by_name(find_tooltip(detail, "Back to recents"), "clicked");
    /* A withheld number: nothing to call and nothing guessed. */
    GPtrArray *infos = g_ptr_array_new();
    collect_class(window, "recent-info", infos);
    g_assert_cmpuint(infos->len, ==, 4);
    info = infos->pdata[3];
    g_ptr_array_free(infos, TRUE);
    g_assert_cmpstr(gtk_widget_get_tooltip_text(info), ==, "Details for Unknown caller");
    g_signal_emit_by_name(info, "clicked");
    detail = gtk_widget_get_ancestor(find_tooltip(window, "Back to recents"), GTK_TYPE_BOX);
    g_assert_nonnull(find_label(detail, "Unknown caller"));
    g_assert_null(find_button(detail, "Call"));
    g_assert_null(find_label(detail, "Phone numbers"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
    mock_ui_set_data(saved_contacts, contacts_count, saved_recents, recents_count, saved_favorites, favorites_count);
}

static void recent_rows_call_back_and_have_centred_separators(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockRecentRowsTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    gtk_window_present(GTK_WINDOW(window));
    GtkWidget *pages = find_pages(window);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "recents");
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    GtkWidget *recents = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    GPtrArray *rows = g_ptr_array_new(), *lines = g_ptr_array_new();
    collect_class(recents, "recent-row", rows);
    collect_class(recents, "recent-separator", lines);
    g_assert_cmpuint(rows->len, ==, mock_recents_count);
    g_assert_cmpuint(lines->len, ==, rows->len - 1);
    for (guint i = 0; i < rows->len; i++) {
        g_assert_true(GTK_IS_BUTTON(rows->pdata[i]));
        g_assert_null(find_button(rows->pdata[i], "Call"));
    }
    g_signal_emit_by_name(rows->pdata[0], "clicked");
    g_assert_cmpint(event.count, ==, 1);
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CALL);
    g_assert_cmpstr(event.value, ==, mock_recents[0].number);
    GtkWidget *line = lines->pdata[0];
    int width = gtk_widget_get_width(line), height = gtk_widget_get_height(line);
    g_assert_cmpint(width, >, 100);
    g_assert_cmpint(height, >=, 1);
    GdkPaintable *paintable = gtk_widget_paintable_new(line);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_assert_nonnull(node);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surface);
    int y = height / 2;
    g_assert_cmpint(pixel_alpha(surface, width * 2 / 100, y), ==, 0);
    g_assert_cmpint(pixel_alpha(surface, width * 7 / 100, y), >, 0);
    g_assert_cmpint(pixel_alpha(surface, width / 2, y), >, 0);
    g_assert_cmpint(pixel_alpha(surface, width * 93 / 100, y), >, 0);
    g_assert_cmpint(pixel_alpha(surface, width * 98 / 100, y), ==, 0);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    g_ptr_array_unref(rows);
    g_ptr_array_unref(lines);
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static GtkWidget *first_recent_row(GtkWidget *root)
{
    GPtrArray *rows = g_ptr_array_new();
    collect_class(root, "recent-row", rows);
    GtkWidget *first = rows->len ? rows->pdata[0] : NULL;
    g_ptr_array_unref(rows);
    return first;
}

static guint count_class(GtkWidget *root, const char *css)
{
    GPtrArray *found = g_ptr_array_new();
    collect_class(root, css, found);
    guint count = found->len;
    g_ptr_array_unref(found);
    return count;
}

/* Long histories show 50 calls, then 50 more per "Show more"; unchanged data is not rebuilt. */
static void recents_are_paged_and_not_rebuilt_without_changes(void)
{
    MockRecent many[120];
    for (int i = 0; i < 120; i++)
        many[i] = (MockRecent){ .name = mock_recents[0].name, .number = mock_recents[0].number,
                                .kind = "Outgoing", .time = "Yesterday" };
    const MockRecent *saved = mock_recents;
    size_t saved_count = mock_recents_count;
    mock_ui_set_data(mock_contacts, mock_contacts_count, many, 120, mock_favorites, mock_favorites_count);
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockPagingTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_assert_cmpuint(count_class(window, "recent-row"), ==, 50);
    g_object_set_data(G_OBJECT(first_recent_row(window)), "test-marker", GINT_TO_POINTER(1));
    mock_main_window_update(window);
    g_assert_nonnull(g_object_get_data(G_OBJECT(first_recent_row(window)), "test-marker"));
    GtkWidget *more = find_button(window, "Show more");
    g_assert_nonnull(more);
    g_signal_emit_by_name(more, "clicked");
    g_assert_cmpuint(count_class(window, "recent-row"), ==, 100);
    g_signal_emit_by_name(find_button(window, "Show more"), "clicked");
    g_assert_cmpuint(count_class(window, "recent-row"), ==, 120);
    g_assert_null(find_button(window, "Show more"));
    g_object_set_data(G_OBJECT(first_recent_row(window)), "test-marker", GINT_TO_POINTER(1));
    mock_ui_set_data(mock_contacts, mock_contacts_count, many, 60, mock_favorites, mock_favorites_count);
    mock_main_window_update(window);
    g_assert_null(g_object_get_data(G_OBJECT(first_recent_row(window)), "test-marker")); /* Data changed: rebuilt. */
    gtk_window_destroy(GTK_WINDOW(window));
    mock_ui_set_data(mock_contacts, mock_contacts_count, saved, saved_count, mock_favorites, mock_favorites_count);
    g_object_unref(app);
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
    GtkWidget *two = find_named(window, "dial-key-2");
    g_assert_nonnull(find_label(two, "2"));
    g_assert_nonnull(find_label(two, "ABC"));
    GtkWidget *search = find_search(window, "Search contacts or enter a number");
    g_assert_cmpint(gtk_widget_get_height(search), >=, 44);
    GtkWidget *pages = find_pages(window);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(pages), "dial");
    for (int i = 0; i < 20; i++) while (g_main_context_iteration(NULL, FALSE));
    GtkWidget *dial = adw_view_stack_get_visible_child(ADW_VIEW_STACK(pages));
    graphene_rect_t five;
    g_assert_true(gtk_widget_compute_bounds(find_named(dial, "dial-key-5"), dial, &five));
    g_assert_cmpint(gtk_widget_get_height(window), <=, 700);
    g_assert_cmpfloat(five.size.width, <=, 80);
    g_assert_cmpfloat(five.size.height, <=, 80);
    g_assert_cmpfloat(ABS(five.origin.x + five.size.width / 2 - gtk_widget_get_width(dial) / 2.0), <=, 2);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

static void live_mode_hides_mock_controls(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.live = true;
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockLiveTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    g_assert_true(gtk_widget_get_visible(find_button(window, "Disconnect")));
    g_assert_false(gtk_widget_get_visible(find_tooltip(window, "Preferences")));
    GtkWidget *number = find_dial_entry(window);
    gtk_editable_set_text(GTK_EDITABLE(number), "123");
    model.connection = MOCK_CONNECTION_CONNECTED;
    model.call_count = 1;
    model.can_add_call = true;
    mock_main_window_update(window);
    GtkWidget *add = find_button(window, "Add call");
    g_assert_nonnull(add);
    g_assert_true(gtk_widget_get_sensitive(add));
    model.call_count = 2;
    model.can_add_call = false;
    mock_main_window_update(window);
    g_assert_false(gtk_widget_get_sensitive(add));
    model.contact_sync_error = true;
    g_strlcpy(model.sync_label, "Contact sync failed — allow Sync Contacts", sizeof model.sync_label);
    mock_main_window_update(window);
    g_assert_nonnull(find_label(window, "Contact sync failed — allow Sync Contacts"));
    model.connection = MOCK_CONNECTION_DISCONNECTED;
    mock_main_window_update(window);
    g_assert_null(find_button(window, "Find iPhone"));
    GtkWidget *settings = find_button(window, "Open Bluetooth Settings");
    g_assert_nonnull(settings);
    g_signal_emit_by_name(settings, "clicked");
    g_assert_cmpint(event.action, ==, MOCK_MAIN_CONNECT);
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void live_exit_controls_dispatch_guarded_actions(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.live = true;
    Event event = {0};
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockExitTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, on_action, &event);
    const struct { const char *label, *name; MockMainAction action; } actions[] = {
        { "Disconnect", "safe-disconnect", MOCK_MAIN_DISCONNECT },
        { "Resume call monitoring", "resume-monitoring", MOCK_MAIN_RESUME },
        { "Forget phone…", "safe-forget", MOCK_MAIN_FORGET },
        { "Quit IBlueConnect", "safe-quit", MOCK_MAIN_QUIT }
    };
    /* Keep exit requests available during a call: the backend checks fresh state. */
    model.call = MOCK_CALL_ACTIVE;
    mock_main_window_update(window);
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        GtkWidget *button = find_button(window, actions[i].label);
        g_assert_nonnull(button);
        g_assert_true(gtk_widget_get_visible(button));
        g_assert_true(gtk_widget_get_sensitive(button));
        g_assert_cmpstr(gtk_widget_get_name(button), ==, actions[i].name);
        g_assert_nonnull(gtk_widget_get_tooltip_text(button));
        g_signal_emit_by_name(button, "clicked");
        g_assert_cmpint(event.action, ==, actions[i].action);
        g_assert_cmpint(event.count, ==, i + 1);
        g_assert_null(event.value);
    }
    gtk_window_destroy(GTK_WINDOW(window));
    model.live = false;
    window = mock_main_window_new(app, &model, on_action, &event);
    for (guint i = 1; i < G_N_ELEMENTS(actions); i++)
        g_assert_false(gtk_widget_get_visible(find_button(window, actions[i].label)));
    gtk_window_destroy(GTK_WINDOW(window));
    g_free(event.value);
    g_object_unref(app);
}

static void non_ascii_names_get_letter_sections(void)
{
    static const MockContact contacts[] = { { "émile zola", { "111" }, { "Mobile" } } };
    const MockContact *saved = mock_contacts;
    size_t saved_count = mock_contacts_count;
    mock_contacts = contacts;
    mock_contacts_count = 1;
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockUtf8Test", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_assert_nonnull(find_label(window, "É"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
    mock_contacts = saved;
    mock_contacts_count = saved_count;
}

/* Text tabs, in this order, stay in step with the pages whichever side changes first. */
static void tabs_follow_the_pages(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockTabsTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    g_assert_null(find_type(window, ADW_TYPE_VIEW_SWITCHER));
    g_assert_null(find_type(window, ADW_TYPE_VIEW_SWITCHER_BAR));
    GtkWidget *pages = find_pages(window);
    GtkWidget *recents = find_named(window, "tab-recents"), *dial = find_named(window, "tab-dial"),
              *contacts = find_named(window, "tab-contacts");
    g_assert_nonnull(find_label(recents, "Recents"));
    g_assert_nonnull(find_label(dial, "Keypad"));
    g_assert_nonnull(find_label(contacts, "Contacts"));
    g_assert_true(gtk_widget_get_next_sibling(recents) == dial);
    g_assert_true(gtk_widget_get_next_sibling(dial) == contacts);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(recents)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(contacts), TRUE);
    g_assert_cmpstr(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(pages)), ==, "contacts");
    press(window, "<Control>d");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dial)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(contacts)));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

/* A ready link is one blue wire; an unavailable link is red with an X, and says so. */
static void link_is_blue_when_ready_and_red_with_x_otherwise(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockLinkTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *link = find_named(window, "link-status");
    GtkWidget *wire = find_named(link, "link-wire"), *x = find_named(link, "link-x"), *far = find_named(link, "link-wire-far");
    GdkRGBA color;
    gtk_widget_get_color(wire, &color);
    g_assert_cmpfloat(color.blue, >, color.red * 2);
    g_assert_false(gtk_widget_get_visible(x));
    g_assert_false(gtk_widget_get_visible(far));
    g_assert_nonnull(find_label(link, "Connected, calls ready"));
    model.calls_unavailable = true;
    mock_main_window_update(window);
    gtk_widget_get_color(wire, &color);
    g_assert_cmpfloat(color.red, >, color.blue * 1.2);
    g_assert_true(gtk_widget_get_visible(x));
    g_assert_true(gtk_widget_get_visible(far));
    g_assert_nonnull(find_label(link, "Calls unavailable"));
    model.calls_unavailable = false;
    mock_main_window_update(window);
    gtk_widget_get_color(wire, &color);
    g_assert_cmpfloat(color.blue, >, color.red * 2); /* And back. */
    GtkWidget *lost = find_named(window, "link-lost");
    gtk_widget_get_color(find_named(lost, "link-wire"), &color);
    g_assert_cmpfloat(color.red, >, color.blue * 1.2);
    g_assert_true(gtk_widget_get_visible(find_named(lost, "link-x")));
    g_assert_nonnull(find_label(lost, "iPhone not connected"));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(app);
}

/* Contacts are names in an address book: no avatars or numbers per row, and search by number still works. */
static void contacts_are_an_address_book(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    GtkApplication *app = gtk_application_new("org.iblueconnect.MockBookTest", 0);
    g_application_register(G_APPLICATION(app), NULL, NULL);
    GtkWidget *window = mock_main_window_new(app, &model, NULL, NULL);
    GtkWidget *split = find_split(window);
    GtkWidget *list = (GtkWidget *)adw_navigation_split_view_get_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split));
    g_assert_null(find_type(list, ADW_TYPE_AVATAR));
    g_assert_nonnull(find_label(list, "A")); /* First letter of the sample names. */
    g_assert_null(find_label(list, mock_contacts[0].numbers[0]));
    gtk_editable_set_text(GTK_EDITABLE(find_search(list, "Search contacts")), "0101234");
    wait_search();
    g_assert_nonnull(find_label(list, mock_contacts[0].name));
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
    g_test_add_func("/main/add-call-keypad", add_call_opens_the_keypad);
    g_test_add_func("/main/siri", siri_button_starts_siri_only_when_idle);
    g_test_add_func("/main/adaptive-contacts", contacts_adapt_to_window_width);
    g_test_add_func("/main/search-number", home_search_calls_typed_number);
    g_test_add_func("/main/contact-details", contact_details);
    g_test_add_func("/main/device-popover", device_popover);
    g_test_add_func("/main/appearance", appearance_choice);
    g_test_add_func("/main/preferences", preferences);
    g_test_add_func("/main/keyboard-shortcuts", keyboard_shortcuts);
    g_test_add_func("/main/proportions", comfortable_proportions);
    g_test_add_func("/main/recent-rows", recent_rows_call_back_and_have_centred_separators);
    g_test_add_func("/main/recent-info", recent_info_opens_the_card);
    g_test_add_func("/main/recent-paging", recents_are_paged_and_not_rebuilt_without_changes);
    g_test_add_func("/main/title-centered", title_is_centered);
    g_test_add_func("/main/live-mode", live_mode_hides_mock_controls);
    g_test_add_func("/main/safe-exit", live_exit_controls_dispatch_guarded_actions);
    g_test_add_func("/main/utf8-sections", non_ascii_names_get_letter_sections);
    g_test_add_func("/main/tabs", tabs_follow_the_pages);
    g_test_add_func("/main/link", link_is_blue_when_ready_and_red_with_x_otherwise);
    g_test_add_func("/main/address-book", contacts_are_an_address_book);
    return g_test_run();
}
