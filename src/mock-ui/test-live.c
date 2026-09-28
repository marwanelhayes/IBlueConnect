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
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Sara Ahmed");
    assert(model.connection == MOCK_CONNECTION_CONNECTED);
    assert(model.call == MOCK_CALL_INCOMING);
    assert(model.popup_visible);
    assert(model.caller_index == 2);
    assert(strcmp(model.call_id, "/call1") == 0);
}

static void status_maps_to_connection_states(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "manage calls on phone", "", "unavailable", "");
    assert(model.connection == MOCK_CONNECTION_CONNECTED);
    assert(model.calls_unavailable);
    live_apply_snapshot(&model, "ready", "", "idle", "");
    assert(!model.calls_unavailable);
    live_apply_snapshot(&model, "calling unavailable", "", "idle", "");
    assert(model.connection == MOCK_CONNECTION_DISCONNECTED);
    live_apply_snapshot(&model, NULL, "", "idle", "");
    assert(model.connection == MOCK_CONNECTION_ERROR);
}

static void ignore_lasts_until_ringing_ends(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    mock_ui_ignore(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    assert(model.call == MOCK_CALL_INCOMING_IGNORED);
    assert(!model.popup_visible);
    live_apply_snapshot(&model, "ready", "", "idle", "");
    assert(model.call == MOCK_CALL_IDLE);
    assert(!model.popup_visible);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    assert(model.call == MOCK_CALL_INCOMING);
    assert(model.popup_visible);
}

static void answered_here_becomes_the_controller(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    mock_ui_answer(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    assert(model.call == MOCK_CALL_ACTIVE);
    live_apply_snapshot(&model, "ready", "/call1", "active", "Dad");
    assert(model.popup_visible);
    mock_ui_tick(&model);
    mock_ui_tick(&model);
    assert(model.elapsed_seconds == 2);
    live_apply_snapshot(&model, "ready", "", "idle", "");
    assert(model.call == MOCK_CALL_ENDED && model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_IDLE && !model.popup_visible);
}

static void answered_on_phone_stays_off_screen(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "Dad");
    live_apply_snapshot(&model, "ready", "/call1", "active", "Dad");
    assert(model.call == MOCK_CALL_ACTIVE && !model.popup_visible);
    live_apply_snapshot(&model, "ready", "", "idle", "");
    assert(model.call == MOCK_CALL_IDLE);
}

static void declined_shows_briefly_then_clears(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "0100");
    assert(model.caller_index == -1);
    assert(strcmp(model.dialed_number, "0100") == 0);
    mock_ui_decline(&model);
    live_apply_snapshot(&model, "ready", "/call1", "incoming", "0100");
    live_apply_snapshot(&model, "ready", "", "idle", "");
    assert(model.call == MOCK_CALL_DECLINED && model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_IDLE);
}

static void outgoing_waits_for_the_phone(void)
{
    MockUiModel model = live_model();
    live_apply_snapshot(&model, "ready", "/call2", "dialing", "Sara Ahmed");
    assert(model.call == MOCK_CALL_OUTGOING && model.popup_visible);
    mock_ui_tick(&model);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_OUTGOING);
    live_apply_snapshot(&model, "ready", "/call2", "alerting", "Sara Ahmed");
    live_apply_snapshot(&model, "ready", "/call2", "active", "Sara Ahmed");
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

int main(void)
{
    device_details_come_from_the_daemon();
    answered_here_becomes_the_controller();
    answered_on_phone_stays_off_screen();
    declined_shows_briefly_then_clears();
    outgoing_waits_for_the_phone();
    ignore_lasts_until_ringing_ends();
    status_maps_to_connection_states();
    incoming_snapshot_shows_the_caller();
    daemon_lists_become_ui_data(); /* Last: it replaces the global data set. */
    return 0;
}
