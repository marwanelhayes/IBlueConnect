#include "mock-state.h"

#include <ctype.h>
#include <stdio.h>
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
    model->call_count = 0;
    model->waiting_call_id[0] = model->ignored_waiting_id[0] = '\0';
    mock_ui_derive_calls(model);
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

#define COPY(field, text) snprintf(field, sizeof field, "%s", (text) ? (text) : "")

void mock_ui_add_line(MockUiModel *model, const char *id, const char *caller, const char *number,
                      MockLinePhase phase, bool multiparty)
{
    if (model->call_count >= MOCK_LINES) return;
    MockLine *line = &model->lines[model->call_count++];
    COPY(line->id, id);
    COPY(line->caller, caller);
    COPY(line->number, number);
    line->phase = phase;
    line->multiparty = multiparty;
}

void mock_ui_derive_calls(MockUiModel *model)
{
    unsigned n = model->call_count, active = 0, held = 0, waiting = 0, ringing = 0;
    unsigned group = 0, active_group = 0, held_group = 0;
    const MockLine *primary = NULL, *wait = NULL;
    int best = -1;
    model->held_call_id[0] = '\0';
    for (unsigned i = 0; i < n; i++) {
        const MockLine *line = &model->lines[i];
        active += line->phase == MOCK_LINE_ACTIVE;
        held += line->phase == MOCK_LINE_HELD;
        ringing += line->phase == MOCK_LINE_DIALING || line->phase == MOCK_LINE_INCOMING;
        group += line->multiparty;
        active_group += line->multiparty && line->phase == MOCK_LINE_ACTIVE;
        held_group += line->multiparty && line->phase == MOCK_LINE_HELD;
        if (line->phase == MOCK_LINE_HELD) COPY(model->held_call_id, line->id);
        if (line->phase == MOCK_LINE_WAITING) { waiting++; wait = line; }
        int priority = line->phase == MOCK_LINE_ACTIVE ? 3 : line->phase == MOCK_LINE_DIALING ? 2 :
                       line->phase == MOCK_LINE_HELD ? 1 : 0;
        if (priority > best) { best = priority; primary = line; }
    }
    bool split = active && held && active + held == n;
    model->can_hold = n == 1 && active == 1 && !group;
    model->can_resume = (n == 1 && held == 1) || (n == 2 && active == 1 && held == 1 && !group);
    /* Each side is one plain call or only conference members. */
    model->can_swap = split && (active == 1 || active_group == active) && (held == 1 || held_group == held);
    model->can_merge = split && n <= 5;
    model->can_add_call = n && active == n && n < 5 && (n == 1 || group == n);
    /* Calls beside an active conference must all be held. */
    model->can_end_conference = group >= 2 && active_group == group && held - held_group == n - group;
    model->can_private = model->can_end_conference && group == n;
    if (waiting != 1) wait = NULL;
    model->can_hold_and_answer = wait && !held && !ringing;
    model->can_release_and_answer = wait && !ringing;
    bool fresh = wait && strcmp(model->waiting_call_id, wait->id) != 0;
    COPY(model->waiting_call_id, wait ? wait->id : "");
    model->waiting_visible = wait && strcmp(model->ignored_waiting_id, wait->id) != 0;
    if (fresh && model->waiting_visible) model->popup_visible = true; /* Raise a dismissed controller. */
    model->call_held = n == 1 && held == 1;
    bool several = n > 1 || held == 1;
    if (primary && (several || (model->call == MOCK_CALL_ACTIVE && primary->phase == MOCK_LINE_ACTIVE))) {
        COPY(model->call_id, primary->id);
        COPY(model->caller_name, primary->caller);
        COPY(model->dialed_number, primary->number);
        model->caller_index = mock_ui_contact_named(primary->caller);
        if (several) model->call = primary->phase == MOCK_LINE_DIALING ? MOCK_CALL_OUTGOING : MOCK_CALL_ACTIVE;
    }
}

void mock_ui_ignore_waiting(MockUiModel *model)
{
    COPY(model->ignored_waiting_id, model->waiting_call_id);
    model->waiting_visible = false;
}

static void add_contact_line(MockUiModel *model, size_t contact, MockLinePhase phase, bool multiparty)
{
    char id[16];
    snprintf(id, sizeof id, "mock-%zu", contact);
    bool known = contact < mock_contacts_count;
    mock_ui_add_line(model, id, known ? mock_contacts[contact].name : "Unknown caller",
                     known ? mock_contacts[contact].numbers[0] : "", phase, multiparty);
}

void mock_ui_simulate_calls(MockUiModel *model, const char *scenario)
{
    mock_ui_finish_call(model);
    mock_ui_incoming(model, 0);
    mock_ui_answer(model);
    if (model->call != MOCK_CALL_ACTIVE) return;
    bool conference = strcmp(scenario, "conference") == 0;
    add_contact_line(model, 0, MOCK_LINE_ACTIVE, conference);
    add_contact_line(model, 1, strcmp(scenario, "waiting") == 0 ? MOCK_LINE_WAITING :
                     conference ? MOCK_LINE_ACTIVE : MOCK_LINE_HELD, conference);
    if (conference) add_contact_line(model, 2, MOCK_LINE_ACTIVE, true);
    mock_ui_derive_calls(model);
}

static int line_index(const MockUiModel *model, const char *id)
{
    for (unsigned i = 0; id && *id && i < model->call_count; i++)
        if (strcmp(model->lines[i].id, id) == 0) return (int)i;
    return -1;
}

static void remove_line(MockUiModel *model, unsigned index)
{
    memmove(&model->lines[index], &model->lines[index + 1],
            (model->call_count - index - 1) * sizeof model->lines[0]);
    model->call_count--;
}

/* Moves every active call aside (held, or ended) and makes the waiting call active. */
static void answer_waiting(MockUiModel *model, int at, bool release)
{
    char id[sizeof model->lines[0].id];
    COPY(id, model->lines[at].id);
    for (unsigned i = model->call_count; i-- > 0;)
        if (model->lines[i].phase == MOCK_LINE_ACTIVE) {
            if (release) remove_line(model, i);
            else model->lines[i].phase = MOCK_LINE_HELD;
        }
    model->lines[line_index(model, id)].phase = MOCK_LINE_ACTIVE;
}

bool mock_ui_calls_action(MockUiModel *model, const char *action, const char *id)
{
    if (model->live || !model->call_count) return false;
    int at = line_index(model, id);
    bool waiting = at >= 0 && model->lines[at].phase == MOCK_LINE_WAITING;
    if (strcmp(action, "swap") == 0 && model->can_swap) {
        for (unsigned i = 0; i < model->call_count; i++)
            model->lines[i].phase = model->lines[i].phase == MOCK_LINE_ACTIVE ? MOCK_LINE_HELD : MOCK_LINE_ACTIVE;
    } else if (strcmp(action, "merge") == 0 && model->can_merge) {
        for (unsigned i = 0; i < model->call_count; i++) {
            model->lines[i].phase = MOCK_LINE_ACTIVE;
            model->lines[i].multiparty = true;
        }
    } else if (strcmp(action, "add") == 0 && model->can_add_call && id && *id) {
        for (unsigned i = 0; i < model->call_count; i++) model->lines[i].phase = MOCK_LINE_HELD;
        const char *caller = id;
        for (size_t i = 0; i < mock_contacts_count && caller == id; i++)
            for (size_t k = 0; k < MOCK_CONTACT_NUMBERS && mock_contacts[i].numbers[k]; k++)
                if (same_digits(id, mock_contacts[i].numbers[k])) caller = mock_contacts[i].name;
        static unsigned added;
        char line_id[32];
        snprintf(line_id, sizeof line_id, "mock-added-%u", ++added);
        mock_ui_add_line(model, line_id, caller, id, MOCK_LINE_ACTIVE, false);
    } else if (strcmp(action, "end-line") == 0 && at >= 0) {
        remove_line(model, (unsigned)at);
    } else if (strcmp(action, "end") == 0 && model->call_count > 1 && line_index(model, model->call_id) >= 0) {
        remove_line(model, (unsigned)line_index(model, model->call_id));
    } else if (strcmp(action, "end-conference") == 0 && model->can_end_conference) {
        for (unsigned i = model->call_count; i-- > 0;)
            if (model->lines[i].multiparty) remove_line(model, i);
    } else if (strcmp(action, "private") == 0 && model->can_private && at >= 0) {
        for (unsigned i = 0; i < model->call_count; i++)
            model->lines[i].phase = (int)i == at ? MOCK_LINE_ACTIVE : MOCK_LINE_HELD;
        model->lines[at].multiparty = false;
    } else if (strcmp(action, "hold-and-answer") == 0 && model->can_hold_and_answer && waiting) {
        answer_waiting(model, at, false);
    } else if (strcmp(action, "release-and-answer") == 0 && model->can_release_and_answer && waiting) {
        answer_waiting(model, at, true);
    } else return false;
    unsigned members = 0;
    for (unsigned i = 0; i < model->call_count; i++) members += model->lines[i].multiparty;
    for (unsigned i = 0; members < 2 && i < model->call_count; i++) model->lines[i].multiparty = false;
    if (model->call_count == 1 && model->lines[0].phase == MOCK_LINE_WAITING) {
        int contact = mock_ui_contact_named(model->lines[0].caller); /* Last call ended: it rings normally. */
        mock_ui_finish_call(model);
        mock_ui_incoming(model, contact);
        return true;
    }
    mock_ui_derive_calls(model);
    if (!model->call_count) mock_ui_end_call(model);
    return true;
}
