#include "live.h"
#include <assert.h>
#include <string.h>

static MockUiModel live_model(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.live = true;
    return model;
}

static void incoming_snapshot_shows_the_caller(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Sara Ahmed", NULL);
    assert(model.connection == MOCK_CONNECTION_CONNECTED);
    assert(model.call == MOCK_CALL_INCOMING);
    assert(model.popup_visible);
    assert(model.caller_index == 2);
    assert(strcmp(model.call_id, "/call1") == 0);
}

static void status_maps_to_connection_states(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "manage calls on phone", "", "unavailable", "", NULL);
    assert(model.connection == MOCK_CONNECTION_CONNECTED);
    assert(model.calls_unavailable);
    live_apply_snapshot(&model, "ready", "", "idle", "", NULL);
    assert(!model.calls_unavailable);
    live_apply_snapshot(&model, "calling unavailable", "", "idle", "", NULL);
    assert(model.connection == MOCK_CONNECTION_DISCONNECTED);
    live_apply_snapshot(&model, NULL, "", "idle", "", NULL);
    assert(model.connection == MOCK_CONNECTION_ERROR);
}

static void ignore_lasts_until_ringing_ends(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    mock_ui_ignore(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    assert(model.call == MOCK_CALL_INCOMING_IGNORED);
    assert(!model.popup_visible);
    live_apply_snapshot(&model, "ready", "", "idle", "", NULL);
    assert(model.call == MOCK_CALL_IDLE);
    assert(!model.popup_visible);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    assert(model.call == MOCK_CALL_INCOMING);
    assert(model.popup_visible);
}

static void answered_here_becomes_the_controller(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    mock_ui_answer(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    assert(model.call == MOCK_CALL_ACTIVE);
    live_apply_snapshot(&model, "ready", "/call1", "active", "Dad", NULL);
    assert(model.popup_visible);
    mock_ui_tick(&model);
    mock_ui_tick(&model);
    assert(model.elapsed_seconds == 2);
    live_apply_snapshot(&model, "ready", "", "idle", "", NULL);
    assert(model.call == MOCK_CALL_ENDED && model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_IDLE && !model.popup_visible);
}

static void answered_on_phone_stays_off_screen(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad", NULL);
    live_apply_snapshot(&model, "ready", "/call1", "active", "Dad", NULL);
    assert(model.call == MOCK_CALL_ACTIVE && !model.popup_visible);
    live_apply_snapshot(&model, "ready", "", "idle", "", NULL);
    assert(model.call == MOCK_CALL_IDLE);
}

static void declined_shows_briefly_then_clears(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "0100", "0100");
    assert(model.caller_index == -1);
    assert(strcmp(model.dialed_number, "0100") == 0);
    mock_ui_decline(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "0100", "0100");
    live_apply_snapshot(&model, "ready", "", "idle", "", NULL);
    assert(model.call == MOCK_CALL_DECLINED && model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_IDLE);
}

static void outgoing_waits_for_the_phone(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call2", "dialing", "Sara Ahmed", NULL);
    assert(model.call == MOCK_CALL_OUTGOING && model.popup_visible);
    mock_ui_tick(&model);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_OUTGOING);
    live_apply_snapshot(&model, "ready", "/call2", "alerting", "Sara Ahmed", NULL);
    live_apply_snapshot(&model, "ready", "/call2", "active", "Sara Ahmed", NULL);
    assert(model.call == MOCK_CALL_ACTIVE && model.popup_visible);
}

static void daemon_lists_become_ui_data(void)
{
    GDateTime *noon = g_date_time_new_local(2026, 9, 28, 12, 0, 0);
    gint64 now = g_date_time_to_unix(noon);
    g_date_time_unref(noon);
    GVariant *contacts = g_variant_ref_sink(g_variant_new_parsed(
        "[('Zed', 'Mobile', '111'), ('Zed', 'Work', '222'), ('Amy', 'Phone', '333')]"));
    GVariant *recents = g_variant_ref_sink(g_variant_new_parsed(
        "[('Zed', '111', 'missed', %x), ('', '999', 'dialed', %x),"
        " ('Zed', '222', 'received', %x), ('Amy', '333', 'dialed', %x)]",
        now - 60, now - 86400, now - 3 * 86400, now - 30 * 86400));
    LiveData *data = live_load(contacts, recents, now);
    assert(mock_contacts_count == 2);
    const MockContact *zed = &mock_contacts[mock_ui_contact_named("Zed")];
    assert(strcmp(zed->numbers[1], "222") == 0 && strcmp(zed->labels[1], "Work") == 0);
    assert(mock_recents_count == 4);
    assert(strcmp(mock_recents[0].kind, "Missed") == 0 && strcmp(mock_recents[0].time, "11:59") == 0);
    assert(mock_recents[1].name == NULL && strcmp(mock_recents[1].number, "999") == 0);
    assert(strcmp(mock_recents[1].time, "Yesterday") == 0);
    assert(strcmp(mock_recents[1].kind, "Outgoing") == 0);
    assert(strcmp(mock_recents[3].time, "2026-08-29") == 0);
    assert(mock_favorites_count == 2);
    assert(strcmp(mock_contacts[mock_favorites[0]].name, "Zed") == 0);
    LiveData *newer = live_load(contacts, recents, now);
    live_free(data); /* Freeing replaced data keeps the newer set installed. */
    assert(mock_contacts_count == 2);
    live_free(newer);
    assert(mock_contacts_count == 0 && mock_recents_count == 0 && mock_favorites_count == 0);
    g_variant_unref(contacts);
    g_variant_unref(recents);
}

static void incoming_contact_keeps_the_number_that_called(void)
{
    const MockContact contacts[] = {{ "Person", { "111", "222" }, { "Mobile", "Work" } }};
    mock_ui_set_data(contacts, 1, NULL, 0, NULL, 0);
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Person", "222");
    assert(model.caller_index == 0);
    assert(strcmp(model.dialed_number, "222") == 0);
    assert(strcmp(model.caller_name, "Person") == 0);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Person", "333");
    assert(strcmp(model.dialed_number, "333") == 0);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Person", "");
    assert(model.dialed_number[0] == '\0');
}

static void withheld_number_is_not_guessed(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Unknown caller", "");
    assert(model.caller_index == -1);
    assert(model.dialed_number[0] == '\0');
    assert(strcmp(model.caller_name, "Unknown caller") == 0);
}

static void device_details_come_from_the_daemon(void)
{
    MockUiModel model = live_model();
    live_apply_battery(&model, "iPhone battery level 3/5 (approximate)");
    assert(strcmp(model.battery, "Battery · about 60%") == 0);
    live_apply_battery(&model, "iPhone battery unavailable");
    assert(strcmp(model.battery, "Battery · Unavailable") == 0);
    live_apply_contacts_status(&model, "Last synced 2026-09-24 20:30");
    assert(strcmp(model.sync_label, "Last synced 2026-09-24 20:30") == 0);
    assert(!model.contact_sync_error);
    live_apply_contacts_status(&model, "Contact sync failed — allow Sync Contacts");
    assert(model.contact_sync_error);
}

static void two_calls_offer_swap_and_merge(void)
{
    MockUiModel model = live_model();
    GVariant *rows = g_variant_ref_sink(g_variant_new_parsed(
        "[('/active', 'active', 'laptop', 'A', '111', false),"
        " ('/held', 'held', 'laptop', 'B', '222', false)]"));
    live_apply_calls(&model, "ready", rows);
    assert(model.call_count == 2);
    assert(model.can_merge && model.can_resume && !model.can_add_call);
    assert(strcmp(model.call_id, "/active") == 0);
    assert(strcmp(model.held_call_id, "/held") == 0);
    assert(strcmp(model.dialed_number, "111") == 0);
    g_variant_unref(rows);
}

static void conference_has_explicit_end_control(void)
{
    MockUiModel model = live_model();
    GVariant *rows = g_variant_ref_sink(g_variant_new_parsed(
        "[('/one', 'active', 'laptop', 'A', '111', true),"
        " ('/two', 'active', 'laptop', 'B', '222', true)]"));
    live_apply_calls(&model, "ready", rows);
    assert(model.call_count == 2 && model.can_end_conference);
    assert(!model.can_hold && !model.can_resume && !model.can_merge && !model.can_add_call);
    g_variant_unref(rows);
}

static void held_call_keeps_controller_for_resume(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/one", "incoming", "A", "111");
    mock_ui_answer(&model);
    live_apply_snapshot(&model, "ready", "/one", "active", "A", "111");
    live_apply_snapshot(&model, "manage calls on phone", "", "unavailable", "", "");
    GVariant *rows = g_variant_ref_sink(g_variant_new_parsed(
        "[('/one', 'held', 'laptop', 'A', '111', false)]"));
    live_apply_calls(&model, "ready", rows);
    assert(model.call == MOCK_CALL_ACTIVE && model.popup_visible);
    assert(model.can_resume && strcmp(model.held_call_id, "/one") == 0);
    assert(model.call_held);
    unsigned elapsed = model.elapsed_seconds;
    mock_ui_tick(&model);
    assert(model.elapsed_seconds == elapsed);
    g_variant_unref(rows);
}

static void resumed_call_keeps_its_controller(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/one", "incoming", "A", "111");
    mock_ui_answer(&model);
    live_apply_snapshot(&model, "ready", "/one", "active", "A", "111");
    /* Resume: two snapshot replies (held, then resumed) can arrive before the call list. */
    live_apply_snapshot(&model, "manage calls on phone", "", "unavailable", "", "");
    live_apply_snapshot(&model, "ready", "/one", "active", "A", "111");
    GVariant *rows = g_variant_ref_sink(g_variant_new_parsed(
        "[('/one', 'active', 'laptop', 'A', '111', false)]"));
    live_apply_calls(&model, "ready", rows);
    assert(model.call == MOCK_CALL_ACTIVE && model.popup_visible);
    for (int i = 0; i < 10; i++) mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_ACTIVE && model.popup_visible);
    g_variant_unref(rows);
}

static void added_dialing_call_is_the_current_call(void)
{
    MockUiModel model = live_model();
    GVariant *rows = g_variant_ref_sink(g_variant_new_parsed(
        "[('/old', 'held', 'laptop', 'A', '111', false),"
        " ('/new', 'dialing', 'laptop', 'B', '222', false)]"));
    live_apply_calls(&model, "ready", rows);
    assert(strcmp(model.call_id, "/new") == 0);
    assert(strcmp(model.dialed_number, "222") == 0);
    g_variant_unref(rows);
}

static void voice_level_follows_loudness(void)
{
    gint16 samples[800];
    memset(samples, 0, sizeof samples);
    assert(live_voice_level(samples, G_N_ELEMENTS(samples)) == 0.0);
    for (size_t i = 0; i < G_N_ELEMENTS(samples); i++) samples[i] = i % 2 ? 16000 : -16000; /* Loud voice. */
    assert(live_voice_level(samples, G_N_ELEMENTS(samples)) > 0.9);
    for (size_t i = 0; i < G_N_ELEMENTS(samples); i++) samples[i] = i % 2 ? 20 : -20; /* Line hiss. */
    assert(live_voice_level(samples, G_N_ELEMENTS(samples)) == 0.0);
    for (size_t i = 0; i < G_N_ELEMENTS(samples); i++) samples[i] = i % 2 ? 1000 : -1000; /* Quiet talk. */
    double quiet = live_voice_level(samples, G_N_ELEMENTS(samples));
    assert(quiet > 0.3 && quiet < 0.9);
    assert(live_voice_level(NULL, 0) == 0.0);
}

int main(void)
{
    voice_level_follows_loudness();
    device_details_come_from_the_daemon();
    answered_here_becomes_the_controller();
    answered_on_phone_stays_off_screen();
    declined_shows_briefly_then_clears();
    outgoing_waits_for_the_phone();
    ignore_lasts_until_ringing_ends();
    status_maps_to_connection_states();
    incoming_snapshot_shows_the_caller();
    incoming_contact_keeps_the_number_that_called();
    withheld_number_is_not_guessed();
    two_calls_offer_swap_and_merge();
    conference_has_explicit_end_control();
    held_call_keeps_controller_for_resume();
    resumed_call_keeps_its_controller();
    added_dialing_call_is_the_current_call();
    daemon_lists_become_ui_data(); /* Last: it replaces the global data set. */
    return 0;
}
