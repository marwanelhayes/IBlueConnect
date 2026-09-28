#ifndef IBLUECONNECT_MOCK_STATE_H
#define IBLUECONNECT_MOCK_STATE_H

#include <stdbool.h>
#include <stddef.h>

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

typedef struct {
    const char *name;
    const char *mobile;
    const char *work;
} MockContact;

typedef struct {
    size_t contact_index;
    const char *kind;
    const char *time;
    const char *duration;
} MockRecent;

typedef struct {
    const char *name;
    const char *description;
} MockAudioDevice;

typedef struct {
    MockConnection connection;
    MockCall call;
    bool popup_visible;
    bool compact;
    bool muted;
    unsigned elapsed_seconds;
    size_t audio_index;
    int caller_index; /* -1 means unknown caller. */
    char dialed_number[64]; /* Used when dialing a number without a contact match. */
    bool contact_sync_error;
    bool calls_unavailable;
    bool empty_contacts;
    bool empty_recents;
    bool empty_favorites;
    bool empty_audio_devices;
} MockUiModel;

extern const MockContact mock_contacts[];
extern const size_t mock_contacts_count;
extern const MockRecent mock_recents[];
extern const size_t mock_recents_count;
extern const MockAudioDevice mock_audio_devices[];
extern const size_t mock_audio_devices_count;

void mock_ui_init(MockUiModel *model);
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

#endif
