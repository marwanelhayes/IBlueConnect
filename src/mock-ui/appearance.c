#include "appearance.h"
#include <adwaita.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *const names[] = { "system", "light", "dark", "black" };
#define APPEARANCES G_N_ELEMENTS(names)

MockAppearance appearance_parse(const char *name)
{
    for (guint i = 0; name && i < APPEARANCES; i++)
        if (strcmp(name, names[i]) == 0) return (MockAppearance)i;
    return MOCK_APPEARANCE_SYSTEM;
}

const char *appearance_name(MockAppearance appearance)
{
    return names[(guint)appearance < APPEARANCES ? appearance : MOCK_APPEARANCE_SYSTEM];
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

/* Near-black surfaces on top of GNOME Dark; text, accent and button colours stay GNOME's. */
static GtkCssProvider *black_surfaces(void)
{
    static const char *const surfaces[][2] = {
        { "window-bg-color", "#000000" }, { "view-bg-color", "#000000" },
        { "headerbar-bg-color", "#0a0a0a" }, { "headerbar-backdrop-color", "#0a0a0a" },
        { "sidebar-bg-color", "#0a0a0a" }, { "dialog-bg-color", "#0a0a0a" },
        { "card-bg-color", "#141414" }, { "popover-bg-color", "#141414" },
    };
    GString *css = g_string_new(NULL);
    /* libadwaita 1.6+ styles through CSS variables; 1.5 (Ubuntu 24.04) through named colours. */
    gboolean variables = adw_get_major_version() > 1 || adw_get_minor_version() >= 6;
    if (variables) g_string_append(css, ":root {");
    for (guint i = 0; i < G_N_ELEMENTS(surfaces); i++) {
        if (variables) g_string_append_printf(css, " --%s: %s;", surfaces[i][0], surfaces[i][1]);
        else {
            char *name = g_strdelimit(g_strdup(surfaces[i][0]), "-", '_');
            g_string_append_printf(css, "@define-color %s %s; ", name, surfaces[i][1]);
            g_free(name);
        }
    }
    if (variables) g_string_append(css, " }");
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css->str);
    g_string_free(css, TRUE);
    return provider;
}

void appearance_apply(MockAppearance appearance)
{
    static const AdwColorScheme schemes[] = {
        ADW_COLOR_SCHEME_DEFAULT, ADW_COLOR_SCHEME_FORCE_LIGHT, ADW_COLOR_SCHEME_FORCE_DARK, ADW_COLOR_SCHEME_FORCE_DARK
    };
    static GtkCssProvider *black;
    if ((guint)appearance >= APPEARANCES) appearance = MOCK_APPEARANCE_SYSTEM;
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), schemes[appearance]);
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    if (appearance == MOCK_APPEARANCE_BLACK && !black) {
        black = black_surfaces();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(black),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else if (appearance != MOCK_APPEARANCE_BLACK && black) {
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(black));
        g_clear_object(&black);
    }
}
