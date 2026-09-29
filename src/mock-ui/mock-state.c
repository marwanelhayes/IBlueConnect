#include "mock-state.h"

#include <ctype.h>
#include <string.h>

static bool same_digits(const char *left, const char *right)
{
    for (;;) {
        while (*left && !isdigit((unsigned char)*left)) left++;
        while (*right && !isdigit((unsigned char)*right)) right++;
        if (!*left || !*right) return *left == *right;
        if (*left++ != *right++) return false;
    }
}

static const MockContact builtin_contacts[] = {
    { "Ahmed Mohamed", { "010 1234 5678", "02 1234 5678" }, { "Mobile", "Work" } },
    { "Mohamed Ali", { "012 3456 7890" }, { "Mobile" } },
    { "Sara Ahmed", { "011 9876 5432" }, { "Mobile" } },
    { "Dad", { "010 5555 1234" }, { "Mobile" } },
};

static const MockRecent builtin_recents[] = {
    { "Ahmed Mohamed", "010 1234 5678", "Incoming", "17:42", "12 min" },
    { "Dad", "010 5555 1234", "Missed", "16:21", NULL },
    { "Mohamed Ali", "012 3456 7890", "Outgoing", "14:03", "4 min" },
};

static const size_t builtin_favorites[] = { 0, 3, 1, 2 };

const MockContact *mock_contacts = builtin_contacts;
size_t mock_contacts_count = sizeof builtin_contacts / sizeof builtin_contacts[0];
const MockRecent *mock_recents = builtin_recents;
size_t mock_recents_count = sizeof builtin_recents / sizeof builtin_recents[0];
const size_t *mock_favorites = builtin_favorites;
size_t mock_favorites_count = sizeof builtin_favorites / sizeof builtin_favorites[0];

void mock_ui_set_data(const MockContact *contacts, size_t contacts_count,
                      const MockRecent *recents, size_t recents_count,
                      const size_t *favorites, size_t favorites_count)
{
    mock_contacts = contacts;
    mock_contacts_count = contacts_count;
    mock_recents = recents;
    mock_recents_count = recents_count;
    mock_favorites = favorites;
    mock_favorites_count = favorites_count;
}

int mock_ui_contact_named(const char *name)
{
    for (size_t i = 0; name && i < mock_contacts_count; i++)
        if (strcmp(mock_contacts[i].name, name) == 0) return (int)i;
    return -1;
}

const MockAudioDevice mock_audio_devices[] = {
    { "Sony WH-1000XM5", "Bluetooth Headset" },
    { "Built-in Audio", "Laptop Speakers" },
    { "Marwan's iPhone", "Continue call on iPhone" },
};
const size_t mock_audio_devices_count = sizeof mock_audio_devices / sizeof mock_audio_devices[0];

void mock_ui_init(MockUiModel *model)
{
    *model = (MockUiModel) {
        .connection = MOCK_CONNECTION_CONNECTED,
        .call = MOCK_CALL_IDLE,
        .caller_index = -1,
    };
    memcpy(model->battery, "Battery · 72%", sizeof "Battery · 72%");
    memcpy(model->sync_label, "Last contact sync · 2 minutes ago", sizeof "Last contact sync · 2 minutes ago");
}

void mock_ui_set_connection(MockUiModel *model, MockConnection connection)
{
    model->connection = connection;
    if (connection != MOCK_CONNECTION_CONNECTED)
        mock_ui_finish_call(model);
}

void mock_ui_incoming(MockUiModel *model, int contact_index)
{
    if (model->connection != MOCK_CONNECTION_CONNECTED || model->call != MOCK_CALL_IDLE)
        return;
    model->call = MOCK_CALL_INCOMING;
    model->caller_index = contact_index >= 0 && (size_t)contact_index < mock_contacts_count
        ? contact_index : -1;
    model->popup_visible = true;
    if (model->caller_index < 0)
        memcpy(model->dialed_number, "010 0000 0000", sizeof "010 0000 0000");
}

void mock_ui_ignore(MockUiModel *model)
{
    if (model->call != MOCK_CALL_INCOMING)
        return;
    model->call = MOCK_CALL_INCOMING_IGNORED;
    model->popup_visible = false;
}

void mock_ui_answer(MockUiModel *model)
{
    if (model->call != MOCK_CALL_INCOMING)
        return;
    model->call = MOCK_CALL_ACTIVE;
}

void mock_ui_decline(MockUiModel *model)
{
    if (model->call != MOCK_CALL_INCOMING)
        return;
    model->call = MOCK_CALL_DECLINED;
}

void mock_ui_outgoing(MockUiModel *model, int contact_index)
{
    if (model->connection != MOCK_CONNECTION_CONNECTED || model->calls_unavailable ||
        model->call != MOCK_CALL_IDLE)
        return;
    model->call = MOCK_CALL_OUTGOING;
    model->caller_index = contact_index >= 0 && (size_t)contact_index < mock_contacts_count
        ? contact_index : -1;
    model->popup_visible = true;
    model->elapsed_seconds = 0;
}

void mock_ui_dial_number(MockUiModel *model, const char *number)
{
    if (!number || !*number || strlen(number) >= sizeof model->dialed_number ||
        model->connection != MOCK_CONNECTION_CONNECTED || model->calls_unavailable ||
        model->call != MOCK_CALL_IDLE)
        return;
    mock_ui_outgoing(model, -1);
    memcpy(model->dialed_number, number, strlen(number) + 1);
    for (size_t i = 0; i < mock_contacts_count; i++) {
        for (size_t n = 0; n < MOCK_CONTACT_NUMBERS && mock_contacts[i].numbers[n]; n++)
            if (same_digits(number, mock_contacts[i].numbers[n])) {
                model->caller_index = (int)i;
                return;
            }
    }
}

void mock_ui_connect_call(MockUiModel *model)
{
    if (model->call == MOCK_CALL_OUTGOING || model->call == MOCK_CALL_CONNECTING)
        model->call = MOCK_CALL_ACTIVE;
}

void mock_ui_tick(MockUiModel *model)
{
    if (model->live && (model->call == MOCK_CALL_OUTGOING || model->call == MOCK_CALL_CONNECTING))
        return; /* The phone reports when the call connects. */
    if (model->call == MOCK_CALL_OUTGOING)
        model->call = MOCK_CALL_CONNECTING;
    else if (model->call == MOCK_CALL_CONNECTING)
        model->call = MOCK_CALL_ACTIVE;
    else if (model->call == MOCK_CALL_ACTIVE && !model->call_held)
        model->elapsed_seconds++;
    else if (model->call == MOCK_CALL_ENDING)
        model->call = MOCK_CALL_ENDED;
    else if (model->call == MOCK_CALL_ENDED || model->call == MOCK_CALL_DECLINED)
        mock_ui_finish_call(model);
}

void mock_ui_end_call(MockUiModel *model)
{
    if (model->call == MOCK_CALL_ACTIVE || model->call == MOCK_CALL_OUTGOING ||
        model->call == MOCK_CALL_CONNECTING)
        model->call = MOCK_CALL_ENDING;
    else if (model->call == MOCK_CALL_INCOMING || model->call == MOCK_CALL_INCOMING_IGNORED)
        mock_ui_finish_call(model);
}

void mock_ui_finish_call(MockUiModel *model)
{
    model->call = MOCK_CALL_IDLE;
    model->popup_visible = false;
    model->muted = false;
    model->can_hold = model->can_resume = model->can_merge = false;
    model->can_add_call = model->can_end_conference = model->call_held = false;
    model->call_count = 0;
    model->held_call_id[0] = '\0';
    model->compact = false;
    model->caller_index = -1;
    model->dialed_number[0] = '\0';
    model->caller_name[0] = '\0';
    model->elapsed_seconds = 0;
    model->call_id[0] = '\0';
}

void mock_ui_toggle_mute(MockUiModel *model)
{
    if (model->call == MOCK_CALL_ACTIVE)
        model->muted = !model->muted;
}

void mock_ui_set_compact(MockUiModel *model, bool compact)
{
    if (model->call == MOCK_CALL_ACTIVE)
        model->compact = compact;
}

void mock_ui_select_audio(MockUiModel *model, size_t index)
{
    if (model->call == MOCK_CALL_ACTIVE && index < mock_audio_devices_count)
        model->audio_index = index;
}
