#ifndef IBLUECONNECT_MOCK_CALL_POPUP_H
#define IBLUECONNECT_MOCK_CALL_POPUP_H

#include <adwaita.h>
#include "mock-state.h"

typedef struct MockCallPopup MockCallPopup;
typedef void (*MockCallPopupAction)(const char *action, const char *argument, gpointer user_data);

MockCallPopup *mock_call_popup_new(GtkApplication *app, MockCallPopupAction action, gpointer user_data);
void mock_call_popup_update(MockCallPopup *popup, const MockUiModel *model);
/* Level 0..1 of the other person's voice; drives the receiving bar and speaking highlight. */
void mock_call_popup_set_voice(MockCallPopup *popup, double level);
/* Level 0..1 on the outgoing call path; -1 clears immediately when unavailable.
 * Muted/held/unavailable audio stays flat. */
void mock_call_popup_set_microphone_level(MockCallPopup *popup, double level);
GtkWindow *mock_call_popup_window(MockCallPopup *popup);

#endif
