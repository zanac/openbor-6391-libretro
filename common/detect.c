/*
 * OpenBOR multi-engine libretro core - engine version detection.
 *
 * Reads the .pak directly (independently of any engine) and collects what the
 * mod uses: models.txt / levels.txt commands, the commands of every model
 * and level file they reference, and from the scripts the builtin functions,
 * openborconstant() names and quoted property/variant names. Each engine's
 * vocabulary (detect_tables.h, generated from the engine sources) tells which
 * of those it does not understand.
 *
 * The engine with the fewest unknown items wins; on a tie the OLDEST one is
 * chosen, since it is the closest to what the mod was written for (newer
 * engines change behaviour in places). Items no engine knows (typos, dead
 * commands) are ignored.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "detect.h"
#include "detect_tables.h"

#define MAX_TOKENS   8192
#define MAX_FILES    4096

enum { CAT_MODELSTXT, CAT_MODEL, CAT_LEVEL, CAT_LEVELORDER, CAT_BUILTIN, CAT_CONST, CAT_PROP, CAT_COUNT };
static const char *const cat_names[CAT_COUNT] = {
    "models.txt command", "model command", "level command", "levels.txt command",
    "script function", "script constant", "script property"
};

typedef struct { char *name; uint32_t start, size; } pak_entry;

typedef struct
{
    FILE *f;
    pak_entry *entries;
    int count;
} pak_t;

typedef struct { unsigned char cat; char *tok; } token_t;

typedef struct
{
    token_t *tokens;
    int count, cap;
    char **files;          /* model/level/script files queued for scanning */
    unsigned char *fcat;
    int nfiles, fcap;
    char **userfuncs;      /* functions defined by the mod's scripts */
    int sab_calls;         /* calls of SAB extension functions (sab_*) */
    int nuser, ucap;
    int video_wxh;         /* video.txt uses "video WxH" */
} scan_t;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void normalize_path(char *p)
{
    for (; *p; p++)
    {
        if (*p == '\\') *p = '/';
        *p = (char)tolower((unsigned char)*p);
    }
}

static uint32_t rd32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ------------------------------------------------------------------------ */
/* .pak reader                                                              */
/* ------------------------------------------------------------------------ */

static int pak_open(pak_t *pak, const char *path)
{
    unsigned char hdr[8], b[12];
    long size, pos;
    uint32_t dir;

    memset(pak, 0, sizeof(*pak));
    pak->f = fopen(path, "rb");
    if (!pak->f) return 0;
    if (fread(hdr, 1, 8, pak->f) != 8 || (memcmp(hdr, "PACK", 4) && memcmp(hdr, "\x1d\x11\x14\x7f", 4)) || rd32(hdr + 4) != 0)
        goto fail;
    if (fseek(pak->f, -4, SEEK_END) || (size = ftell(pak->f) + 4) < 12)
        goto fail;
    if (fread(b, 1, 4, pak->f) != 4)
        goto fail;
    dir = rd32(b);
    if (dir < 8 || (long)dir >= size || fseek(pak->f, (long)dir, SEEK_SET))
        goto fail;

    pos = dir;
    while (pos + 12 < size - 4 && pak->count < 1000000)
    {
        uint32_t len, start, fsize;
        char name[1024];
        size_t nlen;

        if (fread(b, 1, 12, pak->f) != 12) break;
        len = rd32(b); start = rd32(b + 4); fsize = rd32(b + 8);
        if (len <= 12 || len > 12 + sizeof(name)) break;
        nlen = len - 12;
        if (fread(name, 1, nlen, pak->f) != nlen) break;
        name[nlen - 1] = 0;
        name[strnlen(name, nlen)] = 0;
        pos += len;

        if ((pak->count & 1023) == 0)
        {
            pak_entry *n = (pak_entry *)realloc(pak->entries, (pak->count + 1024) * sizeof(pak_entry));
            if (!n) break;
            pak->entries = n;
        }
        normalize_path(name);
        pak->entries[pak->count].name = xstrdup(name);
        pak->entries[pak->count].start = start;
        pak->entries[pak->count].size = fsize;
        pak->count++;
    }
    return pak->count > 0;

fail:
    fclose(pak->f);
    pak->f = NULL;
    return 0;
}

static void pak_close(pak_t *pak)
{
    int i;
    for (i = 0; i < pak->count; i++) free(pak->entries[i].name);
    free(pak->entries);
    if (pak->f) fclose(pak->f);
    memset(pak, 0, sizeof(*pak));
}

/* Returns a malloc'ed, NUL-terminated copy of the file, or NULL. */
static char *pak_read(pak_t *pak, const char *path)
{
    char want[1024];
    int i;

    snprintf(want, sizeof(want), "%s", path);
    normalize_path(want);
    for (i = 0; i < pak->count; i++)
    {
        if (strcmp(pak->entries[i].name, want) == 0)
        {
            uint32_t n = pak->entries[i].size;
            char *buf;
            if (n > 64 * 1024 * 1024) return NULL;
            buf = (char *)malloc(n + 1);
            if (!buf) return NULL;
            if (fseek(pak->f, (long)pak->entries[i].start, SEEK_SET) || fread(buf, 1, n, pak->f) != n)
            {
                free(buf);
                return NULL;
            }
            buf[n] = 0;
            return buf;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Token collection                                                         */
/* ------------------------------------------------------------------------ */

static void add_token(scan_t *s, int cat, const char *tok)
{
    int i;
    char low[128];

    if (!tok[0] || strlen(tok) >= sizeof(low)) return;
    for (i = 0; tok[i]; i++)
        low[i] = (char)(cat == CAT_CONST ? toupper((unsigned char)tok[i]) : tolower((unsigned char)tok[i]));
    low[i] = 0;

    for (i = 0; i < s->count; i++)
        if (s->tokens[i].cat == cat && strcmp(s->tokens[i].tok, low) == 0)
            return;
    if (s->count >= MAX_TOKENS) return;
    if (s->count == s->cap)
    {
        token_t *n = (token_t *)realloc(s->tokens, (s->cap + 256) * sizeof(token_t));
        if (!n) return;
        s->tokens = n;
        s->cap += 256;
    }
    s->tokens[s->count].cat = (unsigned char)cat;
    s->tokens[s->count].tok = xstrdup(low);
    s->count++;
}

static void queue_file(scan_t *s, const char *path, int cat)
{
    int i;
    char p[1024];

    snprintf(p, sizeof(p), "%s", path);
    normalize_path(p);
    for (i = 0; i < s->nfiles; i++)
        if (strcmp(s->files[i], p) == 0) return;
    if (s->nfiles >= MAX_FILES) return;
    if (s->nfiles == s->fcap)
    {
        char **nf = (char **)realloc(s->files, (s->fcap + 256) * sizeof(char *));
        unsigned char *nc;
        if (!nf) return;
        s->files = nf;
        nc = (unsigned char *)realloc(s->fcat, s->fcap + 256);
        if (!nc) return;
        s->fcat = nc;
        s->fcap += 256;
    }
    s->files[s->nfiles] = xstrdup(p);
    s->fcat[s->nfiles] = (unsigned char)cat;
    s->nfiles++;
}

static void add_userfunc(scan_t *s, const char *name)
{
    if (s->nuser == s->ucap)
    {
        char **n = (char **)realloc(s->userfuncs, (s->ucap + 256) * sizeof(char *));
        if (!n) return;
        s->userfuncs = n;
        s->ucap += 256;
    }
    s->userfuncs[s->nuser++] = xstrdup(name);
}

/* Splits one text line into up to `max` whitespace separated words
 * (OpenBOR txt syntax: '#' starts a comment). */
static int split_words(char *line, char **w, int max)
{
    int n = 0;
    char *p = line, *hash = strchr(line, '#');
    if (hash) *hash = 0;
    while (*p && n < max)
    {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        w[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static void scan_script(scan_t *s, const char *src);

/* Scans an OpenBOR text file (models.txt, levels.txt, model or level file). */
static void scan_txt(scan_t *s, char *buf, int cat)
{
    char *line = buf, *next;
    char *script_start = NULL;

    while (line && *line)
    {
        char *w[4];
        int n;
        char copy[2048];

        next = strpbrk(line, "\r\n");
        if (next)
        {
            *next++ = 0;
            while (*next == '\r' || *next == '\n') next++;
        }

        /* inline script blocks: @script ... @end_script, @cmd ... */
        if (script_start)
        {
            if (strstr(line, "@end_script"))
            {
                char *e = strstr(line, "@end_script");
                *e = 0;
                scan_script(s, line);
                script_start = NULL;
            }
            else
                scan_script(s, line);
            line = next;
            continue;
        }
        if (strstr(line, "@script"))
        {
            char *after = strstr(line, "@script") + 7;
            char *e = strstr(after, "@end_script");
            if (e) { *e = 0; scan_script(s, after); }
            else { scan_script(s, after); script_start = after; }
            line = next;
            continue;
        }
        if (strstr(line, "@cmd"))
        {
            scan_script(s, strstr(line, "@cmd") + 4);
            line = next;
            continue;
        }

        snprintf(copy, sizeof(copy), "%s", line);
        n = split_words(copy, w, 4);
        if (n > 0)
        {
            add_token(s, cat, w[0]);

            if (cat == CAT_MODELSTXT && n >= 3 &&
                (!strcasecmp(w[0], "load") || !strcasecmp(w[0], "know")))
                queue_file(s, w[2], CAT_MODEL);
            else if (cat == CAT_LEVELORDER && n >= 2 && !strcasecmp(w[0], "file"))
                queue_file(s, w[1], CAT_LEVEL);
            else if (n >= 2 && strlen(w[0]) > 6 && !strcasecmp(w[0] + strlen(w[0]) - 6, "script"))
                queue_file(s, w[1], CAT_BUILTIN); /* animationscript, onspawnscript, ... */
        }
        line = next;
    }
}

static int is_c_keyword(const char *w)
{
    static const char *const kw[] = {
        "if", "else", "while", "for", "do", "switch", "case", "return", "break",
        "continue", "sizeof", "void", "int", "float", "char", "long", "double",
        "short", "signed", "unsigned", "static", "const", "struct", "default",
        "goto", "import", "include", "define", NULL
    };
    int i;
    for (i = 0; kw[i]; i++)
        if (!strcmp(w, kw[i])) return 1;
    return 0;
}

/* Functions whose quoted arguments are property / variant names */
static int is_property_function(const char *f)
{
    char low[64];
    size_t i;
    if (strlen(f) >= sizeof(low)) return 0;
    for (i = 0; f[i]; i++) low[i] = (char)tolower((unsigned char)f[i]);
    low[i] = 0;
    return strstr(low, "property") || strstr(low, "variant");
}

/* Collects builtin calls, openborconstant() names, property names quoted as
 * arguments of property/variant functions, and the names of functions the
 * script defines itself. */
static void scan_script(scan_t *s, const char *src)
{
    const char *p = src;
    char word[128], prev[128] = "";
    char calls[32][128];         /* function name for each open parenthesis */
    int depth = 0;

    while (*p)
    {
        if (p[0] == '/' && p[1] == '/')
        {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*')
        {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        if (*p == '"')
        {
            int n = 0;
            p++;
            while (*p && *p != '"' && *p != '\n')
            {
                if (*p == '\\' && p[1]) p++;
                if (n < (int)sizeof(word) - 1) word[n++] = *p;
                p++;
            }
            word[n] = 0;
            if (*p == '"') p++;
            if (depth > 0 && depth <= 32)
            {
                const char *fn = calls[depth - 1];
                if (!strcasecmp(fn, "openborconstant"))
                    add_token(s, CAT_CONST, word);
                else if (n > 0 && n < 48 && is_property_function(fn))
                    add_token(s, CAT_PROP, word);
            }
            prev[0] = 0;
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_')
        {
            int n = 0;
            const char *q;
            while ((isalnum((unsigned char)*p) || *p == '_') && n < (int)sizeof(word) - 1)
                word[n++] = *p++;
            word[n] = 0;
            for (q = p; *q == ' ' || *q == '\t'; q++) ;
            if (*q == '(' && !is_c_keyword(word))
            {
                /* "type name(" at the start of a declaration = user function */
                if (!strcmp(prev, "void") || !strcmp(prev, "int") || !strcmp(prev, "float") ||
                    !strcmp(prev, "char") || !strcmp(prev, "long") || !strcmp(prev, "double"))
                    add_userfunc(s, word);
                else if (!strncmp(word, "sab_", 4))
                    s->sab_calls++;     /* SAB extensions: engine 8025_webm only */
                else
                    add_token(s, CAT_BUILTIN, word);
                if (depth < 32) snprintf(calls[depth], sizeof(calls[depth]), "%s", word);
                depth++;
                p = q + 1;
                prev[0] = 0;
                continue;
            }
            snprintf(prev, sizeof(prev), "%s", word);
            continue;
        }
        if (*p == '(')
        {
            if (depth < 32) calls[depth][0] = 0;
            depth++;
        }
        else if (*p == ')')
        {
            if (depth > 0) depth--;
        }
        else if (*p == ';' || *p == '{' || *p == '}')
            depth = 0;
        if (!isspace((unsigned char)*p))
            prev[0] = 0;
        p++;
    }
}

/* ------------------------------------------------------------------------ */
/* Matching                                                                 */
/* ------------------------------------------------------------------------ */

static int vocab_has(const char *const *list, const char *tok)
{
    int i;
    for (i = 0; list[i]; i++)
    {
        const char *e = list[i];
        size_t n = strlen(e);
        if (n && e[n - 1] == '#')
        {
            /* prefix followed by digits */
            const char *d;
            if (strncmp(tok, e, n - 1)) continue;
            d = tok + n - 1;
            if (!*d) continue;
            while (*d && isdigit((unsigned char)*d)) d++;
            if (!*d) return 1;
        }
        else if (!strcmp(e, tok))
            return 1;
    }
    return 0;
}

static const char *const *vocab_list(const ob_vocab *v, int cat)
{
    switch (cat)
    {
    case CAT_MODELSTXT:  return v->modelstxt;
    case CAT_MODEL:      return v->model;
    case CAT_LEVEL:      return v->level;
    case CAT_LEVELORDER: return v->levelorder;
    case CAT_BUILTIN:    return v->builtin;
    case CAT_CONST:      return v->constant;
    default:             return v->property;
    }
}

static const ob_vocab *find_vocab(int id)
{
    int i;
    for (i = 0; i < OB_NUM_VOCABS; i++)
        if (ob_vocabs[i].id == id) return &ob_vocabs[i];
    return NULL;
}

static int is_userfunc(const scan_t *s, const char *tok)
{
    int i;
    for (i = 0; i < s->nuser; i++)
        if (!strcasecmp(s->userfuncs[i], tok)) return 1;
    return 0;
}

static void scan_free(scan_t *s)
{
    int i;
    for (i = 0; i < s->count; i++) free(s->tokens[i].tok);
    for (i = 0; i < s->nfiles; i++) free(s->files[i]);
    for (i = 0; i < s->nuser; i++) free(s->userfuncs[i]);
    free(s->tokens);
    free(s->files);
    free(s->fcat);
    free(s->userfuncs);
}

void ob_detect_version(const char *pak_path, const int *ids, int count, ob_detect_result *out)
{
    pak_t pak;
    scan_t s;
    char *buf;
    int i, v, best = -1, best_unknown = 0;
    int unknown[16];
    char example[16][160];

    out->version = count ? ids[count - 1] : -1;
    out->webm_files = 0;
    out->sab_calls = 0;
    snprintf(out->reason, sizeof(out->reason), "could not read the pak, using the newest engine");
    if (count <= 0 || count > 16 || !pak_open(&pak, pak_path))
        return;

    memset(&s, 0, sizeof(s));

    for (i = 0; i < pak.count; i++)
    {
        size_t n = strlen(pak.entries[i].name);
        if (n > 5 && !strcmp(pak.entries[i].name + n - 5, ".webm"))
            out->webm_files++;
    }

    if ((buf = pak_read(&pak, "data/video.txt")))
    {
        char *line = buf;
        while (line && *line)
        {
            char copy[256], *w[3];
            char *next = strpbrk(line, "\r\n");
            if (next) *next++ = 0;
            snprintf(copy, sizeof(copy), "%s", line);
            if (split_words(copy, w, 3) >= 2 && !strcasecmp(w[0], "video") && strchr(w[1], 'x'))
                s.video_wxh = 1;
            line = next;
        }
        free(buf);
    }

    if ((buf = pak_read(&pak, "data/models.txt")))
    {
        scan_txt(&s, buf, CAT_MODELSTXT);
        free(buf);
    }
    if ((buf = pak_read(&pak, "data/levels.txt")))
    {
        scan_txt(&s, buf, CAT_LEVELORDER);
        free(buf);
    }
    /* queued model / level / script files (the queue grows while scanning) */
    for (i = 0; i < s.nfiles; i++)
    {
        if (!(buf = pak_read(&pak, s.files[i])))
            continue;
        if (s.fcat[i] == CAT_BUILTIN)
            scan_script(&s, buf);
        else
            scan_txt(&s, buf, s.fcat[i]);
        free(buf);
    }
    /* every other script in the pak (data/scripts/..., update scripts...) */
    for (i = 0; i < pak.count; i++)
    {
        size_t n = strlen(pak.entries[i].name);
        if (n > 2 && (!strcmp(pak.entries[i].name + n - 2, ".c") || !strcmp(pak.entries[i].name + n - 2, ".h")))
        {
            int j, done = 0;
            for (j = 0; j < s.nfiles; j++)
                if (!strcmp(s.files[j], pak.entries[i].name)) done = 1;
            if (!done && (buf = pak_read(&pak, pak.entries[i].name)))
            {
                scan_script(&s, buf);
                free(buf);
            }
        }
    }

    /* score */
    for (v = 0; v < count; v++)
    {
        unknown[v] = 0;
        example[v][0] = 0;
    }
    for (i = 0; i < s.count; i++)
    {
        const token_t *t = &s.tokens[i];
        int known_somewhere = 0;
        int known[16];

        if (t->cat == CAT_BUILTIN && is_userfunc(&s, t->tok))
            continue;
        for (v = 0; v < count; v++)
        {
            const ob_vocab *voc = find_vocab(ids[v]);
            known[v] = voc ? vocab_has(vocab_list(voc, t->cat), t->tok) : 1;
            known_somewhere |= known[v];
        }
        if (!known_somewhere)
            continue;
        for (v = 0; v < count; v++)
        {
            if (known[v]) continue;
            unknown[v]++;
            if (strlen(example[v]) < 100)
            {
                char e[96];
                snprintf(e, sizeof(e), "%s%s '%s'", example[v][0] ? ", " : "", cat_names[t->cat], t->tok);
                strncat(example[v], e, sizeof(example[v]) - strlen(example[v]) - 1);
            }
        }
    }
    if (getenv("OB_DETECT_DEBUG"))
    {
        int c, per[CAT_COUNT] = {0};
        for (i = 0; i < s.count; i++) per[s.tokens[i].cat]++;
        fprintf(stderr, "[detect] %d files queued, %d user functions, video WxH %d\n", s.nfiles, s.nuser, s.video_wxh);
        for (c = 0; c < CAT_COUNT; c++) fprintf(stderr, "[detect]   %-20s %d\n", cat_names[c], per[c]);
        for (i = 0; i < s.count; i++)
        {
            const token_t *t = &s.tokens[i];
            char line[256];
            int any = 0, all = 1;
            if (t->cat == CAT_BUILTIN && is_userfunc(&s, t->tok)) continue;
            snprintf(line, sizeof(line), "[detect]   %s '%s':", cat_names[t->cat], t->tok);
            for (v = 0; v < count; v++)
            {
                const ob_vocab *voc = find_vocab(ids[v]);
                int k = voc ? vocab_has(vocab_list(voc, t->cat), t->tok) : 1;
                any |= k; all &= k;
                if (!k) { char b[16]; snprintf(b, sizeof(b), " -%d", ids[v]); strncat(line, b, sizeof(line) - strlen(line) - 1); }
            }
            if (any && !all) fprintf(stderr, "%s\n", line);
        }
    }
    if (s.video_wxh)
        for (v = 0; v < count; v++)
            if (ids[v] < 4000)
            {
                unknown[v]++;
                if (strlen(example[v]) < 100)
                    strncat(example[v], example[v][0] ? ", 'video WxH'" : "'video WxH'",
                            sizeof(example[v]) - strlen(example[v]) - 1);
            }

    for (v = 0; v < count; v++)
        if (best < 0 || unknown[v] < best_unknown)
        {
            best = v;
            best_unknown = unknown[v];
        }

    out->version = ids[best];
    if (best > 0)
        snprintf(out->reason, sizeof(out->reason), "%d items need a newer engine than %d, e.g. %s",
                 unknown[best - 1], ids[best - 1], example[best - 1]);
    else
        snprintf(out->reason, sizeof(out->reason), "oldest engine that understands everything the mod uses%s",
                 best_unknown ? " (some items unknown to every engine)" : "");
    if (best_unknown)
    {
        size_t l = strlen(out->reason);
        snprintf(out->reason + l, sizeof(out->reason) - l, "; still %d unknown to %d: %s",
                 best_unknown, ids[best], example[best]);
    }
    /* SAB extensions exist only in build 8025 (as 8025_webm) */
    out->sab_calls = s.sab_calls;
    if (s.sab_calls > 0)
        for (v = 0; v < count; v++)
            if (ids[v] == 8025)
            {
                out->version = 8025;
                snprintf(out->reason, sizeof(out->reason), "the scripts call %d SAB extension functions (sab_*)",
                         s.sab_calls);
            }

    scan_free(&s);
    pak_close(&pak);
}
