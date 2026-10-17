#include "mock-state.h"

#include <assert.h>
#include <string.h>

static void incoming_can_be_ignored_without_declining(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    assert(model.call == MOCK_CALL_INCOMING);
    assert(model.popup_visible);
    mock_ui_ignore(&model);
    assert(model.call == MOCK_CALL_INCOMING_IGNORED);
    assert(!model.popup_visible);
}

static void answering_keeps_the_same_popup(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    mock_ui_answer(&model);
    assert(model.call == MOCK_CALL_ACTIVE);
    assert(model.popup_visible);
    assert(model.elapsed_seconds == 0);
}

static void declining_dismisses_incoming_call(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    mock_ui_decline(&model);
    assert(model.call == MOCK_CALL_DECLINED);
    assert(model.popup_visible); /* Briefly show the declined confirmation. */
}

static void outgoing_call_waits_for_mock_connection(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_outgoing(&model, 1);
    assert(model.call == MOCK_CALL_OUTGOING);
    assert(model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_CONNECTING);
    mock_ui_connect_call(&model);
    assert(model.call == MOCK_CALL_ACTIVE);
    mock_ui_tick(&model);
    assert(model.elapsed_seconds == 1);
}

static void ending_shows_confirmation_then_resets(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    mock_ui_answer(&model);
    mock_ui_end_call(&model);
    assert(model.call == MOCK_CALL_ENDING);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_ENDED);
    assert(model.popup_visible);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_IDLE);
    assert(!model.popup_visible);
}

static void disconnect_clears_the_call_without_phone_actions(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    mock_ui_set_connection(&model, MOCK_CONNECTION_DISCONNECTED);
    assert(model.connection == MOCK_CONNECTION_DISCONNECTED);
    assert(model.call == MOCK_CALL_IDLE);
    assert(!model.popup_visible);
    mock_ui_set_connection(&model, MOCK_CONNECTION_CONNECTING);
    assert(model.connection == MOCK_CONNECTION_CONNECTING);
    mock_ui_set_connection(&model, MOCK_CONNECTION_ERROR);
    assert(model.connection == MOCK_CONNECTION_ERROR);
}

static void outgoing_call_connects_after_two_ticks(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_outgoing(&model, 0);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_CONNECTING);
    mock_ui_tick(&model);
    assert(model.call == MOCK_CALL_ACTIVE);
    assert(model.elapsed_seconds == 0);
}

static void active_controls_change_only_the_mock_ui(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 0);
    mock_ui_answer(&model);
    mock_ui_toggle_mute(&model);
    assert(model.muted);
    mock_ui_select_audio(&model, 2);
    assert(model.audio_index == 2);
    mock_ui_set_compact(&model, true);
    assert(model.compact);
}

static void keypad_can_call_an_unmatched_number(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_dial_number(&model, "12345");
    assert(model.call == MOCK_CALL_OUTGOING);
    assert(model.caller_index == -1);
    assert(strcmp(model.dialed_number, "12345") == 0);
}

static void dialed_number_resolves_a_mock_contact(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_dial_number(&model, "01012345678");
    assert(model.caller_index == 0);
}

static void unknown_incoming_shows_a_mock_number(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, -1);
    assert(model.caller_index == -1);
    assert(strcmp(model.dialed_number, "010 0000 0000") == 0);
    mock_ui_finish_call(&model);
    assert(model.dialed_number[0] == '\0');
}

static void invalid_contact_index_cannot_escape_mock_data(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_incoming(&model, 99);
    assert(model.caller_index == -1);
}

static void calls_unavailable_prevents_mock_outgoing_calls(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    model.calls_unavailable = true;
    mock_ui_outgoing(&model, 0);
    assert(model.call == MOCK_CALL_IDLE);
    mock_ui_dial_number(&model, "12345");
    assert(model.call == MOCK_CALL_IDLE);
}

static void mock_waiting_call_can_be_held_and_answered(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_simulate_calls(&model, "waiting");
    assert(model.call == MOCK_CALL_ACTIVE && model.popup_visible && model.waiting_visible);
    assert(model.call_count == 2 && model.lines[1].phase == MOCK_LINE_WAITING);
    assert(strcmp(model.waiting_call_id, model.lines[1].id) == 0);
    char waiting[128];
    strcpy(waiting, model.waiting_call_id);
    mock_ui_ignore_waiting(&model);
    assert(!model.waiting_visible && model.popup_visible);
    assert(mock_ui_calls_action(&model, "hold-and-answer", waiting));
    assert(model.lines[0].phase == MOCK_LINE_HELD && model.lines[1].phase == MOCK_LINE_ACTIVE);
    assert(strcmp(model.call_id, waiting) == 0 && model.caller_index == 1);
    assert(model.can_swap && model.can_merge && !model.waiting_call_id[0]);
    assert(mock_ui_calls_action(&model, "swap", ""));
    assert(model.lines[0].phase == MOCK_LINE_ACTIVE && model.caller_index == 0);
}

static void mock_waiting_call_can_replace_the_active_call(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_simulate_calls(&model, "waiting");
    char waiting[128];
    strcpy(waiting, model.waiting_call_id);
    assert(mock_ui_calls_action(&model, "release-and-answer", waiting));
    assert(model.call_count == 1 && model.call == MOCK_CALL_ACTIVE && model.caller_index == 1);
    assert(model.can_hold && model.can_add_call);
    mock_ui_simulate_calls(&model, "waiting");
    assert(mock_ui_calls_action(&model, "end-line", model.waiting_call_id)); /* Decline. */
    assert(model.call_count == 1 && !model.waiting_visible && model.call == MOCK_CALL_ACTIVE);
    mock_ui_simulate_calls(&model, "waiting");
    assert(mock_ui_calls_action(&model, "end", ""));
    assert(model.call == MOCK_CALL_INCOMING && model.caller_index == 1 && model.call_count == 0);
}

static void mock_conference_merges_splits_and_ends(void)
{
    MockUiModel model;
    mock_ui_init(&model);
    mock_ui_simulate_calls(&model, "two-calls");
    assert(model.lines[0].phase == MOCK_LINE_ACTIVE && model.lines[1].phase == MOCK_LINE_HELD);
    assert(mock_ui_calls_action(&model, "merge", ""));
    assert(model.can_end_conference && model.can_private && model.can_add_call);
    assert(mock_ui_calls_action(&model, "add", "555 0100 0000"));
    assert(model.call_count == 3 && model.lines[2].phase == MOCK_LINE_ACTIVE && !model.lines[2].multiparty);
    assert(model.lines[0].phase == MOCK_LINE_HELD && model.can_swap);
    mock_ui_simulate_calls(&model, "conference");
    assert(model.call_count == 3 && model.can_private);
    char member[128];
    strcpy(member, model.lines[1].id);
    assert(mock_ui_calls_action(&model, "private", member));
    assert(model.lines[1].phase == MOCK_LINE_ACTIVE && !model.lines[1].multiparty);
    assert(model.lines[0].phase == MOCK_LINE_HELD && model.lines[0].multiparty);
    assert(mock_ui_calls_action(&model, "swap", ""));
    assert(model.can_end_conference);
    assert(mock_ui_calls_action(&model, "end-conference", ""));
    assert(model.call_count == 1 && model.lines[0].phase == MOCK_LINE_HELD && model.call_held);
    assert(mock_ui_calls_action(&model, "end-line", model.lines[0].id));
    assert(model.call == MOCK_CALL_ENDING);
    assert(!mock_ui_calls_action(&model, "swap", ""));
}

int main(void)
{
    incoming_can_be_ignored_without_declining();
    answering_keeps_the_same_popup();
    declining_dismisses_incoming_call();
    outgoing_call_waits_for_mock_connection();
    ending_shows_confirmation_then_resets();
    disconnect_clears_the_call_without_phone_actions();
    outgoing_call_connects_after_two_ticks();
    active_controls_change_only_the_mock_ui();
    keypad_can_call_an_unmatched_number();
    dialed_number_resolves_a_mock_contact();
    unknown_incoming_shows_a_mock_number();
    invalid_contact_index_cannot_escape_mock_data();
    calls_unavailable_prevents_mock_outgoing_calls();
    mock_waiting_call_can_be_held_and_answered();
    mock_waiting_call_can_replace_the_active_call();
    mock_conference_merges_splits_and_ends();
    return 0;
}
