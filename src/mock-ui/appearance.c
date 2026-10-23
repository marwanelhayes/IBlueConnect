#include "appearance.h"
#include <adwaita.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *const names[] = { "system", "light", "dark" };

MockAppearance appearance_parse(const char *name)
{
    for (int i = 0; name && i < 3; i++)
        if (strcmp(name, names[i]) == 0) return (MockAppearance)i;
    return MOCK_APPEARANCE_SYSTEM;
}

const char *appearance_name(MockAppearance appearance)
{
    return names[appearance <= MOCK_APPEARANCE_DARK ? appearance : MOCK_APPEARANCE_SYSTEM];
}

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "iblueconnect", "appearance.ini", NULL);
}

MockAppearance appearance_load(void)
{
    char *path = config_path();
    GKeyFile *file = g_key_file_new();
    char *name = g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)
        ? g_key_file_get_string(file, "ui", "appearance", NULL) : NULL;
    MockAppearance appearance = appearance_parse(name);
    g_free(name);
    g_key_file_unref(file);
    g_free(path);
    return appearance;
}

gboolean appearance_save(MockAppearance appearance, GError **error)
{
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    gboolean saved = g_mkdir_with_parents(dir, 0700) == 0 && g_chmod(dir, 0700) == 0;
    if (!saved)
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create %s", dir);
    else {
        GKeyFile *file = g_key_file_new();
        g_key_file_set_string(file, "ui", "appearance", appearance_name(appearance));
        saved = g_key_file_save_to_file(file, path, error);
        g_key_file_unref(file);
    }
    g_free(dir);
    g_free(path);
    return saved;
}

void appearance_apply(MockAppearance appearance)
{
    static const AdwColorScheme schemes[] = {
        ADW_COLOR_SCHEME_DEFAULT, ADW_COLOR_SCHEME_FORCE_LIGHT, ADW_COLOR_SCHEME_FORCE_DARK
    };
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
        schemes[appearance <= MOCK_APPEARANCE_DARK ? appearance : MOCK_APPEARANCE_SYSTEM]);
}
