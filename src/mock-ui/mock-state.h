#ifndef IBLUECONNECT_MOCK_STATE_H
#define IBLUECONNECT_MOCK_STATE_H

#include <stdbool.h>
#include <stddef.h>

/* The user's light/dark choice; System follows GNOME. */
typedef enum {
    MOCK_APPEARANCE_SYSTEM,
    MOCK_APPEARANCE_LIGHT,
    MOCK_APPEARANCE_DARK
} MockAppearance;

typedef enum {
    MOCK_CONNECTION_DISCONNECTED,
    MOCK_CONNECTION_CONNECTING,
    MOCK_CONNECTION_CONNECTED,
    MOCK_CONNECTION_ERROR
} MockConnection;

typedef enum {
    MOCK_CALL_IDLE,
    MOCK_CALL_INCOMING,
    MOCK_CALL_INCOMING_IGNORED,
    MOCK_CALL_DECLINED,
    MOCK_CALL_OUTGOING,
    MOCK_CALL_CONNECTING,
    MOCK_CALL_ACTIVE,
    MOCK_CALL_ENDING,
    MOCK_CALL_ENDED
} MockCall;

/* ponytail: fixed cap per contact; extra numbers are dropped. Use a dynamic list if contacts need more. */
#define MOCK_CONTACT_NUMBERS 4

typedef struct {
    const char *name;
    const char *numbers[MOCK_CONTACT_NUMBERS]; /* numbers[0] is the primary number. */
    const char *labels[MOCK_CONTACT_NUMBERS];
} MockContact;

typedef struct {
    const char *name; /* NULL when the caller is not a contact. */
    const char *number;
    const char *kind;
    const char *time;
    const char *duration;
} MockRecent;

typedef struct {
    const char *name;
    const char *description;
} MockAudioDevice;

typedef enum {
    MOCK_LINE_ACTIVE,
    MOCK_LINE_HELD,
    MOCK_LINE_DIALING, /* Dialing or alerting. */
    MOCK_LINE_WAITING,
    MOCK_LINE_INCOMING,
    MOCK_LINE_OTHER
} MockLinePhase;

/* ponytail: fixed cap of the phone's five calls plus one waiting; extra rows are dropped. */
#define MOCK_LINES 6

typedef struct {
    char id[128];
    char caller[128];
    char number[64];
    MockLinePhase phase;
    bool multiparty;
} MockLine;

typedef struct {
    MockConnection connection;
    MockCall call;
    bool popup_visible;
    bool compact;
    bool muted;
    bool can_hold;
    bool can_resume;
    bool can_merge;
    bool can_add_call;
    bool can_end_conference;
    bool can_swap;
    bool can_private; /* Any conference member may be split off. */
    bool can_hold_and_answer;
    bool can_release_and_answer;
    bool waiting_visible; /* A waiting call the user has not ignored. */
    bool call_held;
    unsigned call_count; /* Rows in lines[]. */
    MockLine lines[MOCK_LINES];
    unsigned elapsed_seconds;
    size_t audio_index;
    int caller_index; /* -1 means unknown caller. */
    char dialed_number[64]; /* Used when dialing a number without a contact match. */
    char caller_name[128]; /* Live SnapshotDetails label, retained across contact reloads. */
    char call_id[128]; /* Daemon call id in live mode. */
    char held_call_id[128];
    char waiting_call_id[128];
    char ignored_waiting_id[128];
    char battery[48];
    char sync_label[160];
    bool call_audio; /* Live: the daemon found this call's audio on the laptop, so Mute works. */
    bool live; /* Call phases come from the daemon; ticks only advance timers and banners. */
    bool contact_sync_error;
    bool calls_unavailable;
    bool empty_contacts;
    bool empty_recents;
    MockAppearance appearance;
    bool empty_favorites;
    bool empty_audio_devices;
} MockUiModel;

/* The active data set: built-in mock data by default, replaced by mock_ui_set_data(). */
extern const MockContact *mock_contacts;
extern size_t mock_contacts_count;
extern const MockRecent *mock_recents;
extern size_t mock_recents_count;
/* Bumped whenever the lists change, so views rebuild only then. */
extern unsigned mock_data_version;
extern const size_t *mock_favorites;
extern size_t mock_favorites_count;
extern const MockAudioDevice mock_audio_devices[];
extern const size_t mock_audio_devices_count;

void mock_ui_init(MockUiModel *model);
void mock_ui_set_data(const MockContact *contacts, size_t contacts_count,
                      const MockRecent *recents, size_t recents_count,
                      const size_t *favorites, size_t favorites_count);
int mock_ui_contact_named(const char *name);
void mock_ui_set_connection(MockUiModel *model, MockConnection connection);
void mock_ui_incoming(MockUiModel *model, int contact_index);
void mock_ui_outgoing(MockUiModel *model, int contact_index);
void mock_ui_dial_number(MockUiModel *model, const char *number);
void mock_ui_ignore(MockUiModel *model);
void mock_ui_decline(MockUiModel *model);
void mock_ui_answer(MockUiModel *model);
void mock_ui_connect_call(MockUiModel *model);
void mock_ui_end_call(MockUiModel *model);
void mock_ui_finish_call(MockUiModel *model);
void mock_ui_tick(MockUiModel *model);
void mock_ui_toggle_mute(MockUiModel *model);
void mock_ui_set_compact(MockUiModel *model, bool compact);
void mock_ui_select_audio(MockUiModel *model, size_t index);

/* Per-call rows: append, then derive eligibility and the call the controller shows. */
void mock_ui_add_line(MockUiModel *model, const char *id, const char *caller, const char *number,
                      MockLinePhase phase, bool multiparty);
void mock_ui_derive_calls(MockUiModel *model);
void mock_ui_ignore_waiting(MockUiModel *model);
/* Mock only: "waiting", "two-calls" or "conference" around an answered call. */
void mock_ui_simulate_calls(MockUiModel *model, const char *scenario);
/* Mock only: swap, merge, add (number), end, end-line, end-conference, private, hold-and-answer,
 * release-and-answer (id). Returns false when the action does not apply. */
bool mock_ui_calls_action(MockUiModel *model, const char *action, const char *id);

#endif
