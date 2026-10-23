#ifndef IBLUECONNECT_APPEARANCE_H
#define IBLUECONNECT_APPEARANCE_H

#include <glib.h>
#include "mock-state.h"

/* Persists the System/Light/Dark choice in ~/.config/iblueconnect/appearance.ini and applies it. */
MockAppearance appearance_parse(const char *name);
const char *appearance_name(MockAppearance appearance);
MockAppearance appearance_load(void);
gboolean appearance_save(MockAppearance appearance, GError **error);
/* Applies to every window of this process (main window and call controller alike). */
void appearance_apply(MockAppearance appearance);

#endif
