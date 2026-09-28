#ifndef IBLUECONNECT_MOCK_CALL_POPUP_H
#define IBLUECONNECT_MOCK_CALL_POPUP_H

#include <adwaita.h>
#include "mock-state.h"

typedef struct MockCallPopup MockCallPopup;
typedef void (*MockCallPopupAction)(const char *action, const char *argument, gpointer user_data);

MockCallPopup *mock_call_popup_new(GtkApplication *app, MockCallPopupAction action, gpointer user_data);
void mock_call_popup_update(MockCallPopup *popup, const MockUiModel *model);
GtkWindow *mock_call_popup_window(MockCallPopup *popup);

#endif
