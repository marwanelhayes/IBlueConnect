#include "live.h"
#include <math.h>

#include <stdio.h>
#include <string.h>

static void set_caller(MockUiModel *model, const char *caller, const char *number)
{
    g_strlcpy(model->caller_name, caller ? caller : "", sizeof model->caller_name);
    int index = mock_ui_contact_named(caller);
    model->caller_index = index;
    g_strlcpy(model->dialed_number, number ? number : "", sizeof model->dialed_number);
}

void live_apply_snapshot(MockUiModel *model, const char *status, const char *id,
                         const char *phase, const char *caller, const char *number)
{
    if (!status) model->connection = MOCK_CONNECTION_ERROR;
    else if (strcmp(status, "ready") == 0 || strcmp(status, "manage calls on phone") == 0)
        model->connection = MOCK_CONNECTION_CONNECTED;
    else model->connection = MOCK_CONNECTION_DISCONNECTED;
    model->calls_unavailable = model->connection == MOCK_CONNECTION_CONNECTED &&
                               strcmp(status, "ready") != 0;
    /* Connected but "unavailable": held or several calls the single-call snapshot can't describe.
     * They have not ended; live_apply_calls() owns this state. */
    if (model->connection == MOCK_CONNECTION_CONNECTED && strcmp(phase, "unavailable") == 0)
        return;
    gboolean calling = model->connection == MOCK_CONNECTION_CONNECTED && id && *id &&
                       strcmp(phase, "ended") != 0 && strcmp(phase, "idle") != 0 &&
                       strcmp(phase, "unavailable") != 0;
    if (!calling) {
        gboolean was_live = model->call == MOCK_CALL_ACTIVE || model->call == MOCK_CALL_OUTGOING ||
                            model->call == MOCK_CALL_CONNECTING || model->call == MOCK_CALL_ENDING;
        if (was_live && model->popup_visible) model->call = MOCK_CALL_ENDED;
        else if (!(model->call == MOCK_CALL_DECLINED || model->call == MOCK_CALL_ENDED))
            mock_ui_finish_call(model);
        return;
    }
    if (strcmp(model->call_id, id) != 0) {
        gboolean shown = FALSE; /* A call already in the controller's list, e.g. a waiting call just answered. */
        for (unsigned i = 0; model->call == MOCK_CALL_ACTIVE && i < model->call_count; i++)
            shown |= strcmp(model->lines[i].id, id) == 0;
        if (!shown) mock_ui_finish_call(model);
        g_strlcpy(model->call_id, id, sizeof model->call_id);
    }
    set_caller(model, caller, number);
    MockCall call = model->call;
    if (strcmp(phase, "incoming") == 0) {
        if (call == MOCK_CALL_IDLE) {
            model->call = MOCK_CALL_INCOMING;
            model->popup_visible = true;
        }
    } else if (strcmp(phase, "dialing") == 0 || strcmp(phase, "alerting") == 0) {
        if (call == MOCK_CALL_IDLE) {
            model->call = MOCK_CALL_OUTGOING;
            model->popup_visible = true;
        }
    } else if (strcmp(phase, "active") == 0) {
        if (call == MOCK_CALL_IDLE || call == MOCK_CALL_INCOMING || call == MOCK_CALL_INCOMING_IGNORED) {
            /* Answered on the iPhone after ringing here stays on the phone; a call first seen already
             * active (the phone connected mid-call) opens the controller. */
            model->call = MOCK_CALL_ACTIVE;
            model->popup_visible = call == MOCK_CALL_IDLE;
            model->elapsed_seconds = 0;
        } else if (call == MOCK_CALL_OUTGOING || call == MOCK_CALL_CONNECTING) {
            model->call = MOCK_CALL_ACTIVE;
            model->elapsed_seconds = 0;
        }
    } else if (strcmp(phase, "ending") == 0) {
        if (call == MOCK_CALL_ACTIVE || call == MOCK_CALL_OUTGOING || call == MOCK_CALL_CONNECTING)
            model->call = MOCK_CALL_ENDING;
    }
}

static MockLinePhase line_phase(const char *phase)
{
    if (strcmp(phase, "active") == 0) return MOCK_LINE_ACTIVE;
    if (strcmp(phase, "held") == 0) return MOCK_LINE_HELD;
    if (strcmp(phase, "dialing") == 0 || strcmp(phase, "alerting") == 0) return MOCK_LINE_DIALING;
    if (strcmp(phase, "waiting") == 0) return MOCK_LINE_WAITING;
    if (strcmp(phase, "incoming") == 0) return MOCK_LINE_INCOMING;
    return MOCK_LINE_OTHER;
}

void live_apply_calls(MockUiModel *model, const char *status, GVariant *rows)
{
    model->call_count = 0;
    if (status && strcmp(status, "ready") == 0 && rows) {
        model->calls_unavailable = false;
        GVariantIter iter;
        const char *id, *phase, *audio, *caller, *number;
        gboolean group;
        g_variant_iter_init(&iter, rows);
        while (g_variant_iter_next(&iter, "(&s&s&s&s&sb)", &id, &phase, &audio, &caller, &number, &group))
            mock_ui_add_line(model, id, caller, number, line_phase(phase), group);
    }
    mock_ui_derive_calls(model);
}

struct LiveData {
    GStringChunk *strings;
    GArray *contacts, *recents, *favorites;
};

static const char *kind_text(const char *kind)
{
    if (strcmp(kind, "missed") == 0) return "Missed";
    if (strcmp(kind, "received") == 0) return "Incoming";
    if (strcmp(kind, "dialed") == 0) return "Outgoing";
    if (strcmp(kind, "declined") == 0) return "Declined";
    if (strcmp(kind, "failed") == 0) return "Failed";
    return "Call";
}

static guint local_day(GDateTime *time)
{
    int year, month, day;
    g_date_time_get_ymd(time, &year, &month, &day);
    GDate *date = g_date_new_dmy(day, month, year);
    guint julian = g_date_get_julian(date);
    g_date_free(date);
    return julian;
}

/* Time for today's calls, "Yesterday", weekday within a week, otherwise the date. */
static char *when(gint64 started, gint64 now)
{
    GDateTime *then = g_date_time_new_from_unix_local(started);
    GDateTime *today = g_date_time_new_from_unix_local(now);
    guint days = local_day(today) - local_day(then);
    char *text = days == 1 ? g_strdup("Yesterday")
        : g_date_time_format(then, days == 0 ? "%H:%M" : days < 7 ? "%A" : "%Y-%m-%d");
    g_date_time_unref(then);
    g_date_time_unref(today);
    return text;
}

typedef struct { size_t index; guint calls; } Frequency;

static gint by_calls(gconstpointer a, gconstpointer b)
{
    const Frequency *left = a, *right = b;
    return left->calls != right->calls ? (right->calls > left->calls ? 1 : -1)
                                       : (left->index > right->index) - (left->index < right->index);
}

LiveData *live_load(GVariant *contacts, GVariant *recents, gint64 now)
{
    LiveData *data = g_new0(LiveData, 1);
    data->strings = g_string_chunk_new(4096);
    data->contacts = g_array_new(FALSE, TRUE, sizeof(MockContact));
    data->recents = g_array_new(FALSE, TRUE, sizeof(MockRecent));
    data->favorites = g_array_new(FALSE, FALSE, sizeof(size_t));
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
    GVariantIter iter;
    const char *name, *label, *number, *kind;
    g_variant_iter_init(&iter, contacts);
    while (g_variant_iter_next(&iter, "(&s&s&s)", &name, &label, &number)) {
        if (!*name || !*number) continue;
        gpointer found = g_hash_table_lookup(index, name);
        if (!found) {
            MockContact contact = { .name = g_string_chunk_insert_const(data->strings, name) };
            g_array_append_val(data->contacts, contact);
            found = GSIZE_TO_POINTER(data->contacts->len);
            g_hash_table_insert(index, (gpointer)contact.name, found);
        }
        MockContact *contact = &g_array_index(data->contacts, MockContact, GPOINTER_TO_SIZE(found) - 1);
        for (size_t n = 0; n < MOCK_CONTACT_NUMBERS; n++) {
            if (contact->numbers[n]) continue;
            contact->numbers[n] = g_string_chunk_insert_const(data->strings, number);
            contact->labels[n] = *label ? g_string_chunk_insert_const(data->strings, label) : NULL;
            break;
        }
    }
    GArray *frequency = g_array_new(FALSE, FALSE, sizeof(Frequency));
    gint64 started;
    g_variant_iter_init(&iter, recents);
    while (g_variant_iter_next(&iter, "(&s&s&sx)", &name, &number, &kind, &started)) {
        char *time = when(started, now);
        MockRecent recent = {
            .name = *name ? g_string_chunk_insert_const(data->strings, name) : NULL,
            .number = g_string_chunk_insert_const(data->strings, *number ? number : "Unknown caller"),
            .kind = kind_text(kind),
            .time = g_string_chunk_insert_const(data->strings, time),
        };
        g_free(time);
        g_array_append_val(data->recents, recent);
        gpointer found = *name ? g_hash_table_lookup(index, name) : NULL;
        if (!found) continue;
        size_t contact = GPOINTER_TO_SIZE(found) - 1;
        guint i = 0;
        while (i < frequency->len && g_array_index(frequency, Frequency, i).index != contact) i++;
        if (i == frequency->len) g_array_append_val(frequency, ((Frequency) { contact, 0 }));
        g_array_index(frequency, Frequency, i).calls++;
    }
    g_array_sort(frequency, by_calls);
    for (guint i = 0; i < frequency->len && i < 4; i++)
        g_array_append_val(data->favorites, g_array_index(frequency, Frequency, i).index);
    g_array_free(frequency, TRUE);
    g_hash_table_destroy(index);
    mock_ui_set_data((MockContact *)(void *)data->contacts->data, data->contacts->len,
                     (MockRecent *)(void *)data->recents->data, data->recents->len,
                     (size_t *)(void *)data->favorites->data, data->favorites->len);
    return data;
}

void live_free(LiveData *data)
{
    if (!data) return;
    if (mock_contacts == (const MockContact *)(void *)data->contacts->data)
        mock_ui_set_data(NULL, 0, NULL, 0, NULL, 0);
    g_array_free(data->contacts, TRUE);
    g_array_free(data->recents, TRUE);
    g_array_free(data->favorites, TRUE);
    g_string_chunk_free(data->strings);
    g_free(data);
}

void live_apply_battery(MockUiModel *model, const char *status)
{
    unsigned level;
    if (status && sscanf(status, "iPhone battery level %u/5", &level) == 1 && level <= 5)
        g_snprintf(model->battery, sizeof model->battery, "Battery · about %u%%", level * 20);
    else
        g_strlcpy(model->battery, "Battery · Unavailable", sizeof model->battery);
}

void live_apply_contacts_status(MockUiModel *model, const char *status)
{
    g_strlcpy(model->sync_label, status ? status : "", sizeof model->sync_label);
    model->contact_sync_error = status && g_str_has_prefix(status, "Contact sync failed");
}

double live_voice_level(const gint16 *samples, size_t count)
{
    if (!count) return 0.0;
    double sum = 0;
    for (size_t i = 0; i < count; i++) sum += (double)samples[i] * samples[i];
    double rms = sqrt(sum / count) / 32768.0;
    if (rms <= 0) return 0.0;
    /* ponytail: fixed -50..-10 dBFS window; add a calibration setting if real calls sit outside it. */
    return CLAMP((20 * log10(rms) + 50) / 40, 0.0, 1.0);
}

struct LiveVoiceMeter {
    GSubprocess *process;
    GCancellable *cancel;
    LiveVoiceFn fn; /* NULL once stopped. */
    gpointer data;
    bool reading; /* A read (or its callback) is in flight and will free a stopped meter. */
    gint16 buffer[800]; /* 50 ms at 16 kHz. */
};

static void meter_free(LiveVoiceMeter *meter)
{
    g_subprocess_force_exit(meter->process);
    g_object_unref(meter->process);
    g_object_unref(meter->cancel);
    g_free(meter);
}

static void meter_read(LiveVoiceMeter *meter);

static void on_meter_read(GObject *source, GAsyncResult *result, gpointer data)
{
    LiveVoiceMeter *meter = data;
    gssize bytes = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);
    if (meter->fn) /* -1 tells the owner the stream ended, e.g. the audio moved to the iPhone. */
        meter->fn(bytes > 0 ? live_voice_level(meter->buffer, (size_t)bytes / 2) : -1.0, meter->data);
    meter->reading = false;
    if (!meter->fn) meter_free(meter);
    else if (bytes > 0) meter_read(meter); /* Otherwise the stream ended; stop() frees it. */
}

static void meter_read(LiveVoiceMeter *meter)
{
    meter->reading = true;
    g_input_stream_read_async(g_subprocess_get_stdout_pipe(meter->process), meter->buffer,
                              sizeof meter->buffer, G_PRIORITY_DEFAULT, meter->cancel, on_meter_read, meter);
}

LiveVoiceMeter *live_voice_meter_start(guint64 serial, LiveVoiceFn fn, gpointer data)
{
    char target[32];
    g_snprintf(target, sizeof target, "%" G_GUINT64_FORMAT, serial);
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
        "pw-record", "--target", target, "--rate", "16000", "--channels", "1", "--format", "s16",
        "--latency", "50ms", "-P",
        "{ node.name=iblueconnect-voice-meter node.dont-reconnect=true state.restore-props=false }", "-", NULL);
    if (!process) return NULL;
    LiveVoiceMeter *meter = g_new0(LiveVoiceMeter, 1);
    meter->process = process;
    meter->cancel = g_cancellable_new();
    meter->fn = fn;
    meter->data = data;
    meter_read(meter);
    return meter;
}

void live_voice_meter_stop(LiveVoiceMeter *meter)
{
    if (!meter) return;
    meter->fn = NULL;
    if (meter->reading) g_cancellable_cancel(meter->cancel);
    else meter_free(meter);
}
