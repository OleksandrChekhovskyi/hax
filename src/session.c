/* SPDX-License-Identifier: MIT */
#include "session.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "diag.h"
#include "provider.h"
#include "session_prune.h"
#include "version.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/git.h"
#include "system/path.h"
#include "system/rand.h"
#include "text/width.h"

/* struct stat's sub-second mtime field is spelled differently across
 * platforms. Used to break ties between sessions created in the same
 * second so --continue / the picker reliably pick the most recent. */
#if defined(__APPLE__)
#define ST_MTIME_NSEC(st) ((long)(st).st_mtimespec.tv_nsec)
#else
#define ST_MTIME_NSEC(st) ((long)(st).st_mtim.tv_nsec)
#endif

static const char *item_kind_name(enum item_kind k)
{
    switch (k) {
    case ITEM_USER_MESSAGE:
        return "user";
    case ITEM_ASSISTANT_MESSAGE:
        return "assistant";
    case ITEM_TOOL_CALL:
        return "tool_call";
    case ITEM_TOOL_RESULT:
        return "tool_result";
    case ITEM_REASONING:
        return "reasoning";
    case ITEM_TURN_BOUNDARY:
        return "turn_boundary";
    case ITEM_TURN_USAGE:
        return "turn_usage";
    }
    return NULL;
}

static int parse_item_kind(const char *name, enum item_kind *out)
{
    if (!name)
        return -1;
    if (strcmp(name, "user") == 0)
        *out = ITEM_USER_MESSAGE;
    else if (strcmp(name, "assistant") == 0)
        *out = ITEM_ASSISTANT_MESSAGE;
    else if (strcmp(name, "tool_call") == 0)
        *out = ITEM_TOOL_CALL;
    else if (strcmp(name, "tool_result") == 0)
        *out = ITEM_TOOL_RESULT;
    else if (strcmp(name, "reasoning") == 0)
        *out = ITEM_REASONING;
    else if (strcmp(name, "turn_boundary") == 0)
        *out = ITEM_TURN_BOUNDARY;
    else if (strcmp(name, "turn_usage") == 0)
        *out = ITEM_TURN_USAGE;
    else
        return -1;
    return 0;
}

/* Jansson rejects invalid UTF-8; omit such unexpected values rather than storing JSON null. */
static void json_set_optional_string(json_t *object, const char *key, const char *value)
{
    if (!value)
        return;
    json_t *string = json_string(value);
    if (string)
        json_object_set_new(object, key, string);
}

static void json_set_nonnegative_integer(json_t *object, const char *key, long value)
{
    if (value >= 0)
        json_object_set_new(object, key, json_integer(value));
}

static void json_set_nonnegative_real(json_t *object, const char *key, double value)
{
    if (value >= 0)
        json_object_set_new(object, key, json_real(value));
}

static json_t *turn_usage_to_json(const struct turn_usage *usage)
{
    json_t *object = json_object();
    json_set_nonnegative_integer(object, "input", usage->usage.input_tokens);
    json_set_nonnegative_integer(object, "output", usage->usage.output_tokens);
    json_set_nonnegative_integer(object, "cached", usage->usage.cached_tokens);
    json_set_nonnegative_integer(object, "cache_write", usage->usage.cache_write_tokens);
    json_set_nonnegative_integer(object, "cache_write_1h", usage->usage.cache_write_1h_tokens);
    json_set_nonnegative_real(object, "cost", usage->usage.cost);
    json_set_nonnegative_integer(object, "elapsed_ms", usage->elapsed_ms);
    json_set_nonnegative_integer(object, "in_tokens", usage->uncached_input_tokens);
    json_set_nonnegative_real(object, "cost_in", usage->cost_input);
    json_set_nonnegative_real(object, "cost_cache_read", usage->cost_cache_read);
    json_set_nonnegative_real(object, "cost_cache_write", usage->cost_cache_write);
    json_set_nonnegative_real(object, "cost_out", usage->cost_output);
    json_set_nonnegative_real(object, "cost_total", usage->cost_total);
    if (usage->cost_estimated)
        json_object_set_new(object, "cost_estimated", json_true());
    json_set_optional_string(object, "provider_label", usage->provenance.provider_label);
    json_set_optional_string(object, "model_label", usage->provenance.model_label);
    json_set_optional_string(object, "effort", usage->provenance.effort);
    json_set_optional_string(object, "served_model", usage->provenance.served_model);
    json_set_optional_string(object, "route", usage->provenance.route);
    json_set_optional_string(object, "response_id", usage->provenance.response_id);
    return object;
}

static long json_get_integer_or_negative(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_integer(value) ? (long)json_integer_value(value) : -1;
}

static double json_get_real_or_negative(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_number(value) ? json_number_value(value) : -1;
}

static char *json_dup_string(const json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    return value ? xstrdup(value) : NULL;
}

static struct turn_usage *turn_usage_from_json(const json_t *object)
{
    if (!json_is_object(object))
        return NULL;
    struct turn_usage *usage = xmalloc(sizeof(*usage));
    usage->usage.input_tokens = json_get_integer_or_negative(object, "input");
    usage->usage.output_tokens = json_get_integer_or_negative(object, "output");
    usage->usage.cached_tokens = json_get_integer_or_negative(object, "cached");
    usage->usage.cache_write_tokens = json_get_integer_or_negative(object, "cache_write");
    usage->usage.cache_write_1h_tokens = json_get_integer_or_negative(object, "cache_write_1h");
    usage->usage.cost = json_get_real_or_negative(object, "cost");
    usage->elapsed_ms = json_get_integer_or_negative(object, "elapsed_ms");
    usage->uncached_input_tokens = json_get_integer_or_negative(object, "in_tokens");
    if (usage->uncached_input_tokens < 0) {
        /* These legacy records predate surcharge pricing, so plain subtraction is correct. */
        long cached = usage->usage.cached_tokens > 0 ? usage->usage.cached_tokens : 0;
        long written = usage->usage.cache_write_tokens > 0 ? usage->usage.cache_write_tokens : 0;
        long uncached = usage->usage.input_tokens - cached - written;
        usage->uncached_input_tokens = uncached > 0 ? uncached : 0;
    }
    usage->cost_input = json_get_real_or_negative(object, "cost_in");
    usage->cost_cache_read = json_get_real_or_negative(object, "cost_cache_read");
    usage->cost_cache_write = json_get_real_or_negative(object, "cost_cache_write");
    usage->cost_output = json_get_real_or_negative(object, "cost_out");
    usage->cost_total = json_get_real_or_negative(object, "cost_total");
    usage->cost_estimated = json_is_true(json_object_get(object, "cost_estimated"));
    usage->provenance.provider_label = json_dup_string(object, "provider_label");
    usage->provenance.model_label = json_dup_string(object, "model_label");
    usage->provenance.effort = json_dup_string(object, "effort");
    usage->provenance.served_model = json_dup_string(object, "served_model");
    usage->provenance.route = json_dup_string(object, "route");
    usage->provenance.response_id = json_dup_string(object, "response_id");
    return usage;
}

/* Unknown origins remain ordinary items; treating them as synthetic could hide a typed prompt. */
static const struct {
    enum item_origin origin;
    const char *name;
} ORIGIN_NAMES[] = {
    {ITEM_ORIGIN_COMPACT_SEED, "compact_seed"},
    {ITEM_ORIGIN_CONTINUATION, "continuation"},
    {ITEM_ORIGIN_LOOP, "loop"},
    {ITEM_ORIGIN_INTERRUPTED, "interrupted"},
    {ITEM_ORIGIN_SKIPPED, "skipped"},
    {ITEM_ORIGIN_REFUSED, "refused"},
    {ITEM_ORIGIN_SUMMARIZED, "summarized"},
    {ITEM_ORIGIN_TASK_NOTE, "task_note"},
    {ITEM_ORIGIN_COMPACTION, "compaction"},
};

static const char *origin_to_str(enum item_origin origin)
{
    for (size_t i = 0; i < sizeof(ORIGIN_NAMES) / sizeof(ORIGIN_NAMES[0]); i++)
        if (ORIGIN_NAMES[i].origin == origin)
            return ORIGIN_NAMES[i].name;
    return NULL; /* ITEM_ORIGIN_NONE: the key is omitted */
}

static enum item_origin json_get_item_origin(const json_t *object)
{
    const char *name = json_string_value(json_object_get(object, "origin"));
    if (!name)
        return ITEM_ORIGIN_NONE;
    for (size_t i = 0; i < sizeof(ORIGIN_NAMES) / sizeof(ORIGIN_NAMES[0]); i++)
        if (strcmp(ORIGIN_NAMES[i].name, name) == 0)
            return ORIGIN_NAMES[i].origin;
    return ITEM_ORIGIN_NONE;
}

json_t *item_to_json(const struct item *item)
{
    json_t *object = json_object();
    json_set_optional_string(object, "kind", item_kind_name(item->kind));
    json_set_optional_string(object, "text", item->text);
    json_set_optional_string(object, "call_id", item->call_id);
    json_set_optional_string(object, "tool_name", item->tool_name);
    json_set_optional_string(object, "arguments", item->tool_arguments_json);
    json_set_optional_string(object, "output", item->output);
    if (item->output_hidden_tail)
        json_object_set_new(object, "output_hidden_tail",
                            json_integer((json_int_t)item->output_hidden_tail));
    json_set_optional_string(object, "reasoning_json", item->reasoning_json);
    json_set_optional_string(object, "reasoning_text", item->reasoning_text);
    json_set_optional_string(object, "provider", item->provider);
    json_set_optional_string(object, "model", item->model);
    json_set_optional_string(object, "origin", origin_to_str(item->origin));
    if (item->usage)
        json_object_set_new(object, "usage", turn_usage_to_json(item->usage));
    if (item->inherited)
        json_object_set_new(object, "inherited", json_true());
    if (item->n_images) {
        json_t *images = json_array();
        for (size_t i = 0; i < item->n_images; i++) {
            const struct item_image *image = &item->images[i];
            json_t *image_object = json_object();
            json_set_optional_string(image_object, "mime", image->mime);
            json_set_optional_string(image_object, "data", image->data_b64);
            if (image->width > 0)
                json_object_set_new(image_object, "width", json_integer(image->width));
            if (image->height > 0)
                json_object_set_new(image_object, "height", json_integer(image->height));
            json_array_append_new(images, image_object);
        }
        json_object_set_new(object, "images", images);
    }
    return object;
}

int item_from_json(const json_t *object, struct item *out)
{
    memset(out, 0, sizeof(*out));
    if (!json_is_object(object))
        return -1;
    enum item_kind kind;
    if (parse_item_kind(json_string_value(json_object_get(object, "kind")), &kind) < 0)
        return -1;
    out->kind = kind;
    out->text = json_dup_string(object, "text");
    out->call_id = json_dup_string(object, "call_id");
    out->tool_name = json_dup_string(object, "tool_name");
    out->tool_arguments_json = json_dup_string(object, "arguments");
    out->output = json_dup_string(object, "output");
    json_t *hidden_tail = json_object_get(object, "output_hidden_tail");
    if (json_is_integer(hidden_tail) && json_integer_value(hidden_tail) > 0)
        out->output_hidden_tail = (size_t)json_integer_value(hidden_tail);
    out->reasoning_json = json_dup_string(object, "reasoning_json");
    out->reasoning_text = json_dup_string(object, "reasoning_text");
    out->provider = json_dup_string(object, "provider");
    out->model = json_dup_string(object, "model");
    out->origin = json_get_item_origin(object);
    if (kind == ITEM_TURN_USAGE)
        out->usage = turn_usage_from_json(json_object_get(object, "usage"));
    out->inherited = json_is_true(json_object_get(object, "inherited"));

    json_t *images = json_object_get(object, "images");
    size_t image_count = json_is_array(images) ? json_array_size(images) : 0;
    if (image_count > 0) {
        out->images = xcalloc(image_count, sizeof(*out->images));
        for (size_t i = 0; i < image_count; i++) {
            json_t *image_object = json_array_get(images, i);
            struct item_image *image = &out->images[out->n_images];
            image->mime = json_dup_string(image_object, "mime");
            image->data_b64 = json_dup_string(image_object, "data");
            if (!image->data_b64 || !image->mime) {
                free(image->mime);
                free(image->data_b64);
                image->mime = image->data_b64 = NULL;
                continue;
            }
            json_t *value = json_object_get(image_object, "width");
            image->width = json_is_integer(value) ? (long)json_integer_value(value) : 0;
            value = json_object_get(image_object, "height");
            image->height = json_is_integer(value) ? (long)json_integer_value(value) : 0;
            out->n_images++;
        }
        if (out->n_images == 0) {
            free(out->images);
            out->images = NULL;
        }
    }
    return 0;
}

void session_meta_free(struct session_meta *meta)
{
    if (!meta)
        return;
    free(meta->id);
    free(meta->cwd);
    free(meta->provider);
    free(meta->model);
    free(meta->effort);
    free(meta->preset);
    memset(meta, 0, sizeof(*meta));
}

/* Only the explicit half of the `no_session` tri-state: "auto" isn't a truthy
 * spelling, so it reads as "record" here and the callers who know which
 * provider is live resolve what it means (agent_recording_enabled). */
static int sessions_disabled(void)
{
    return config_bool("no_session");
}

/* The hash disambiguates the readable but non-injective path slug. */
#define CWD_SLUG_MAX 80
static char *encode_cwd(const char *cwd)
{
    if (!cwd || !*cwd)
        cwd = "unknown";

    uint64_t hash = 1469598103934665603ULL;
    for (const char *cursor = cwd; *cursor; cursor++) {
        hash ^= (unsigned char)*cursor;
        hash *= 1099511628211ULL;
    }

    const char *relative = cwd;
    while (*relative == '/')
        relative++;
    if (!*relative)
        relative = "root";
    char slug[CWD_SLUG_MAX];
    size_t length = 0;
    for (; relative[length] && length < sizeof(slug) - 1; length++)
        slug[length] = relative[length] == '/' ? '-' : relative[length];
    slug[length] = '\0';

    return xasprintf("%s.%016llx", slug, (unsigned long long)hash);
}

static char *session_directory(const char *cwd)
{
    char *encoded_cwd = encode_cwd(cwd);
    char *relative_path = xasprintf("sessions/%s", encoded_cwd);
    char *directory = xdg_hax_state_path(relative_path);
    free(relative_path);
    free(encoded_cwd);
    return directory;
}

static int is_uuid(const char *value, size_t length)
{
    if (length != 36)
        return 0;
    for (size_t i = 0; i < length; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)value[i])) {
            return 0;
        }
    }
    return 1;
}

static int has_session_timestamp(const char *value)
{
    static const char shape[] = "dddd-dd-ddTdd-dd-ddZ";
    for (size_t i = 0; i < sizeof(shape) - 1; i++) {
        if (shape[i] == 'd') {
            if (!isdigit((unsigned char)value[i]))
                return 0;
        } else if (value[i] != shape[i]) {
            return 0;
        }
    }
    return 1;
}

/* Validate the whole basename so pruning cannot claim unrelated UUID-suffixed JSONL files. */
static char *session_id_from_path(const char *path)
{
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    if (strlen(basename) != 63 || !has_session_timestamp(basename) || basename[20] != '_' ||
        strcmp(basename + 57, ".jsonl") != 0 || !is_uuid(basename + 21, 36))
        return NULL;
    char *id = xmalloc(37);
    memcpy(id, basename + 21, 36);
    id[36] = '\0';
    return id;
}

int session_path_is_standard(const char *path)
{
    char *id = session_id_from_path(path);
    int standard = id != NULL;
    free(id);
    return standard;
}

int session_touch(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    /* If flock is unsupported, pruning also fails closed; touching can still
     * proceed. On supported filesystems this waits out an in-flight prune. */
    (void)flock(fd, LOCK_SH);
    struct stat st;
    int result = fstat(fd, &st) == 0 && st.st_nlink > 0 ? futimens(fd, NULL) : -1;
    close(fd);
    return result;
}

struct session_log {
    FILE *file; /* NULL until a fresh session is materialized */
    char *path;
    int header_written;
    int selection_pending;
    size_t written_items;
    char *id;
    char *cwd;
    char *timestamp;
    char *provider;
    char *model;
    char *model_label;
    char *effort;
    char *preset;
};

/* Selection fields survive /new; identity and writer state do not. */
static int prepare_fresh_session(struct session_log *log)
{
    char *cwd = getcwd(NULL, 0);
    if (!cwd)
        return -1;
    char *directory = session_directory(cwd);
    if (!directory) {
        free(cwd);
        return -1;
    }

    char uuid[37];
    gen_uuid_v4(uuid);
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char filename_time[32];
    char header_time[32];
    /* Colons are not portable in filenames. */
    strftime(filename_time, sizeof(filename_time), "%Y-%m-%dT%H-%M-%SZ", &utc);
    strftime(header_time, sizeof(header_time), "%Y-%m-%dT%H:%M:%SZ", &utc);

    free(log->path);
    log->path = xasprintf("%s/%s_%s.jsonl", directory, filename_time, uuid);
    free(directory);
    free(log->id);
    log->id = xstrdup(uuid);
    free(log->cwd);
    log->cwd = cwd;
    free(log->timestamp);
    log->timestamp = xstrdup(header_time);
    log->file = NULL;
    log->header_written = 0;
    log->selection_pending = 0;
    log->written_items = 0;
    return 0;
}

enum session_file_mode {
    SESSION_FILE_CREATE,
    SESSION_FILE_APPEND,
};

static FILE *open_session_file(const char *path, enum session_file_mode mode);

struct session_log *session_log_open(const char *provider, const char *model,
                                     const char *model_label, const char *effort,
                                     const char *preset)
{
    if (sessions_disabled())
        return NULL;
    struct session_log *log = xcalloc(1, sizeof(*log));
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->model_label = model_label ? xstrdup(model_label) : NULL;
    log->effort = effort ? xstrdup(effort) : NULL;
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;
    if (prepare_fresh_session(log) < 0) {
        session_log_close(log);
        return NULL;
    }
    return log;
}

struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *effort, const char *preset,
                                       size_t loaded_item_count)
{
    if (sessions_disabled())
        return NULL;
    FILE *file = open_session_file(path, SESSION_FILE_APPEND);
    if (!file) {
        hax_warn("cannot append to session '%s'; this run won't be recorded", path);
        return NULL;
    }
    struct session_log *log = xcalloc(1, sizeof(*log));
    log->file = file;
    log->path = xstrdup(path);
    log->id = session_id_from_path(path);
    log->header_written = 1;
    log->written_items = loaded_item_count;
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->effort = effort ? xstrdup(effort) : NULL;
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;
    return log;
}

/* Session contents may contain secrets, so both new and resumed files are owner-only. */
static FILE *open_session_file(const char *path, enum session_file_mode mode)
{
    /* Append omits O_CREAT so a removed session is not recreated without a header. */
    int flags = O_CLOEXEC;
    flags |= mode == SESSION_FILE_APPEND ? O_RDWR | O_APPEND : O_CREAT | O_WRONLY | O_TRUNC;
    int fd = open(path, flags, 0600);
    if (fd < 0)
        return NULL;
    (void)fchmod(fd, 0600);

    /* A successful pruner lock may already refer to an unlinked inode. */
    if (flock(fd, LOCK_SH) == 0) {
        struct stat locked_stat;
        if (fstat(fd, &locked_stat) != 0 || locked_stat.st_nlink == 0)
            goto error;
    }
    if (mode == SESSION_FILE_APPEND) {
        /* Separate a partial crash record from the next valid JSON object. */
        struct stat file_stat;
        char last_byte;
        if (fstat(fd, &file_stat) != 0)
            goto error;
        if (file_stat.st_size > 0 && (pread(fd, &last_byte, 1, file_stat.st_size - 1) != 1 ||
                                      (last_byte != '\n' && write(fd, "\n", 1) != 1)))
            goto error;
    }
    FILE *file = fdopen(fd, mode == SESSION_FILE_APPEND ? "a" : "w");
    if (!file)
        goto error;
    setvbuf(file, NULL, _IOLBF, 0);
    return file;

error:
    close(fd);
    return NULL;
}

static int materialize_log(struct session_log *log)
{
    if (log->file)
        return 0;
    if (!log->path)
        return -1;

    char *filename_separator = strrchr(log->path, '/');
    if (filename_separator) {
        *filename_separator = '\0';
        int result = fs_mkdir_p(log->path);
        if (result == 0) {
            /* Project paths in directory names must not be visible to other users. */
            (void)chmod(log->path, 0700);
            char *sessions_separator = strrchr(log->path, '/');
            if (sessions_separator) {
                *sessions_separator = '\0';
                (void)chmod(log->path, 0700);
                *sessions_separator = '/';
            }
        }
        *filename_separator = '/';
        if (result < 0)
            return -1;
    }
    log->file = open_session_file(log->path, SESSION_FILE_CREATE);
    return log->file ? 0 : -1;
}

/* Recorded only when it says something the wire id doesn't, so a reader can treat its absence as
 * "the id is the label" — which is also what files predating labels mean. */
static const char *differing_model_label(const char *model_label, const char *model)
{
    if (!model_label || !model || strcmp(model_label, model) == 0)
        return NULL;
    return model_label;
}

json_t *session_header_to_json(const struct session_header *header)
{
    json_t *object = json_object();
    json_object_set_new(object, "type", json_string("session"));
    json_object_set_new(object, "version", json_integer(SESSION_FORMAT_VERSION));
    json_object_set_new(object, "hax_version", json_string(HAX_VERSION));
    json_set_optional_string(object, "id", header->id);
    json_set_optional_string(object, "timestamp", header->timestamp);
    json_set_optional_string(object, "cwd", header->cwd);
    json_set_optional_string(object, "provider", header->provider);
    json_set_optional_string(object, "model", header->model);
    json_set_optional_string(object, "model_label",
                             differing_model_label(header->model_label, header->model));
    json_set_optional_string(object, "effort", header->effort);
    json_set_optional_string(object, "preset", header->preset);

    /* Probed when the record is built — at file materialization, so the position recorded is
     * the one the conversation actually started from, and runs that never send a message pay
     * nothing; at run start for the --json stream. */
    struct git_state git;
    git_state_probe(&git);
    json_set_optional_string(object, "git_branch", git.branch);
    json_set_optional_string(object, "git_commit", git.commit);
    json_set_optional_string(object, "git_subject", git.subject);
    git_state_free(&git);
    return object;
}

static int write_json_line(FILE *file, const json_t *object)
{
    char *json = json_dumps(object, JSON_COMPACT);
    if (!json)
        return -1;
    int result = fputs(json, file) == EOF || fputc('\n', file) == EOF ? -1 : 0;
    free(json);
    return result;
}

static int write_header(struct session_log *log)
{
    struct session_header header = {
        .id = log->id,
        .timestamp = log->timestamp,
        .cwd = log->cwd,
        .provider = log->provider,
        .model = log->model,
        .model_label = log->model_label,
        .effort = log->effort,
        .preset = log->preset,
    };
    json_t *object = session_header_to_json(&header);
    int result = write_json_line(log->file, object);
    json_decref(object);
    return result;
}

/* Same-string test tolerating NULL on either side, with "" and NULL treated
 * as the same absence — the selection fields arrive from config resolution,
 * where an unset value can be spelled either way. */
static int optional_strings_equal(const char *a, const char *b)
{
    if (!a || !*a)
        return !b || !*b;
    return b && strcmp(a, b) == 0;
}

static int write_selection(struct session_log *log)
{
    json_t *selection = json_object();
    json_object_set_new(selection, "type", json_string("selection"));
    json_set_optional_string(selection, "provider", log->provider);
    json_set_optional_string(selection, "model", log->model);
    json_set_optional_string(selection, "model_label",
                             differing_model_label(log->model_label, log->model));
    json_set_optional_string(selection, "effort", log->effort);
    json_set_optional_string(selection, "preset", log->preset);
    int result = write_json_line(log->file, selection);
    json_decref(selection);
    return result;
}

void session_log_begin(struct session_log *log)
{
    if (!log || log->header_written || materialize_log(log) < 0 || write_header(log) < 0)
        return;
    log->header_written = 1;
}

void session_log_append(struct session_log *log, const struct item *items, size_t item_count)
{
    if (!log || item_count <= log->written_items || materialize_log(log) < 0)
        return;
    if (!log->header_written) {
        if (write_header(log) < 0)
            return;
        log->header_written = 1;
    }
    if (log->selection_pending) {
        if (write_selection(log) < 0)
            return;
        log->selection_pending = 0;
    }
    for (size_t i = log->written_items; i < item_count; i++) {
        json_t *object = item_to_json(&items[i]);
        int result = write_json_line(log->file, object);
        json_decref(object);
        if (result < 0)
            return;
        log->written_items = i + 1;
    }
}

void session_log_set_meta(struct session_log *log, const char *provider, const char *model,
                          const char *model_label, const char *effort, const char *preset)
{
    if (!log)
        return;
    /* A label renders the model it belongs to, so it cannot differ on its own: keep it current
     * without letting it stage a selection record. */
    free(log->model_label);
    log->model_label = model_label ? xstrdup(model_label) : NULL;

    if (optional_strings_equal(log->provider, provider) &&
        optional_strings_equal(log->model, model) && optional_strings_equal(log->effort, effort) &&
        optional_strings_equal(log->preset, preset))
        return;

    free(log->provider);
    log->provider = provider ? xstrdup(provider) : NULL;
    free(log->model);
    log->model = model ? xstrdup(model) : NULL;
    free(log->effort);
    log->effort = effort ? xstrdup(effort) : NULL;
    free(log->preset);
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;

    /* Defer the record until this selection produces an item. */
    if (log->header_written)
        log->selection_pending = 1;
}

void session_log_discard_selection(struct session_log *log)
{
    if (log)
        log->selection_pending = 0;
}

void session_log_reset(struct session_log *log)
{
    if (!log)
        return;
    if (log->file) {
        fclose(log->file);
        log->file = NULL;
    }
    if (prepare_fresh_session(log) < 0) {
        /* State dir vanished mid-run (unlikely) — mark unavailable so
         * subsequent appends no-op rather than crash. */
        free(log->path);
        log->path = NULL;
    }
}

void session_log_close(struct session_log *log)
{
    if (!log)
        return;
    if (log->file)
        fclose(log->file);
    free(log->path);
    free(log->id);
    free(log->cwd);
    free(log->timestamp);
    free(log->provider);
    free(log->model);
    free(log->model_label);
    free(log->effort);
    free(log->preset);
    free(log);
}

const char *session_log_path(const struct session_log *log)
{
    return log ? log->path : NULL;
}

const char *session_log_resume_hint(const struct session_log *log)
{
    if (!log || !log->header_written)
        return NULL;
    return log->id;
}

const char *session_log_id(const struct session_log *log)
{
    return log ? log->id : NULL;
}

int session_log_undo(struct session_log *log, size_t keep_user_turns, size_t new_item_count)
{
    if (!log || !log->file || !log->header_written)
        return 0;
    json_t *record = json_object();
    json_object_set_new(record, "type", json_string("undo"));
    json_object_set_new(record, "keep_user_turns", json_integer((json_int_t)keep_user_turns));
    int result = write_json_line(log->file, record);
    json_decref(record);
    if (result < 0 || fflush(log->file) != 0)
        return -1;
    log->written_items = new_item_count;
    return 0;
}

void session_log_user_turn(struct session_log *log, long elapsed_ms)
{
    if (!log || !log->file || !log->header_written || elapsed_ms < 0)
        return;
    json_t *record = json_object();
    json_object_set_new(record, "type", json_string("user_turn"));
    json_object_set_new(record, "elapsed_ms", json_integer((json_int_t)elapsed_ms));
    if (write_json_line(log->file, record) == 0)
        fflush(log->file);
    json_decref(record);
}

int session_log_materialized(const struct session_log *log)
{
    return log && log->header_written;
}

static char *fork_session_path(const char *source_path, const char *filename_time, const char *uuid)
{
    const char *separator = strrchr(source_path, '/');
    if (!separator)
        return xasprintf("%s_%s.jsonl", filename_time, uuid);

    size_t directory_length = (size_t)(separator - source_path);
    char *directory = xmalloc(directory_length + 1);
    memcpy(directory, source_path, directory_length);
    directory[directory_length] = '\0';
    char *path = xasprintf("%s/%s_%s.jsonl", directory, filename_time, uuid);
    free(directory);
    return path;
}

static json_t *read_header(const char *path)
{
    FILE *source = fopen(path, "r");
    if (!source)
        return NULL;
    char *line = NULL;
    size_t capacity = 0;
    json_t *header = NULL;
    if (getline(&line, &capacity, source) >= 0)
        header = json_loads(line, 0, NULL);
    free(line);
    fclose(source);
    if (header && !json_is_object(header)) {
        json_decref(header);
        header = NULL;
    }
    return header;
}

/* Set `key` to `value`, or drop it so an absent field reads as unset rather than inherited. */
static void set_or_delete_string(json_t *object, const char *key, const char *value)
{
    if (value && *value)
        json_object_set_new(object, key, json_string(value));
    else
        json_object_del(object, key);
}

int session_fork_file(const char *source_path, const struct item *items, size_t n_items,
                      const struct session_header *selection, char **out_path)
{
    *out_path = NULL;
    int result = -1;
    int destination_created = 0;
    int destination_fd = -1;
    FILE *destination = NULL;
    char *destination_path = NULL;

    json_t *header = read_header(source_path);
    if (!header)
        goto out;

    char uuid[37];
    gen_uuid_v4(uuid);
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char filename_time[32];
    char header_time[32];
    strftime(filename_time, sizeof(filename_time), "%Y-%m-%dT%H-%M-%SZ", &utc);
    strftime(header_time, sizeof(header_time), "%Y-%m-%dT%H:%M:%SZ", &utc);

    const char *source_id = json_string_value(json_object_get(header, "id"));
    if (source_id)
        json_object_set_new(header, "forked_from", json_string(source_id));
    json_object_set_new(header, "id", json_string(uuid));
    json_object_set_new(header, "timestamp", json_string(header_time));
    set_or_delete_string(header, "provider", selection->provider);
    set_or_delete_string(header, "model", selection->model);
    set_or_delete_string(header, "model_label",
                         differing_model_label(selection->model_label, selection->model));
    set_or_delete_string(header, "effort", selection->effort);
    set_or_delete_string(header, "preset", selection->preset);

    destination_path = fork_session_path(source_path, filename_time, uuid);
    destination_fd = open(destination_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (destination_fd < 0)
        goto out;
    destination_created = 1;
    destination = fdopen(destination_fd, "w");
    if (!destination)
        goto out;
    destination_fd = -1;

    if (write_json_line(destination, header) < 0)
        goto out;
    for (size_t i = 0; i < n_items; i++) {
        struct item copy = items[i];
        copy.inherited = 1;
        json_t *object = item_to_json(&copy);
        int written = write_json_line(destination, object);
        json_decref(object);
        if (written < 0)
            goto out;
    }
    if (fclose(destination) != 0) {
        destination = NULL;
        goto out;
    }
    destination = NULL;

    *out_path = destination_path;
    destination_path = NULL;
    result = 0;

out:
    if (destination)
        fclose(destination);
    if (destination_fd >= 0)
        close(destination_fd);
    if (result < 0 && destination_created)
        unlink(destination_path);
    free(destination_path);
    json_decref(header);
    return result;
}

static void push_item(struct item **items, size_t *count, size_t *capacity, struct item item)
{
    if (*count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 16;
        *items = xrealloc(*items, *capacity * sizeof(**items));
    }
    (*items)[(*count)++] = item;
}

/* Keep earliest images when a file exceeds the current request limits. Only the model-visible
 * window is budgeted: images a compaction summarized away are never sent, so charging the limit
 * for them would degrade the live images that replaced them. */
static void degrade_excess_images(struct item *items, size_t item_count)
{
    size_t encoded_bytes = 0;
    size_t image_count = 0;
    for (size_t i = items_context_floor(items, item_count); i < item_count; i++) {
        struct item *item = &items[i];
        if (item->n_images == 0)
            continue;

        size_t item_bytes = 0;
        for (size_t image_index = 0; image_index < item->n_images; image_index++)
            if (item->images[image_index].data_b64)
                item_bytes += strlen(item->images[image_index].data_b64);
        if (encoded_bytes + item_bytes <= IMAGE_REQUEST_BASE64_BUDGET_BYTES &&
            image_count + item->n_images <= IMAGE_REQUEST_MAX_COUNT) {
            encoded_bytes += item_bytes;
            image_count += item->n_images;
            continue;
        }

        char *output = xstrdup(item->output ? item->output : "");
        for (size_t image_index = 0; image_index < item->n_images; image_index++) {
            char *placeholder = item_image_placeholder(&item->images[image_index]);
            char *extended_output = xasprintf("%s\n%s", output, placeholder);
            free(output);
            free(placeholder);
            output = extended_output;
            free(item->images[image_index].mime);
            free(item->images[image_index].data_b64);
        }
        free(item->images);
        item->images = NULL;
        item->n_images = 0;
        free(item->output);
        item->output = output;
    }
}

/* Selection records are complete snapshots, so absent fields clear previous values. */
static void apply_selection_record(struct session_meta *meta, const json_t *object)
{
    free(meta->provider);
    meta->provider = json_dup_string(object, "provider");
    free(meta->model);
    meta->model = json_dup_string(object, "model");
    free(meta->effort);
    meta->effort = json_dup_string(object, "effort");
    free(meta->preset);
    meta->preset = json_dup_string(object, "preset");
}

static FILE *open_session_reader(const char *path)
{
    int fd = fs_open_regular(path);
    if (fd < 0)
        return NULL;

    FILE *file = fdopen(fd, "r");
    if (!file) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return file;
}

int session_read_meta(const char *path, struct session_meta *out)
{
    memset(out, 0, sizeof(*out));
    FILE *file = open_session_reader(path);
    if (!file)
        return -1;

    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        /* Item records use "kind", so most large lines need no JSON parse. */
        if (!strstr(line, "\"type\""))
            continue;
        json_t *object = json_loads(line, 0, NULL);
        if (!object)
            continue;
        const char *type = json_string_value(json_object_get(object, "type"));
        if (type && strcmp(type, "session") == 0) {
            free(out->id);
            out->id = json_dup_string(object, "id");
            free(out->cwd);
            out->cwd = json_dup_string(object, "cwd");
            apply_selection_record(out, object);
        } else if (type && strcmp(type, "selection") == 0) {
            apply_selection_record(out, object);
        }
        json_decref(object);
    }
    int result = ferror(file) ? -1 : 0;
    free(line);
    fclose(file);
    if (result < 0)
        session_meta_free(out);
    return result;
}

static int tool_call_has_result(const struct item *items, size_t count, const struct item *call)
{
    if (!call->call_id)
        return 0;
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_TOOL_RESULT && items[i].call_id &&
            strcmp(items[i].call_id, call->call_id) == 0)
            return 1;
    return 0;
}

static size_t remove_incomplete_tool_calls(struct item *items, size_t count)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (items[i].kind == ITEM_TOOL_CALL && !tool_call_has_result(items, count, &items[i])) {
            item_free(&items[i]);
            continue;
        }
        items[kept++] = items[i];
    }
    return kept;
}

/* Undo records name a typed-prompt count rather than an item index: item indices shift when the
 * loader skips a torn line or drops an incomplete tool call, prompt counts do not. */
static void apply_undo_record(struct session_loaded *loaded, size_t *retired_capacity,
                              const json_t *object)
{
    json_t *value = json_object_get(object, "keep_user_turns");
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        return;
    size_t cut =
        items_user_turn_cut(loaded->items, loaded->n_items, (size_t)json_integer_value(value));
    for (size_t i = cut; i < loaded->n_items; i++)
        push_item(&loaded->retired, &loaded->n_retired, retired_capacity, loaded->items[i]);
    loaded->n_items = cut;
    loaded->last_user_turn_ms = -1;
}

static void apply_user_turn_record(struct session_loaded *loaded, const json_t *object)
{
    json_t *value = json_object_get(object, "elapsed_ms");
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        return;
    loaded->last_user_turn_ms = (long)json_integer_value(value);
    loaded->worked_ms += loaded->last_user_turn_ms;
}

int session_load_all(const char *path, struct session_loaded *out)
{
    memset(out, 0, sizeof(*out));
    out->last_user_turn_ms = -1;

    FILE *file = open_session_reader(path);
    if (!file)
        return -1;

    size_t live_capacity = 0;
    size_t retired_capacity = 0;
    char *header_provider = NULL;
    char *header_model = NULL;
    char *line = NULL;
    size_t line_capacity = 0;

    while (getline(&line, &line_capacity, file) >= 0) {
        json_t *object = json_loads(line, 0, NULL);
        if (!object)
            continue; /* A crash may leave one partial final record. */

        const char *type = json_string_value(json_object_get(object, "type"));
        if (type) {
            if (strcmp(type, "session") == 0) {
                if (!header_provider)
                    header_provider = json_dup_string(object, "provider");
                if (!header_model)
                    header_model = json_dup_string(object, "model");
                free(out->meta.id);
                out->meta.id = json_dup_string(object, "id");
                free(out->meta.cwd);
                out->meta.cwd = json_dup_string(object, "cwd");
                apply_selection_record(&out->meta, object);
            } else if (strcmp(type, "selection") == 0) {
                apply_selection_record(&out->meta, object);
            } else if (strcmp(type, "undo") == 0) {
                apply_undo_record(out, &retired_capacity, object);
            } else if (strcmp(type, "user_turn") == 0) {
                apply_user_turn_record(out, object);
            }
            json_decref(object);
            continue;
        }

        struct item item;
        if (item_from_json(object, &item) == 0) {
            /* Old reasoning records inherit the header provenance needed for safe replay. */
            if (item.kind == ITEM_REASONING) {
                if (!item.provider && header_provider)
                    item.provider = xstrdup(header_provider);
                if (!item.model && header_model)
                    item.model = xstrdup(header_model);
            }
            push_item(&out->items, &out->n_items, &live_capacity, item);
        }
        json_decref(object);
    }

    int read_failed = ferror(file);
    free(line);
    fclose(file);
    free(header_provider);
    free(header_model);
    if (read_failed) {
        session_loaded_free(out);
        return -1;
    }

    /* Providers reject tool calls that lack a corresponding result after a crash. */
    out->n_items = remove_incomplete_tool_calls(out->items, out->n_items);
    degrade_excess_images(out->items, out->n_items);
    return 0;
}

void session_loaded_free(struct session_loaded *loaded)
{
    for (size_t i = 0; i < loaded->n_items; i++)
        item_free(&loaded->items[i]);
    free(loaded->items);
    for (size_t i = 0; i < loaded->n_retired; i++)
        item_free(&loaded->retired[i]);
    free(loaded->retired);
    session_meta_free(&loaded->meta);
    memset(loaded, 0, sizeof(*loaded));
}

int session_load(const char *path, struct item **out_items, size_t *out_count,
                 struct session_meta *out_meta)
{
    struct session_loaded loaded;
    if (out_meta)
        memset(out_meta, 0, sizeof(*out_meta));
    *out_items = NULL;
    *out_count = 0;
    if (session_load_all(path, &loaded) != 0)
        return -1;
    *out_items = loaded.items;
    *out_count = loaded.n_items;
    loaded.items = NULL;
    loaded.n_items = 0;
    if (out_meta) {
        *out_meta = loaded.meta;
        memset(&loaded.meta, 0, sizeof(loaded.meta));
    }
    session_loaded_free(&loaded);
    return 0;
}

/* A prompt should occur before this bound; avoid reading an early multi-megabyte result. */
#define LABEL_SCAN_CAP (64 * 1024)

void session_label_read(const char *path, int max_cells, struct session_label *out)
{
    *out = (struct session_label){0};
    char *data = fs_read_file_capped(path, LABEL_SCAN_CAP, NULL, NULL);
    if (!data)
        return;

    char *save = NULL;
    int saw_compaction_seed = 0;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line)
            continue;
        json_t *object = json_loads(line, 0, NULL);
        if (!object)
            continue;

        const char *type = json_string_value(json_object_get(object, "type"));
        if (type && (strcmp(type, "session") == 0 || strcmp(type, "selection") == 0)) {
            /* Only records ahead of the opening prompt are seen, so a later /model switch does not
             * show up — the label describes what the conversation started as. */
            free(out->provider);
            out->provider = json_dup_string(object, "provider");
            free(out->model);
            out->model = json_dup_string(object, "model_label");
            if (!out->model)
                out->model = json_dup_string(object, "model");
            free(out->effort);
            out->effort = json_dup_string(object, "effort");
            free(out->preset);
            out->preset = json_dup_string(object, "preset");
            if (strcmp(type, "session") == 0) {
                free(out->git_branch);
                out->git_branch = json_dup_string(object, "git_branch");
                free(out->git_subject);
                out->git_subject = json_dup_string(object, "git_subject");
            }
            json_decref(object);
            continue;
        }

        const char *kind = json_string_value(json_object_get(object, "kind"));
        if (kind && strcmp(kind, "user") == 0) {
            enum item_origin origin = json_get_item_origin(object);
            if (origin != ITEM_ORIGIN_NONE) {
                if (origin == ITEM_ORIGIN_COMPACT_SEED)
                    saw_compaction_seed = 1;
                json_decref(object);
                continue;
            }
            const char *text = json_string_value(json_object_get(object, "text"));
            if (text) {
                char *flattened = flatten_for_display(text);
                out->prompt = truncate_for_display(flattened, (size_t)max_cells);
                free(flattened);
            }
            json_decref(object);
            break;
        }
        json_decref(object);
    }
    free(data);
    if (!out->prompt && saw_compaction_seed)
        out->prompt = xstrdup("(compacted)");
}

void session_label_free(struct session_label *label)
{
    free(label->prompt);
    free(label->provider);
    free(label->model);
    free(label->effort);
    free(label->preset);
    free(label->git_branch);
    free(label->git_subject);
    *label = (struct session_label){0};
}

static int compare_session_mtime_desc(const void *left_pointer, const void *right_pointer)
{
    const struct session_entry *left = left_pointer;
    const struct session_entry *right = right_pointer;
    if (left->mtime != right->mtime)
        return left->mtime < right->mtime ? 1 : -1;
    if (left->mtime_nsec != right->mtime_nsec)
        return left->mtime_nsec < right->mtime_nsec ? 1 : -1;
    /* Sessions written within one timestamp tick carry no recorded order, and not every filesystem
     * records a sub-second one: OpenBSD stamps rapid writes with an identical mtime. Order them by
     * path so listings and --continue stay reproducible rather than left to an unstable sort. */
    return strcmp(right->path, left->path);
}

static int has_jsonl_extension(const char *name)
{
    size_t length = strlen(name);
    return length >= 6 && strcmp(name + length - 6, ".jsonl") == 0;
}

int session_list(const char *cwd, struct session_entry **out_entries, size_t *out_count)
{
    *out_entries = NULL;
    *out_count = 0;
    char *directory = session_directory(cwd);
    if (!directory)
        return 0;
    DIR *directory_stream = opendir(directory);
    if (!directory_stream) {
        free(directory);
        return 0;
    }

    struct session_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    time_t cutoff = session_retention_cutoff();
    struct dirent *directory_entry;
    while ((directory_entry = readdir(directory_stream))) {
        if (!has_jsonl_extension(directory_entry->d_name))
            continue;
        char *path = xasprintf("%s/%s", directory, directory_entry->d_name);
        struct stat file_stat;
        if (stat(path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            free(path);
            continue;
        }
        char *id = session_id_from_path(path);
        if (id && cutoff && file_stat.st_mtime < cutoff) {
            free(id);
            free(path);
            continue;
        }
        struct session_entry entry = {
            .path = path,
            .id = id,
            .mtime = (long)file_stat.st_mtime,
            .mtime_nsec = ST_MTIME_NSEC(file_stat),
        };
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            entries = xrealloc(entries, capacity * sizeof(*entries));
        }
        entries[count++] = entry;
    }
    closedir(directory_stream);
    free(directory);

    qsort(entries, count, sizeof(*entries), compare_session_mtime_desc);
    *out_entries = entries;
    *out_count = count;
    return 0;
}

char *session_prompt_history_path(const char *cwd)
{
    if (!cwd)
        return NULL;
    char *directory = session_directory(cwd);
    if (!directory)
        return NULL;
    char *path = path_join(directory, "history");
    free(directory);
    return path;
}

void session_list_free(struct session_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].id);
        session_label_free(&entries[i].label);
    }
    free(entries);
}
