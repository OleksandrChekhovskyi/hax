/* SPDX-License-Identifier: MIT */
#include "session.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "system/fs.h"

/* struct stat's sub-second mtime field is spelled differently across
 * platforms. Used to break ties between sessions created in the same
 * second so --continue / the picker reliably pick the most recent. */
#if defined(__APPLE__)
#define ST_MTIME_NSEC(st) ((long)(st).st_mtimespec.tv_nsec)
#else
#define ST_MTIME_NSEC(st) ((long)(st).st_mtim.tv_nsec)
#endif

/* ---------------- item <-> JSON codec ---------------- */

static const char *kind_to_str(enum item_kind k)
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
    }
    return NULL;
}

static int kind_from_str(const char *s, enum item_kind *out)
{
    if (!s)
        return -1;
    if (strcmp(s, "user") == 0)
        *out = ITEM_USER_MESSAGE;
    else if (strcmp(s, "assistant") == 0)
        *out = ITEM_ASSISTANT_MESSAGE;
    else if (strcmp(s, "tool_call") == 0)
        *out = ITEM_TOOL_CALL;
    else if (strcmp(s, "tool_result") == 0)
        *out = ITEM_TOOL_RESULT;
    else if (strcmp(s, "reasoning") == 0)
        *out = ITEM_REASONING;
    else if (strcmp(s, "turn_boundary") == 0)
        *out = ITEM_TURN_BOUNDARY;
    else
        return -1;
    return 0;
}

/* Set `key` to `val` only when val is non-NULL. json_string returns NULL
 * for non-UTF-8 input; json_object_set_new tolerates that (it would error
 * out), so we guard explicitly and skip rather than drop a half-set key.
 * In practice every string here has already survived jansson encoding on
 * the provider wire path, so NULL is not expected. */
static void set_str(json_t *o, const char *key, const char *val)
{
    if (!val)
        return;
    json_t *s = json_string(val);
    if (s)
        json_object_set_new(o, key, s);
}

json_t *item_to_json(const struct item *it)
{
    json_t *o = json_object();
    set_str(o, "kind", kind_to_str(it->kind));
    set_str(o, "text", it->text);
    set_str(o, "call_id", it->call_id);
    set_str(o, "tool_name", it->tool_name);
    set_str(o, "arguments", it->tool_arguments_json);
    set_str(o, "output", it->output);
    set_str(o, "reasoning_json", it->reasoning_json);
    set_str(o, "reasoning_text", it->reasoning_text);
    return o;
}

static char *dup_field(const json_t *o, const char *key)
{
    const char *s = json_string_value(json_object_get(o, key));
    return s ? xstrdup(s) : NULL;
}

int item_from_json(const json_t *obj, struct item *out)
{
    memset(out, 0, sizeof(*out));
    if (!json_is_object(obj))
        return -1;
    enum item_kind k;
    if (kind_from_str(json_string_value(json_object_get(obj, "kind")), &k) < 0)
        return -1;
    out->kind = k;
    out->text = dup_field(obj, "text");
    out->call_id = dup_field(obj, "call_id");
    out->tool_name = dup_field(obj, "tool_name");
    out->tool_arguments_json = dup_field(obj, "arguments");
    out->output = dup_field(obj, "output");
    out->reasoning_json = dup_field(obj, "reasoning_json");
    out->reasoning_text = dup_field(obj, "reasoning_text");
    return 0;
}

/* ---------------- shared helpers ---------------- */

void session_meta_free(struct session_meta *m)
{
    if (!m)
        return;
    free(m->id);
    free(m->cwd);
    free(m->provider);
    free(m->model);
    free(m->reasoning_effort);
    memset(m, 0, sizeof(*m));
}

static int sessions_disabled(void)
{
    const char *e = getenv("HAX_NO_SESSION");
    return e && *e && strcmp(e, "0") != 0;
}

/* Per-cwd directory name: a readable slug plus a hash of the full path,
 * e.g. "Users-me-src.4f1c0a9b8d2e3f01". The slug (leading '/' dropped so
 * the name doesn't start with '-' — which makes it look like an option
 * flag to cd/rm/tar/etc. — remaining '/' folded to '-', capped so deep
 * trees stay within filesystem name limits) is for human eyes only;
 * folding '/' to '-' isn't injective ("/a-b" and "/a/b" both slug to
 * "a-b"), so the FNV-1a hash of the untouched cwd is what actually keys
 * the directory and keeps distinct projects isolated. */
#define CWD_SLUG_MAX 80
static char *encode_cwd(const char *cwd)
{
    if (!cwd || !*cwd)
        cwd = "unknown";

    uint64_t h = 1469598103934665603ULL; /* FNV-1a over the full path */
    for (const char *p = cwd; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }

    const char *s = cwd;
    while (*s == '/')
        s++;
    if (!*s)
        s = "root"; /* cwd was "/" */
    char slug[CWD_SLUG_MAX];
    size_t i = 0;
    for (; s[i] && i < sizeof(slug) - 1; i++)
        slug[i] = s[i] == '/' ? '-' : s[i];
    slug[i] = '\0';

    return xasprintf("%s.%016llx", slug, (unsigned long long)h);
}

/* $XDG_STATE_HOME/hax/sessions/<encoded-cwd>, or NULL when no state dir
 * resolves (neither XDG_STATE_HOME nor HOME set). Caller frees. */
static char *session_dir(const char *cwd)
{
    char *enc = encode_cwd(cwd);
    char *rel = xasprintf("sessions/%s", enc);
    char *dir = xdg_hax_state_path(rel);
    free(rel);
    free(enc);
    return dir;
}

/* Pull the uuid out of a session filename ("<ts>_<uuid>.jsonl"): the part
 * after the last '_', minus the ".jsonl" suffix, validated as a v4-shaped
 * uuid (36 chars, hex with dashes at 8/13/18/23). Returns malloc'd, or
 * NULL when the basename doesn't fit the convention — e.g. a user-supplied
 * --resume=/some/path.jsonl, for which a derived "id" wouldn't resolve in
 * the cwd's session list and so would be a misleading resume hint. Caller
 * frees. */
static int is_uuid(const char *s, size_t len)
{
    if (len != 36)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static char *id_from_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *us = strrchr(base, '_');
    if (!us)
        return NULL;
    const char *id = us + 1;
    size_t len = strlen(id);
    if (len <= 6 || strcmp(id + len - 6, ".jsonl") != 0)
        return NULL;
    len -= 6;
    if (!is_uuid(id, len))
        return NULL;
    char *out = xmalloc(len + 1);
    memcpy(out, id, len);
    out[len] = '\0';
    return out;
}

struct session_log {
    FILE *fp; /* NULL until materialized (fresh sessions); set eagerly on resume */
    char *path;
    int header_written;
    size_t n_written;
    /* Header fields, all owned. Regenerated by session_begin on open/reset. */
    char *id;
    char *cwd;
    char *timestamp; /* ISO-8601 for the header */
    char *provider;
    char *model;
    char *reasoning_effort;
};

/* (Re)compute path/id/cwd/timestamp for a fresh session and clear the
 * write state. Leaves provider/model/effort untouched (session-level
 * config that survives /new). Returns 0, or -1 when no cwd or state dir
 * is available — the caller treats that as "sessions unavailable". */
static int session_begin(struct session_log *log)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return -1;
    char *dir = session_dir(cwd);
    if (!dir)
        return -1;

    char uuid[37];
    gen_uuid_v4(uuid);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts_file[32], ts_iso[32];
    /* Colons aren't filesystem-safe everywhere, so the filename uses
     * dashes; the header carries a proper ISO-8601 timestamp. */
    strftime(ts_file, sizeof(ts_file), "%Y-%m-%dT%H-%M-%SZ", &tm);
    strftime(ts_iso, sizeof(ts_iso), "%Y-%m-%dT%H:%M:%SZ", &tm);

    free(log->path);
    log->path = xasprintf("%s/%s_%s.jsonl", dir, ts_file, uuid);
    free(dir);
    free(log->id);
    log->id = xstrdup(uuid);
    free(log->cwd);
    log->cwd = xstrdup(cwd);
    free(log->timestamp);
    log->timestamp = xstrdup(ts_iso);
    log->fp = NULL;
    log->header_written = 0;
    log->n_written = 0;
    return 0;
}

static FILE *open_session_file(const char *path, int append);

struct session_log *session_log_open(const char *provider, const char *model,
                                     const char *reasoning_effort)
{
    if (sessions_disabled())
        return NULL;
    struct session_log *log = xcalloc(1, sizeof(*log));
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->reasoning_effort = reasoning_effort ? xstrdup(reasoning_effort) : NULL;
    if (session_begin(log) < 0) {
        session_log_close(log);
        return NULL;
    }
    return log;
}

struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *reasoning_effort, size_t n_loaded)
{
    if (sessions_disabled())
        return NULL;
    FILE *fp = open_session_file(path, 1);
    if (!fp) {
        /* Readable enough to load but not appendable (perms changed, gone,
         * read-only mount): the conversation resumes in memory but new
         * turns won't be recorded. Rare, but say so rather than silently
         * dropping persistence for the rest of the run. */
        hax_warn("cannot append to session '%s'; this run won't be recorded", path);
        return NULL;
    }
    struct session_log *log = xcalloc(1, sizeof(*log));
    log->fp = fp;
    log->path = xstrdup(path);
    log->id = id_from_path(path); /* for the resume hint; NULL if non-standard name */
    log->header_written = 1;      /* the resumed file already has its header */
    log->n_written = n_loaded;
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->reasoning_effort = reasoning_effort ? xstrdup(reasoning_effort) : NULL;
    return log;
}

/* Open a session file owner-only (0600). Conversation logs hold prompts,
 * tool output, and env dumps, so they must not be world-readable the way
 * fopen()'s 0666 & ~umask default leaves them. O_CLOEXEC mirrors the 'e'
 * fopen flag; fchmod tightens a pre-existing file too (best-effort —
 * ignored if we don't own it). `append` continues an existing file
 * (resume), else truncates (fresh). Line-buffered like the transcript log
 * so a tail -f sees each block. Returns NULL on failure. */
static FILE *open_session_file(const char *path, int append)
{
    /* Append needs O_RDWR (not O_WRONLY) so the torn-line check below can
     * pread the last byte; a write-only fd would fail that read. */
    int flags = O_CREAT | O_CLOEXEC | (append ? (O_RDWR | O_APPEND) : (O_WRONLY | O_TRUNC));
    int fd = open(path, flags, 0600);
    if (fd < 0)
        return NULL;
    fchmod(fd, 0600);
    if (append) {
        /* A crash can leave the last record half-written with no trailing
         * newline; appending onto it would fuse two JSON objects into one
         * corrupt line. Terminate the torn record first (pread reads at an
         * absolute offset regardless of O_APPEND) so the next record starts
         * clean — the fragment becomes its own line, skipped on load. */
        struct stat st;
        char last;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && pread(fd, &last, 1, st.st_size - 1) == 1 &&
            last != '\n') {
            ssize_t w = write(fd, "\n", 1);
            (void)w;
        }
    }
    FILE *fp = fdopen(fd, append ? "a" : "w");
    if (!fp) {
        close(fd);
        return NULL;
    }
    setvbuf(fp, NULL, _IOLBF, 0);
    return fp;
}

/* Lazily create the directory + file. Returns 0 when fp is ready, -1
 * when the session can't be materialized (no path, mkdir/open failed) —
 * callers then silently drop the append, same posture as the transcript
 * log on a dead file. */
static int ensure_open(struct session_log *log)
{
    if (log->fp)
        return 0;
    if (!log->path)
        return -1;
    char *slash = strrchr(log->path, '/');
    if (slash) {
        *slash = '\0';
        int rc = fs_mkdir_p(log->path);
        if (rc == 0) {
            /* Tighten to owner-only: the per-cwd dir name is the project
             * path, so a 0755 sessions tree would leak which projects this
             * user works on. Best-effort — ignore chmod failures. */
            chmod(log->path, 0700); /* the <encoded-cwd> dir */
            char *s2 = strrchr(log->path, '/');
            if (s2) {
                *s2 = '\0';
                chmod(log->path, 0700); /* the sessions/ dir */
                *s2 = '/';
            }
        }
        *slash = '/';
        if (rc < 0)
            return -1;
    }
    FILE *fp = open_session_file(log->path, 0);
    if (!fp)
        return -1;
    log->fp = fp;
    return 0;
}

static void write_header(struct session_log *log)
{
    json_t *h = json_object();
    json_object_set_new(h, "type", json_string("session"));
    json_object_set_new(h, "version", json_integer(SESSION_FORMAT_VERSION));
    set_str(h, "id", log->id);
    set_str(h, "timestamp", log->timestamp);
    set_str(h, "cwd", log->cwd);
    set_str(h, "provider", log->provider);
    set_str(h, "model", log->model);
    set_str(h, "reasoning_effort", log->reasoning_effort);
    char *s = json_dumps(h, JSON_COMPACT);
    if (s) {
        fputs(s, log->fp);
        fputc('\n', log->fp);
        free(s);
    }
    json_decref(h);
}

void session_log_append(struct session_log *log, const struct item *items, size_t n_items)
{
    if (!log || n_items <= log->n_written)
        return;
    if (ensure_open(log) < 0)
        return;
    if (!log->header_written) {
        write_header(log);
        log->header_written = 1;
    }
    for (size_t i = log->n_written; i < n_items; i++) {
        json_t *o = item_to_json(&items[i]);
        /* Stamp reasoning with the model that produced it (this run's
         * provider+model). The opaque half (reasoning_json) is bound to
         * that exact model — an encrypted/signed CoT blob is rejected by
         * any other backend — so a later resume under a different model
         * can clear just the opaque half while keeping the portable text,
         * even though a resumed file mixes models under one header. */
        if (items[i].kind == ITEM_REASONING) {
            set_str(o, "provider", log->provider);
            set_str(o, "model", log->model);
        }
        char *s = json_dumps(o, JSON_COMPACT);
        if (s) {
            fputs(s, log->fp);
            fputc('\n', log->fp);
            free(s);
        }
        json_decref(o);
    }
    log->n_written = n_items;
}

void session_log_reset(struct session_log *log)
{
    if (!log)
        return;
    if (log->fp) {
        fclose(log->fp);
        log->fp = NULL;
    }
    if (session_begin(log) < 0) {
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
    if (log->fp)
        fclose(log->fp);
    free(log->path);
    free(log->id);
    free(log->cwd);
    free(log->timestamp);
    free(log->provider);
    free(log->model);
    free(log->reasoning_effort);
    free(log);
}

const char *session_log_path(const struct session_log *log)
{
    return log ? log->path : NULL;
}

const char *session_log_resume_hint(const struct session_log *log)
{
    /* header_written means the file exists on disk — only then is there a
     * session to point a --resume at. A fresh, never-written session has
     * an id assigned but no file, so report nothing. Resumable sessions
     * always live in the current cwd's directory (--resume is per-cwd and
     * has no path form), so the bare id always resolves. */
    if (!log || !log->header_written)
        return NULL;
    return log->id;
}

/* ---------------- loading ---------------- */

static void push_item(struct item **items, size_t *n, size_t *cap, struct item it)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        *items = xrealloc(*items, c * sizeof(struct item));
        *cap = c;
    }
    (*items)[(*n)++] = it;
}

int session_load(const char *path, const char *cur_provider, const char *cur_model,
                 struct item **out_items, size_t *out_n, struct session_meta *out_meta)
{
    size_t len;
    char *data = slurp_file(path, &len);
    if (!data) {
        *out_items = NULL;
        *out_n = 0;
        return -1;
    }
    if (out_meta)
        memset(out_meta, 0, sizeof(*out_meta));

    struct item *items = NULL;
    size_t n = 0, cap = 0;
    /* Header provider/model are the fallback origin for reasoning items
     * that predate per-item stamping; per-item fields (when present) win. */
    char *hdr_provider = NULL;
    char *hdr_model = NULL;

    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line)
            continue;
        json_t *o = json_loads(line, 0, NULL);
        if (!o)
            continue; /* tolerate a stray/partial line rather than abort */

        const char *type = json_string_value(json_object_get(o, "type"));
        if (type && strcmp(type, "session") == 0) {
            if (!hdr_provider)
                hdr_provider = dup_field(o, "provider");
            if (!hdr_model)
                hdr_model = dup_field(o, "model");
            if (out_meta) {
                out_meta->id = dup_field(o, "id");
                out_meta->cwd = dup_field(o, "cwd");
                out_meta->provider = dup_field(o, "provider");
                out_meta->model = dup_field(o, "model");
                out_meta->reasoning_effort = dup_field(o, "reasoning_effort");
            }
            json_decref(o);
            continue;
        }

        struct item it;
        if (item_from_json(o, &it) == 0) {
            /* The opaque half of reasoning (reasoning_json) round-trips
             * only to the exact model that produced it; clear it on a
             * model switch but keep the portable text. Drop the item only
             * if nothing replayable is left (an encrypted-only blob under
             * a different model). cur_provider == NULL means "load
             * verbatim" (diagnostics / full restore). */
            if (it.kind == ITEM_REASONING && cur_provider && it.reasoning_json) {
                const char *ip = json_string_value(json_object_get(o, "provider"));
                const char *im = json_string_value(json_object_get(o, "model"));
                const char *prov = ip ? ip : hdr_provider;
                const char *mdl = im ? im : hdr_model;
                int same_model = cur_model && prov && mdl && strcmp(prov, cur_provider) == 0 &&
                                 strcmp(mdl, cur_model) == 0;
                if (!same_model) {
                    free(it.reasoning_json);
                    it.reasoning_json = NULL;
                }
            }
            if (it.kind == ITEM_REASONING && !it.reasoning_json &&
                (!it.reasoning_text || !it.reasoning_text[0]))
                item_free(&it); /* nothing left to replay */
            else
                push_item(&items, &n, &cap, it);
        }
        json_decref(o);
    }
    free(data);
    free(hdr_provider);
    free(hdr_model);

    /* Drop any tool_call with no matching tool_result. A crash between a
     * flushed tool_call line and its result line leaves a dangling call,
     * which Chat Completions backends reject as malformed on the first
     * request after resume. A cleanly-ended session always pairs them
     * (Esc/error synthesize [interrupted] results), so this only ever
     * trims crash debris. */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == ITEM_TOOL_CALL) {
            int paired = 0;
            for (size_t j = 0; items[i].call_id && j < n; j++)
                if (items[j].kind == ITEM_TOOL_RESULT && items[j].call_id &&
                    strcmp(items[j].call_id, items[i].call_id) == 0) {
                    paired = 1;
                    break;
                }
            if (!paired) {
                item_free(&items[i]);
                continue;
            }
        }
        items[w++] = items[i];
    }
    n = w;

    *out_items = items;
    *out_n = n;
    return 0;
}

/* ---------------- listing ---------------- */

#define FIRST_PROMPT_CELLS 64

/* How much of a session file to read when extracting the first prompt.
 * The first user message sits within the first few lines (header,
 * boundary, user), so a bounded prefix avoids slurping a huge file (one
 * line can be a multi-megabyte tool result) just to label a picker row. */
#define FIRST_PROMPT_SCAN_CAP (64 * 1024)

char *session_first_prompt(const char *path)
{
    size_t len;
    int truncated;
    char *data = slurp_file_capped(path, FIRST_PROMPT_SCAN_CAP, &len, &truncated);
    if (!data)
        return NULL;
    char *result = NULL;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line)
            continue;
        json_t *o = json_loads(line, 0, NULL);
        if (!o)
            continue; /* a partial last line from the cap, or the header */
        const char *kind = json_string_value(json_object_get(o, "kind"));
        if (kind && strcmp(kind, "user") == 0) {
            const char *txt = json_string_value(json_object_get(o, "text"));
            if (txt) {
                char *flat = flatten_for_display(txt);
                result = truncate_for_display(flat, FIRST_PROMPT_CELLS);
                free(flat);
            }
            json_decref(o);
            break; /* first user message is all we want */
        }
        json_decref(o);
    }
    free(data);
    return result;
}

static int cmp_mtime_desc(const void *a, const void *b)
{
    const struct session_entry *ea = a, *eb = b;
    if (ea->mtime != eb->mtime)
        return ea->mtime < eb->mtime ? 1 : -1;
    /* Same second: break the tie on sub-second mtime so two quick runs
     * still order by recency rather than arbitrarily. */
    if (ea->mtime_nsec != eb->mtime_nsec)
        return ea->mtime_nsec < eb->mtime_nsec ? 1 : -1;
    return 0;
}

static int has_jsonl_suffix(const char *name)
{
    size_t l = strlen(name);
    return l >= 6 && strcmp(name + l - 6, ".jsonl") == 0;
}

int session_list(const char *cwd, struct session_entry **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    char *dir = session_dir(cwd);
    if (!dir)
        return 0;
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return 0; /* no directory yet == no sessions */
    }

    struct session_entry *list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!has_jsonl_suffix(de->d_name))
            continue;
        char *fpath = xasprintf("%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(fpath);
            continue;
        }
        /* Enumeration only — no file reads here. The id is in the
         * filename; the first prompt is filled lazily by the picker for
         * just the rows it shows (session_first_prompt), so --continue and
         * --resume=ID never touch a transcript. */
        struct session_entry e;
        memset(&e, 0, sizeof(e));
        e.path = fpath;
        e.mtime = (long)st.st_mtime;
        e.mtime_nsec = ST_MTIME_NSEC(st);
        e.id = id_from_path(fpath);
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            list = xrealloc(list, cap * sizeof(*list));
        }
        list[n++] = e;
    }
    closedir(d);
    free(dir);

    qsort(list, n, sizeof(*list), cmp_mtime_desc);
    *out = list;
    *n_out = n;
    return 0;
}

void session_list_free(struct session_entry *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(list[i].path);
        free(list[i].id);
        free(list[i].first_prompt);
    }
    free(list);
}
