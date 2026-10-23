#include "appearance.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

/* The choice persists in a private keyfile; anything unreadable falls back to the system appearance. */
static void appearance_round_trips_and_fails_safe(void)
{
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_SYSTEM);
    GError *error = NULL;
    g_assert_true(appearance_save(MOCK_APPEARANCE_DARK, &error));
    g_assert_no_error(error);
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_DARK);
    g_assert_true(appearance_save(MOCK_APPEARANCE_LIGHT, NULL));
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_LIGHT);
    char *dir = g_build_filename(g_get_user_config_dir(), "iblueconnect", NULL);
    char *path = g_build_filename(dir, "appearance.ini", NULL);
    GStatBuf info;
    g_assert_cmpint(g_stat(dir, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, 0700);
    g_assert_true(g_file_set_contents(path, "not a keyfile", -1, NULL));
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_SYSTEM);
    g_assert_true(g_file_set_contents(path, "[ui]\nappearance=purple\n", -1, NULL));
    g_assert_cmpint(appearance_load(), ==, MOCK_APPEARANCE_SYSTEM);
    g_assert_cmpint(appearance_parse("dark"), ==, MOCK_APPEARANCE_DARK);
    g_assert_cmpstr(appearance_name(MOCK_APPEARANCE_LIGHT), ==, "light");
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

int main(int argc, char **argv)
{
    char *home = g_dir_make_tmp("iblueconnect-appearance-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", home, TRUE); /* Before GLib caches the config directory. */
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/appearance/round-trip", appearance_round_trips_and_fails_safe);
    int result = g_test_run();
    g_rmdir(home);
    g_free(home);
    return result;
}
