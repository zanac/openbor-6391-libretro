/*
 * SAB - native script extensions of the 8025_webm engine
 * ======================================================
 *
 * Generic helpers in C for script-driven OpenBOR mods (written for the
 * "Slaps And Beans" conversion, usable by any mod). Compiled only into the
 * 8025_webm engine (-DSAB); every function name starts with "sab_" so no
 * existing mod is affected. This file is #included at the end of
 * openborscript.c (it uses the script variant and array internals);
 * sab_register_functions() is called by Script_LoadSystemFunctions().
 *
 * Reference: engines/8025/sab/README.md.
 *
 *   Profiler
 *     sab_profile(on)                 1: reset and start, 0: stop
 *     sab_profile_dump(n)             log the n script/native functions with
 *                                     the most self time
 *   Navigation map (free walkable area: z intervals per x column)
 *     sab_nav_load(path)              handle (cached per path) or 0
 *     sab_nav_free(h)                 forget a map (the next load re-reads it)
 *     sab_nav_sample(h, x, y, z, maxd, climb, out)
 *                                     nearest walkable point (out[0..2]); 1/0
 *     sab_nav_floor(h, x, y, z)       floor height under (x, z) at most 1
 *                                     above y; y when none
 *     sab_nav_raycast(h, ax, az, bx, bz, y, step, out)
 *                                     march a -> b: 1 when blocked, out[0..1]
 *                                     = last walkable point (b when clear)
 *     sab_nav_patch(h, name, on)      switch a named patch
 *   Camera
 *     sab_camera(on, x, y)            1: the engine's scroll is replaced by
 *                                     (x, y) every frame (level position
 *                                     follows x, so spawns "at" still work);
 *                                     0: back to the engine's scrolling
 *   Post effects (32-bit screens; what is queued below the split z is
 *   graded, the rest - HUD, texts - is drawn on top unchanged)
 *     sab_postfx_load(lut, curves)    a colour LUT file (N^3 RGB bytes, node
 *                                     (r,g,b) at ((b*N + g)*N + r)*3) applied
 *                                     before the bloom, and an optional
 *                                     per-channel table (3 x 256 bytes, R G B)
 *                                     applied after it ("" = none); 1/0
 *     sab_postfx(on, curves)          enable; curves 0 skips the table
 *     sab_postfx_bloom(intensity, threshold)
 *                                     Unity Standard Assets Bloom (High,
 *                                     2 blur iterations, spread 2.5, Add);
 *                                     intensity 0 = off
 *     sab_postfx_zsplit(z)            sprite-queue z where the grading stops
 *                                     (default HUD_Z)
 */

#include <time.h>
#include <math.h>
#include "sab/sab_prof.h"

/* ================================================================ helpers */

static int sab_arg_double(ScriptVariant **v, int i, int n, DOUBLE *out)
{
    if(i >= n || !v[i])
    {
        return 0;
    }
    return SUCCEEDED(ScriptVariant_DecimalValue(v[i], out));
}

static int sab_arg_long(ScriptVariant **v, int i, int n, LONG *out)
{
    if(i >= n || !v[i])
    {
        return 0;
    }
    return SUCCEEDED(ScriptVariant_IntegerValue(v[i], out));
}

static const char *sab_arg_str(ScriptVariant **v, int i, int n)
{
    if(i >= n || !v[i] || v[i]->vt != VT_STR)
    {
        return NULL;
    }
    return StrCache_Get(v[i]->strVal);
}

static Varlist *sab_arg_array(ScriptVariant **v, int i, int n)
{
    Varlist *a;

    if(i >= n || !v[i] || v[i]->vt != VT_PTR)
    {
        return NULL;
    }
    a = (Varlist *)v[i]->ptrVal;
    if(!a || a->magic != varlist_magic)
    {
        return NULL;
    }
    return a;
}

static void sab_array_set_double(Varlist *a, int index, double value)
{
    ScriptVariant tmp;

    ScriptVariant_Init(&tmp);
    ScriptVariant_ChangeType(&tmp, VT_DECIMAL);
    tmp.dblVal = value;
    Varlist_SetByIndex(a, index, &tmp);
}

static void sab_ret_long(ScriptVariant **pretvar, LONG value)
{
    ScriptVariant_ChangeType(*pretvar, VT_INTEGER);
    (*pretvar)->lVal = value;
}

static void sab_ret_double(ScriptVariant **pretvar, double value)
{
    ScriptVariant_ChangeType(*pretvar, VT_DECIMAL);
    (*pretvar)->dblVal = value;
}

/* =============================================================== profiler */

#define SAB_PROF_MAX   4096
#define SAB_PROF_STACK 256

typedef struct
{
    const char *name;
    unsigned long calls;
    double total;           /* inclusive seconds */
    double self;            /* exclusive seconds */
} sab_prof_entry;

typedef struct
{
    int entry;
    double start;
    double child;
} sab_prof_frame;

int sab_prof_on = 0;
static sab_prof_entry sab_prof_tab[SAB_PROF_MAX];
static int sab_prof_count = 0;
static sab_prof_frame sab_prof_stack[SAB_PROF_STACK];
static int sab_prof_depth = 0;
static double sab_prof_t0 = 0;

static double sab_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Function name -> table entry (open addressing on an FNV-1a hash of the text). */
#define SAB_PROF_HASH 8192
static int sab_prof_hash[SAB_PROF_HASH];

static int sab_prof_find(const char *name)
{
    unsigned h = 2166136261u;
    const unsigned char *p;
    int i, k;

    for(p = (const unsigned char *)name; *p; p++)
    {
        h = (h ^ *p) * 16777619u;
    }
    for(i = 0; i < SAB_PROF_HASH; i++)
    {
        k = (h + i) & (SAB_PROF_HASH - 1);
        if(!sab_prof_hash[k])
        {
            if(sab_prof_count >= SAB_PROF_MAX)
            {
                return -1;
            }
            sab_prof_hash[k] = sab_prof_count + 1;
            sab_prof_tab[sab_prof_count].name = strdup(name);
            sab_prof_tab[sab_prof_count].calls = 0;
            sab_prof_tab[sab_prof_count].total = 0;
            sab_prof_tab[sab_prof_count].self = 0;
            return sab_prof_count++;
        }
        if(!strcmp(sab_prof_tab[sab_prof_hash[k] - 1].name, name))
        {
            return sab_prof_hash[k] - 1;
        }
    }
    return -1;
}

/* Call target pointer -> entry (cache in front of the name table). */
#define SAB_PROF_PTRS 8192
static const void *sab_prof_ptr_key[SAB_PROF_PTRS];
static int sab_prof_ptr_val[SAB_PROF_PTRS];

static int sab_prof_entry_of(Interpreter *pi, Instruction *call)
{
    const void *key = call->ptheJumpTarget ? (const void *)call->ptheJumpTarget : (const void *)call->functionRef;
    unsigned h = (unsigned)(((uintptr_t)key >> 3) * 2654435761u);
    int i, k, e;
    char buf[64];
    const char *name = NULL;

    for(i = 0; i < SAB_PROF_PTRS; i++)
    {
        k = (h + i) & (SAB_PROF_PTRS - 1);
        if(sab_prof_ptr_key[k] == key)
        {
            return sab_prof_ptr_val[k];
        }
        if(!sab_prof_ptr_key[k])
        {
            break;
        }
    }
    if(call->ptheJumpTarget)
    {
        /* the first instruction of a script function carries its label */
        extern const char *ImportCache_NameOf(Instruction **target);
        Instruction *t = *call->ptheJumpTarget;

        name = ImportCache_NameOf(call->ptheJumpTarget);
        if(!name)
        {
            name = t ? t->Label : NULL;
        }
        if(!name)
        {
            snprintf(buf, sizeof(buf), "script@%p", key);
            name = buf;
        }
    }
    else if(call->functionRef && pi && pi->ptheFunctionList)
    {
        if(List_Includes(pi->ptheFunctionList, call->functionRef))
        {
            name = List_GetName(pi->ptheFunctionList);
        }
    }
    if(!name)
    {
        snprintf(buf, sizeof(buf), "native@%p", key);
        name = buf;
    }
    e = sab_prof_find(name);
    if(i < SAB_PROF_PTRS)
    {
        sab_prof_ptr_key[k] = key;
        sab_prof_ptr_val[k] = e;
    }
    return e;
}

void sab_prof_enter_call(void *interpreter, void *callp)
{
    Instruction *call = (Instruction *)callp;
    sab_prof_frame *f;

    if(sab_prof_depth >= SAB_PROF_STACK)
    {
        sab_prof_depth++;
        return;
    }
    f = &sab_prof_stack[sab_prof_depth++];
    f->entry = sab_prof_entry_of((Interpreter *)interpreter, call);
    f->start = sab_now();
    f->child = 0;
}

static void sab_prof_enter(const char *name)
{
    sab_prof_frame *f;

    if(sab_prof_depth >= SAB_PROF_STACK)
    {
        sab_prof_depth++;
        return;
    }
    f = &sab_prof_stack[sab_prof_depth++];
    f->entry = sab_prof_find(name ? name : "?");
    f->start = sab_now();
    f->child = 0;
}

/*
 * Script call stack at a runtime error (printed by the interpreter after a
 * native function failed): the profiler's frames, outermost first. Empty
 * unless sab_profile(1) is on.
 */
void sab_prof_print_stack(void)
{
    int i, n = sab_prof_depth < SAB_PROF_STACK ? sab_prof_depth : SAB_PROF_STACK;

    if(!sab_prof_on || n <= 0)
    {
        return;
    }
    printf(" script stack (sab_profile):");
    for(i = 0; i < n; i++)
    {
        printf(" %s%s", i ? "> " : "", sab_prof_stack[i].entry >= 0 ? sab_prof_tab[sab_prof_stack[i].entry].name : "?");
    }
    printf("\n");
}

void sab_prof_leave(void)
{
    sab_prof_frame *f;
    double dt;

    if(sab_prof_depth <= 0)
    {
        return;
    }
    sab_prof_depth--;
    if(sab_prof_depth >= SAB_PROF_STACK)
    {
        return;
    }
    f = &sab_prof_stack[sab_prof_depth];
    dt = sab_now() - f->start;
    if(f->entry >= 0)
    {
        sab_prof_tab[f->entry].calls++;
        sab_prof_tab[f->entry].total += dt;
        sab_prof_tab[f->entry].self += dt - f->child;
    }
    if(sab_prof_depth > 0 && sab_prof_depth - 1 < SAB_PROF_STACK)
    {
        sab_prof_stack[sab_prof_depth - 1].child += dt;
    }
}

//sab_profile(on)
HRESULT sab_profile(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG on = 1;

    sab_arg_long(varlist, 0, paramCount, &on);
    if(on)
    {
        int i;

        for(i = 0; i < sab_prof_count; i++)
        {
            free((void *)sab_prof_tab[i].name);
        }
        memset(sab_prof_hash, 0, sizeof(sab_prof_hash));
        memset(sab_prof_ptr_key, 0, sizeof(sab_prof_ptr_key));
        sab_prof_count = 0;
        sab_prof_depth = 0;
        sab_prof_t0 = sab_now();
    }
    sab_prof_on = on ? 1 : 0;
    sab_prof_depth = 0;
    *pretvar = NULL;
    return S_OK;
}

static int sab_prof_cmp(const void *a, const void *b)
{
    double d = ((const sab_prof_entry *)b)->self - ((const sab_prof_entry *)a)->self;

    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

//sab_profile_dump(n)
HRESULT sab_profile_dump(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG n = 40;
    int i;
    double wall = sab_now() - sab_prof_t0, all = 0;
    sab_prof_entry *copy;

    sab_arg_long(varlist, 0, paramCount, &n);
    copy = malloc(sizeof(*copy) * (sab_prof_count ? sab_prof_count : 1));
    memcpy(copy, sab_prof_tab, sizeof(*copy) * sab_prof_count);
    qsort(copy, sab_prof_count, sizeof(*copy), sab_prof_cmp);
    for(i = 0; i < sab_prof_count; i++)
    {
        all += copy[i].self;
    }
    printf("SAB profile: %.3f s wall, %.3f s in profiled calls, %d functions\n", wall, all, sab_prof_count);
    printf("SAB profile: %10s %10s %10s %10s  %s\n", "self ms", "total ms", "calls", "us/call", "function");
    for(i = 0; i < sab_prof_count && i < n; i++)
    {
        printf("SAB profile: %10.2f %10.2f %10lu %10.2f  %s\n", copy[i].self * 1000.0, copy[i].total * 1000.0,
               copy[i].calls, copy[i].calls ? copy[i].total * 1e6 / copy[i].calls : 0.0, copy[i].name);
    }
    free(copy);
    *pretvar = NULL;
    return S_OK;
}

/* ========================================================= navigation map */

/*
 * File format (the "walk map" of the Slaps And Beans pipeline):
 *
 *   grid <x0> <z0> <cell> <columns>
 *   col <index> <n> <z0> <z1> <y0> <y1> [...]    n intervals of column index
 *   patch <name> <box|disc> <cx> <cz> <a> <b> <cos> <sin> <ylo> <yhi> <on>
 *   end
 *
 * Column i covers x0 + i cell ... x0 + (i + 1) cell; an interval is walkable
 * from z0 to z1 (cell centres, extended by half a cell) with the floor
 * height going linearly from y0 to y1. A patch removes the floor points
 * inside it (box: |local x| <= a, |local z| <= b in the frame rotated by
 * (cos, sin) about Y; disc: radius a) whose height is in [ylo, yhi].
 */

typedef struct
{
    float z0, z1, y0, y1;
} sab_nav_iv;

typedef struct
{
    char name[128];
    int box, on;
    float cx, cz, a, b, c, s, ylo, yhi;
} sab_nav_patch_t;

typedef struct
{
    char path[256];
    float x0, z0, cell;
    int ncols;
    int *first;             /* first interval of column i, ncols + 1 entries */
    sab_nav_iv *iv;
    int niv;
    sab_nav_patch_t *patch;
    int npatch;
} sab_nav_map;

#define SAB_NAV_MAX 16
static sab_nav_map *sab_navs[SAB_NAV_MAX + 1];

static void sab_nav_free_map(sab_nav_map *m)
{
    if(!m)
    {
        return;
    }
    free(m->first);
    free(m->iv);
    free(m->patch);
    free(m);
}

/* Reads a whole pak file into memory (NUL terminated). */
static char *sab_read_file(const char *path, size_t *len)
{
    int h = openpackfile(path, packfile);
    int size;
    char *buf;

    if(h < 0)
    {
        return NULL;
    }
    size = seekpackfile(h, 0, SEEK_END);
    seekpackfile(h, 0, SEEK_SET);
    buf = malloc(size + 1);
    if(!buf)
    {
        closepackfile(h);
        return NULL;
    }
    if(readpackfile(h, buf, size) != size)
    {
        free(buf);
        closepackfile(h);
        return NULL;
    }
    closepackfile(h);
    buf[size] = 0;
    if(len)
    {
        *len = size;
    }
    return buf;
}

static sab_nav_map *sab_nav_parse(const char *path)
{
    size_t len;
    char *buf = sab_read_file(path, &len), *line, *next, *tok, *save;
    sab_nav_map *m;
    int pass, col, n, k, cap_iv = 0, cap_p = 0, last = -1;
    int *count = NULL;

    if(!buf)
    {
        printf("sab_nav_load: cannot open %s\n", path);
        return NULL;
    }
    m = calloc(1, sizeof(*m));
    strncpy(m->path, path, sizeof(m->path) - 1);
    /* two passes: sizes, then data */
    for(pass = 0; pass < 2; pass++)
    {
        char *copy = strdup(buf);

        m->niv = 0;
        m->npatch = 0;
        for(line = copy; line && *line; line = next)
        {
            next = strpbrk(line, "\r\n");
            if(next)
            {
                *next++ = 0;
                while(*next == '\r' || *next == '\n')
                {
                    next++;
                }
            }
            tok = strtok_r(line, " \t", &save);
            if(!tok || tok[0] == '#')
            {
                continue;
            }
            if(!strcmp(tok, "grid"))
            {
                m->x0 = atof(strtok_r(NULL, " \t", &save));
                m->z0 = atof(strtok_r(NULL, " \t", &save));
                m->cell = atof(strtok_r(NULL, " \t", &save));
                m->ncols = atoi(strtok_r(NULL, " \t", &save));
                if(pass == 0)
                {
                    count = calloc(m->ncols + 1, sizeof(int));
                }
            }
            else if(!strcmp(tok, "col") && count)
            {
                col = atoi(strtok_r(NULL, " \t", &save));
                n = atoi(strtok_r(NULL, " \t", &save));
                if(col < 0 || col >= m->ncols)
                {
                    continue;
                }
                if(pass == 0)
                {
                    count[col] = n;
                    m->niv += n;
                    continue;
                }
                for(k = 0; k < n; k++)
                {
                    sab_nav_iv *v = &m->iv[m->first[col] + k];

                    v->z0 = atof(strtok_r(NULL, " \t", &save));
                    v->z1 = atof(strtok_r(NULL, " \t", &save));
                    v->y0 = atof(strtok_r(NULL, " \t", &save));
                    v->y1 = atof(strtok_r(NULL, " \t", &save));
                }
            }
            else if(!strcmp(tok, "patch"))
            {
                if(pass == 0)
                {
                    m->npatch++;
                    continue;
                }
                {
                    sab_nav_patch_t *p = &m->patch[m->npatch++];

                    strncpy(p->name, strtok_r(NULL, " \t", &save), sizeof(p->name) - 1);
                    p->box = !strcmp(strtok_r(NULL, " \t", &save), "box");
                    p->cx = atof(strtok_r(NULL, " \t", &save));
                    p->cz = atof(strtok_r(NULL, " \t", &save));
                    p->a = atof(strtok_r(NULL, " \t", &save));
                    p->b = atof(strtok_r(NULL, " \t", &save));
                    p->c = atof(strtok_r(NULL, " \t", &save));
                    p->s = atof(strtok_r(NULL, " \t", &save));
                    p->ylo = atof(strtok_r(NULL, " \t", &save));
                    p->yhi = atof(strtok_r(NULL, " \t", &save));
                    p->on = atoi(strtok_r(NULL, " \t", &save));
                }
            }
        }
        free(copy);
        if(pass == 0)
        {
            if(!count)
            {
                break;
            }
            m->first = calloc(m->ncols + 1, sizeof(int));
            for(col = 0; col < m->ncols; col++)
            {
                m->first[col + 1] = m->first[col] + count[col];
            }
            m->iv = calloc(m->niv ? m->niv : 1, sizeof(sab_nav_iv));
            m->patch = calloc(m->npatch ? m->npatch : 1, sizeof(sab_nav_patch_t));
        }
    }
    (void)cap_iv;
    (void)cap_p;
    (void)last;
    free(count);
    free(buf);
    if(!m->first)
    {
        sab_nav_free_map(m);
        printf("sab_nav_load: no grid in %s\n", path);
        return NULL;
    }
    printf("sab_nav_load: %s: %d columns, %d intervals, %d patches\n", path, m->ncols, m->niv, m->npatch);
    return m;
}

static sab_nav_map *sab_nav_get(ScriptVariant **v, int n)
{
    LONG h;

    if(!sab_arg_long(v, 0, n, &h) || h < 1 || h > SAB_NAV_MAX)
    {
        return NULL;
    }
    return sab_navs[h];
}

static int sab_nav_patched(const sab_nav_map *m, float x, float y, float z)
{
    int i;

    for(i = 0; i < m->npatch; i++)
    {
        const sab_nav_patch_t *p = &m->patch[i];
        float dx, dz;

        if(!p->on || y < p->ylo || y > p->yhi)
        {
            continue;
        }
        dx = x - p->cx;
        dz = z - p->cz;
        if(p->box)
        {
            float u = dx * p->c + dz * p->s, w = -dx * p->s + dz * p->c;

            if(fabsf(u) <= p->a && fabsf(w) <= p->b)
            {
                return 1;
            }
        }
        else if(dx * dx + dz * dz <= p->a * p->a)
        {
            return 1;
        }
    }
    return 0;
}

static float sab_nav_iv_y(const sab_nav_iv *v, float z)
{
    if(v->z1 - v->z0 < 0.001f)
    {
        return v->y0;
    }
    if(z < v->z0)
    {
        z = v->z0;
    }
    if(z > v->z1)
    {
        z = v->z1;
    }
    return v->y0 + (v->y1 - v->y0) * (z - v->z0) / (v->z1 - v->z0);
}

static int sab_nav_col(const sab_nav_map *m, float x)
{
    return (int)floorf((x - m->x0) / m->cell);
}

/*
 * Nearest point of column c to (y, z): distance^2 = dz^2 + (|dy| - climb)+^2
 * (a slope is followed in plan, another floor is far). Patched points are
 * skipped by stepping out of the patch along z.
 */
static int sab_nav_col_nearest(const sab_nav_map *m, int c, float y, float z, float climb,
                               float *bd, float *bz, float *by)
{
    int k, found = 0;
    float x, h = m->cell * 0.5f;

    if(c < 0 || c >= m->ncols)
    {
        return 0;
    }
    x = m->x0 + (c + 0.5f) * m->cell;
    for(k = m->first[c]; k < m->first[c + 1]; k++)
    {
        const sab_nav_iv *v = &m->iv[k];
        float z0 = v->z0 - h, z1 = v->z1 + h, pz = z, py, e, d;

        if(pz < z0)
        {
            pz = z0;
        }
        if(pz > z1)
        {
            pz = z1;
        }
        if(m->npatch && sab_nav_patched(m, x, sab_nav_iv_y(v, pz), pz))
        {
            float a = pz, b = pz;
            int ok = 0;

            while(a >= z0 || b <= z1)
            {
                a -= m->cell;
                b += m->cell;
                if(a >= z0 && !sab_nav_patched(m, x, sab_nav_iv_y(v, a), a))
                {
                    pz = a;
                    ok = 1;
                    break;
                }
                if(b <= z1 && !sab_nav_patched(m, x, sab_nav_iv_y(v, b), b))
                {
                    pz = b;
                    ok = 1;
                    break;
                }
            }
            if(!ok)
            {
                continue;
            }
        }
        py = sab_nav_iv_y(v, pz);
        e = fabsf(py - y) - climb;
        if(e < 0)
        {
            e = 0;
        }
        d = (pz - z) * (pz - z) + e * e;
        if(!found || d < *bd)
        {
            *bd = d;
            *bz = pz;
            *by = py;
            found = 1;
        }
    }
    return found;
}

static int sab_nav_sample_pt(const sab_nav_map *m, float x, float y, float z, float maxd, float climb,
                             float *ox, float *oy, float *oz)
{
    int c = sab_nav_col(m, x), k, side, kmax = (int)floorf(maxd / m->cell) + 1, found = 0;
    float best = maxd * maxd, d, pz, py, cx, dx;

    for(k = 0; k <= kmax; k++)
    {
        dx = (k - 1) * m->cell;
        if(k > 0 && found && dx * dx >= best)
        {
            break;
        }
        for(side = -1; side <= 1; side += 2)
        {
            if(k == 0 && side > 0)
            {
                continue;
            }
            if(!sab_nav_col_nearest(m, c + k * side, y, z, climb, &d, &pz, &py))
            {
                continue;
            }
            cx = (k == 0) ? x : m->x0 + (c + k * side + 0.5f) * m->cell - side * m->cell * 0.5f;
            d += (cx - x) * (cx - x);
            if(d <= best)
            {
                best = d;
                *ox = cx;
                *oy = py;
                *oz = pz;
                found = 1;
            }
        }
    }
    return found;
}

static int sab_nav_floor_at(const sab_nav_map *m, float x, float y, float z, float *out)
{
    int c = sab_nav_col(m, x), k, found = 0;
    float h = m->cell * 0.5f, best = -1e9f;

    if(c < 0 || c >= m->ncols)
    {
        return 0;
    }
    for(k = m->first[c]; k < m->first[c + 1]; k++)
    {
        const sab_nav_iv *v = &m->iv[k];
        float fy;

        if(z < v->z0 - h || z > v->z1 + h)
        {
            continue;
        }
        fy = sab_nav_iv_y(v, z);
        if(fy <= y + 1.0f && fy > best)
        {
            best = fy;
            found = 1;
        }
    }
    if(found)
    {
        *out = best;
    }
    return found;
}

static int sab_nav_on(const sab_nav_map *m, float x, float y, float z)
{
    int c = sab_nav_col(m, x), k;
    float h = m->cell * 0.5f;

    if(c < 0 || c >= m->ncols)
    {
        return 0;
    }
    for(k = m->first[c]; k < m->first[c + 1]; k++)
    {
        const sab_nav_iv *v = &m->iv[k];
        float fy;

        if(z < v->z0 - h || z > v->z1 + h)
        {
            continue;
        }
        fy = sab_nav_iv_y(v, z);
        if(fabsf(fy - y) <= 1.0f && !(m->npatch && sab_nav_patched(m, x, fy, z)))
        {
            return 1;
        }
    }
    return 0;
}

//sab_nav_load(path)
HRESULT sab_nav_load(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    int i, slot = 0;
    sab_nav_map *m;

    if(!path)
    {
        printf("Function requires a path: sab_nav_load(path)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    for(i = 1; i <= SAB_NAV_MAX; i++)
    {
        if(sab_navs[i] && !strcmp(sab_navs[i]->path, path))
        {
            sab_ret_long(pretvar, i);
            return S_OK;
        }
        if(!sab_navs[i] && !slot)
        {
            slot = i;
        }
    }
    if(!slot)
    {
        /* reuse the oldest slot */
        slot = 1;
        sab_nav_free_map(sab_navs[1]);
        sab_navs[1] = NULL;
    }
    m = sab_nav_parse(path);
    sab_navs[slot] = m;
    sab_ret_long(pretvar, m ? slot : 0);
    return S_OK;
}

//sab_nav_free(h)
HRESULT sab_nav_free(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG h;

    *pretvar = NULL;
    if(sab_arg_long(varlist, 0, paramCount, &h) && h >= 1 && h <= SAB_NAV_MAX)
    {
        sab_nav_free_map(sab_navs[h]);
        sab_navs[h] = NULL;
    }
    return S_OK;
}

//sab_nav_sample(h, x, y, z, maxd, climb, out)
HRESULT sab_nav_sample(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    sab_nav_map *m = sab_nav_get(varlist, paramCount);
    DOUBLE x, y, z, d, climb;
    Varlist *out = sab_arg_array(varlist, 6, paramCount);
    float ox = 0, oy = 0, oz = 0;

    if(!m || !out || !sab_arg_double(varlist, 1, paramCount, &x) || !sab_arg_double(varlist, 2, paramCount, &y)
       || !sab_arg_double(varlist, 3, paramCount, &z) || !sab_arg_double(varlist, 4, paramCount, &d)
       || !sab_arg_double(varlist, 5, paramCount, &climb))
    {
        printf("Function requires: sab_nav_sample(handle, x, y, z, maxdist, climb, out_array)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    if(!sab_nav_sample_pt(m, x, y, z, d, climb, &ox, &oy, &oz))
    {
        sab_ret_long(pretvar, 0);
        return S_OK;
    }
    sab_array_set_double(out, 0, ox);
    sab_array_set_double(out, 1, oy);
    sab_array_set_double(out, 2, oz);
    sab_ret_long(pretvar, 1);
    return S_OK;
}

//sab_nav_floor(h, x, y, z)
HRESULT sab_nav_floor(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    sab_nav_map *m = sab_nav_get(varlist, paramCount);
    DOUBLE x, y, z;
    float f;

    if(!m || !sab_arg_double(varlist, 1, paramCount, &x) || !sab_arg_double(varlist, 2, paramCount, &y)
       || !sab_arg_double(varlist, 3, paramCount, &z))
    {
        printf("Function requires: sab_nav_floor(handle, x, y, z)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    sab_ret_double(pretvar, sab_nav_floor_at(m, x, y, z, &f) ? f : y);
    return S_OK;
}

//sab_nav_raycast(h, ax, az, bx, bz, y, step, out)
HRESULT sab_nav_raycast(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    sab_nav_map *m = sab_nav_get(varlist, paramCount);
    DOUBLE ax, az, bx, bz, y, step;
    Varlist *out = sab_arg_array(varlist, 7, paramCount);
    double len, t, x, z, lx, lz;
    float f;
    int i, n, blocked = 0;

    if(!m || !out || !sab_arg_double(varlist, 1, paramCount, &ax) || !sab_arg_double(varlist, 2, paramCount, &az)
       || !sab_arg_double(varlist, 3, paramCount, &bx) || !sab_arg_double(varlist, 4, paramCount, &bz)
       || !sab_arg_double(varlist, 5, paramCount, &y) || !sab_arg_double(varlist, 6, paramCount, &step) || step <= 0)
    {
        printf("Function requires: sab_nav_raycast(handle, ax, az, bx, bz, y, step, out_array)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    lx = ax;
    lz = az;
    n = (int)floor(len / step) + 1;
    for(i = 1; i <= n && len > 0; i++)
    {
        t = i * step / len;
        if(t > 1)
        {
            t = 1;
        }
        x = ax + (bx - ax) * t;
        z = az + (bz - az) * t;
        if(!sab_nav_floor_at(m, x, y, z, &f))
        {
            f = y;
        }
        if(!sab_nav_on(m, x, f, z))
        {
            blocked = 1;
            break;
        }
        lx = x;
        lz = z;
        y = f;
    }
    sab_array_set_double(out, 0, blocked ? lx : bx);
    sab_array_set_double(out, 1, blocked ? lz : bz);
    sab_ret_long(pretvar, blocked);
    return S_OK;
}

//sab_nav_patch(h, name, on)
HRESULT sab_nav_patch(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    sab_nav_map *m = sab_nav_get(varlist, paramCount);
    const char *name = sab_arg_str(varlist, 1, paramCount);
    LONG on;
    int i, found = 0;

    if(!m || !name || !sab_arg_long(varlist, 2, paramCount, &on))
    {
        printf("Function requires: sab_nav_patch(handle, name, on)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    for(i = 0; i < m->npatch; i++)
    {
        if(!strcmp(m->patch[i].name, name))
        {
            m->patch[i].on = on ? 1 : 0;
            found = 1;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

/* ================================================================= camera */

static int sab_cam_on = 0;
static float sab_cam_x = 0, sab_cam_y = 0;

//sab_camera(on, x, y)
HRESULT sab_camera(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG on = 0;
    DOUBLE x = 0, y = 0;

    sab_arg_long(varlist, 0, paramCount, &on);
    sab_arg_double(varlist, 1, paramCount, &x);
    sab_arg_double(varlist, 2, paramCount, &y);
    sab_cam_on = on ? 1 : 0;
    sab_cam_x = x;
    sab_cam_y = y;
    *pretvar = NULL;
    return S_OK;
}

/* Called by the engine right after update_scroller(). */
void sab_camera_apply(void)
{
    if(!sab_cam_on || !level)
    {
        return;
    }
    advancex = sab_cam_x;
    advancey = sab_cam_y;
    if(advancex < 0)
    {
        advancex = 0;
    }
    if(advancey < 0)
    {
        advancey = 0;
    }
    level->pos = (int)advancex;
}


/* ============================================================ post effects */

static int sab_pfx_on = 0, sab_pfx_curves_on = 0, sab_pfx_n = 0, sab_pfx_have_curves = 0;
static int sab_pfx_zsplit = 0, sab_pfx_zsplit_set = 0;
static unsigned char *sab_pfx_lut = NULL;
static unsigned char sab_pfx_curves[3][256];
static double sab_pfx_intensity = 0.0, sab_pfx_threshold = 0.5;
static unsigned char *sab_pfx_a = NULL;        /* graded frame before the bloom, RGB */

/* Direct-mapped cache of graded colours (pixel art: few colours per frame). */
#define SAB_PFX_CACHE 65536
static unsigned int sab_pfx_ckey[SAB_PFX_CACHE];
static unsigned char sab_pfx_cval[SAB_PFX_CACHE][3];

int sab_postfx_active(void)
{
    return sab_pfx_on && sab_pfx_lut;
}

int sab_postfx_split(void)
{
    return sab_pfx_zsplit_set ? sab_pfx_zsplit : HUD_Z;
}

HRESULT sab_postfx_load(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *lp = sab_arg_str(varlist, 0, paramCount), *cp = sab_arg_str(varlist, 1, paramCount);
    size_t len = 0;
    char *buf;
    int n;

    sab_ret_long(pretvar, 0);
    if(!lp)
    {
        return S_OK;
    }
    buf = sab_read_file(lp, &len);
    if(!buf)
    {
        printf("sab_postfx_load: cannot open %s\n", lp);
        return S_OK;
    }
    for(n = 2; n <= 256 && (size_t)n * n * n * 3 < len; n++)
    {
    }
    if((size_t)n * n * n * 3 != len)
    {
        printf("sab_postfx_load: %s is not an N^3 RGB table (%d bytes)\n", lp, (int)len);
        free(buf);
        return S_OK;
    }
    free(sab_pfx_lut);
    sab_pfx_lut = (unsigned char *)buf;
    memset(sab_pfx_ckey, 0, sizeof(sab_pfx_ckey));
    sab_pfx_n = n;
    sab_pfx_have_curves = 0;
    if(cp && cp[0])
    {
        buf = sab_read_file(cp, &len);
        if(buf && len == 768)
        {
            memcpy(sab_pfx_curves, buf, 768);
            sab_pfx_have_curves = 1;
        }
        else
        {
            printf("sab_postfx_load: bad curves file %s\n", cp);
        }
        free(buf);
    }
    sab_ret_long(pretvar, 1);
    return S_OK;
}

HRESULT sab_postfx(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG on = 0, curves = 1;

    sab_arg_long(varlist, 0, paramCount, &on);
    sab_arg_long(varlist, 1, paramCount, &curves);
    sab_pfx_on = on ? 1 : 0;
    sab_pfx_curves_on = curves ? 1 : 0;
    *pretvar = NULL;
    return S_OK;
}

HRESULT sab_postfx_bloom(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    DOUBLE i = 0, t = 0.5;

    sab_arg_double(varlist, 0, paramCount, &i);
    sab_arg_double(varlist, 1, paramCount, &t);
    sab_pfx_intensity = i;
    sab_pfx_threshold = t;
    *pretvar = NULL;
    return S_OK;
}

HRESULT sab_postfx_zsplit(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG z = 0;

    if(sab_arg_long(varlist, 0, paramCount, &z))
    {
        sab_pfx_zsplit = (int)z;
        sab_pfx_zsplit_set = 1;
    }
    *pretvar = NULL;
    return S_OK;
}


static void sab_pfx_lookup_slow(const unsigned char *src, unsigned char *dst);

static void sab_pfx_lookup(const unsigned char *src, unsigned char *dst)
{
    unsigned int key = src[0] | (src[1] << 8) | (src[2] << 16) | 0x1000000u;
    unsigned int h = (key * 2654435761u) >> 16;

    if(sab_pfx_ckey[h] == key)
    {
        dst[0] = sab_pfx_cval[h][0];
        dst[1] = sab_pfx_cval[h][1];
        dst[2] = sab_pfx_cval[h][2];
        return;
    }
    sab_pfx_lookup_slow(src, dst);
    sab_pfx_ckey[h] = key;
    sab_pfx_cval[h][0] = dst[0];
    sab_pfx_cval[h][1] = dst[1];
    sab_pfx_cval[h][2] = dst[2];
}

/* Trilinear lookup of one 8-bit colour in the N^3 table. */
static void sab_pfx_lookup_slow(const unsigned char *src, unsigned char *dst)
{
    int n = sab_pfx_n, ch, i;
    int fr = src[0] * (n - 1), fg = src[1] * (n - 1), fb = src[2] * (n - 1);
    int r0 = fr / 255, g0 = fg / 255, b0 = fb / 255;
    int r1 = r0 < n - 1 ? r0 + 1 : r0, g1 = g0 < n - 1 ? g0 + 1 : g0, b1 = b0 < n - 1 ? b0 + 1 : b0;
    int wr = (fr - r0 * 255) * 256 / 255, wg = (fg - g0 * 255) * 256 / 255, wb = (fb - b0 * 255) * 256 / 255;
    const unsigned char *L = sab_pfx_lut;
    int o[8];

    o[0] = ((b0 * n + g0) * n + r0) * 3;
    o[1] = ((b0 * n + g0) * n + r1) * 3;
    o[2] = ((b0 * n + g1) * n + r0) * 3;
    o[3] = ((b0 * n + g1) * n + r1) * 3;
    o[4] = ((b1 * n + g0) * n + r0) * 3;
    o[5] = ((b1 * n + g0) * n + r1) * 3;
    o[6] = ((b1 * n + g1) * n + r0) * 3;
    o[7] = ((b1 * n + g1) * n + r1) * 3;
    for(ch = 0; ch < 3; ch++)
    {
        int c00 = L[o[0] + ch] * (256 - wr) + L[o[1] + ch] * wr;
        int c01 = L[o[2] + ch] * (256 - wr) + L[o[3] + ch] * wr;
        int c10 = L[o[4] + ch] * (256 - wr) + L[o[5] + ch] * wr;
        int c11 = L[o[6] + ch] * (256 - wr) + L[o[7] + ch] * wr;
        int c0 = (c00 * (256 - wg) + c01 * wg) >> 8;
        int c1 = (c10 * (256 - wg) + c11 * wg) >> 8;
        i = (c0 * (256 - wb) + c1 * wb + (1 << 15)) >> 16;
        dst[ch] = (unsigned char)(i > 255 ? 255 : i);
    }
}

/* [1, 2, 1] / 4 separable tent with clamped edges (an 8-bit pass each way). */
static void sab_pfx_tent(unsigned char *q, int w, int h, unsigned char *tmp)
{
    int x, y, c;

    for(y = 0; y < h; y++)
    {
        for(x = 0; x < w; x++)
        {
            int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : x;
            for(c = 0; c < 3; c++)
            {
                tmp[(y * w + x) * 3 + c] = (unsigned char)((q[(y * w + xl) * 3 + c] + 2 * q[(y * w + x) * 3 + c]
                                                            + q[(y * w + xr) * 3 + c] + 2) >> 2);
            }
        }
    }
    for(y = 0; y < h; y++)
    {
        int yu = y > 0 ? y - 1 : 0, yd = y < h - 1 ? y + 1 : y;
        for(x = 0; x < w; x++)
        {
            for(c = 0; c < 3; c++)
            {
                q[(y * w + x) * 3 + c] = (unsigned char)((tmp[(yu * w + x) * 3 + c] + 2 * tmp[(y * w + x) * 3 + c]
                                                          + tmp[(yd * w + x) * 3 + c] + 2) >> 2);
            }
        }
    }
}

/* BlurAndFlares pass 4: 9 bilinear taps at 0, +-1, +-2, +-3, +-5 x unit,
   weights 0.225, 0.15, 0.11, 0.075, 0.0525, folded into an integer kernel. */
static int sab_pfx_kernel(double unit, double *k, int maxr)
{
    static const double off[5] = {0, 1, 2, 3, 5}, wt[5] = {0.225, 0.15, 0.11, 0.075, 0.0525};
    int i, s, r = 0;

    for(i = 0; i <= 2 * maxr; i++)
    {
        k[i] = 0;
    }
    for(i = 0; i < 5; i++)
    {
        for(s = -1; s <= 1; s += 2)
        {
            double d = off[i] * unit * s, f;
            int d0;

            if(i == 0 && s > 0)
            {
                continue;
            }
            d0 = (int)floor(d);
            f = d - d0;
            if(d0 + 1 > maxr || d0 < -maxr)
            {
                continue;
            }
            k[d0 + maxr] += wt[i] * (1 - f);
            k[d0 + 1 + maxr] += wt[i] * f;
            if(abs(d0) + 1 > r)
            {
                r = abs(d0) + 1;
            }
        }
    }
    return r;
}

static void sab_pfx_blur(unsigned char *src, unsigned char *dst, int w, int h, double unit, unsigned char *tmp)
{
    double k[25];
    int R = 12, x, y, c, i, nt = 0, off[25], wt[25];

    sab_pfx_kernel(unit, k, R);
    for(i = 0; i <= 2 * R; i++)
    {
        if(k[i] > 0)
        {
            off[nt] = i - R;
            wt[nt] = (int)(k[i] * 65536 + 0.5);
            nt++;
        }
    }
    for(y = 0; y < h; y++)                        /* vertical */
    {
        for(x = 0; x < w; x++)
        {
            for(c = 0; c < 3; c++)
            {
                int v = 0;
                for(i = 0; i < nt; i++)
                {
                    int yy = y + off[i];
                    yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                    v += wt[i] * src[(yy * w + x) * 3 + c];
                }
                v = (v + 32768) >> 16;
                tmp[(y * w + x) * 3 + c] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
    for(y = 0; y < h; y++)                        /* horizontal */
    {
        for(x = 0; x < w; x++)
        {
            for(c = 0; c < 3; c++)
            {
                int v = 0;
                for(i = 0; i < nt; i++)
                {
                    int xx = x + off[i];
                    xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                    v += wt[i] * tmp[(y * w + xx) * 3 + c];
                }
                v = (v + 32768) >> 16;
                dst[(y * w + x) * 3 + c] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
}

/* Bilinear 2x upsample (texel centres), RGB: output texel 2i samples
   0.75 src i + 0.25 src i-1, 2i+1 samples 0.75 src i + 0.25 src i+1. */
static void sab_pfx_up2(const unsigned char *src, int w, int h, unsigned char *dst)
{
    int W = w * 2, H = h * 2, x, y, c;

    for(y = 0; y < H; y++)
    {
        int y0 = y >> 1, y1 = (y & 1) ? y0 + 1 : y0 - 1;
        y1 = y1 < 0 ? 0 : (y1 >= h ? h - 1 : y1);
        for(x = 0; x < W; x++)
        {
            int x0 = x >> 1, x1 = (x & 1) ? x0 + 1 : x0 - 1;
            const unsigned char *a, *b, *cc, *d;
            unsigned char *o = dst + (y * W + x) * 3;
            x1 = x1 < 0 ? 0 : (x1 >= w ? w - 1 : x1);
            a = src + (y0 * w + x0) * 3;
            b = src + (y0 * w + x1) * 3;
            cc = src + (y1 * w + x0) * 3;
            d = src + (y1 * w + x1) * 3;
            for(c = 0; c < 3; c++)
            {
                o[c] = (unsigned char)((9 * a[c] + 3 * b[c] + 3 * cc[c] + d[c] + 8) >> 4);
            }
        }
    }
}

/*
 * The camera chain on a 32-bit screen (docs/postfx.md 8): pre LUT, bloom
 * (4x4 max to a quarter, tent, threshold, two cumulative 9-tap blurs combined
 * with max, tent, 2x bilinear up twice, + intensity), the curves table.
 */
void sab_postfx_apply(s_screen *scr)
{
    int W, H, x, y, c, qw, qh;
    unsigned char *px, *q = NULL, *t = NULL, *b0 = NULL, *b1 = NULL, *tmp = NULL, *u1 = NULL, *u2 = NULL;

    if(!sab_postfx_active() || !scr || scr->pixelformat != PIXEL_32)
    {
        return;
    }
    W = scr->width;
    H = scr->height;
    sab_pfx_a = realloc(sab_pfx_a, (size_t)W * H * 3);
    px = scr->data;
    for(y = 0; y < H; y++)
    {
        for(x = 0; x < W; x++)
        {
            sab_pfx_lookup(px + ((size_t)y * W + x) * 4, sab_pfx_a + ((size_t)y * W + x) * 3);
        }
    }
    if(sab_pfx_intensity > 0 && W % 4 == 0 && H % 4 == 0)
    {
        int th = (int)(sab_pfx_threshold * 255 + 0.5);
        qw = W / 4;
        qh = H / 4;
        q = malloc((size_t)qw * qh * 3);
        tmp = malloc((size_t)qw * qh * 3);
        b0 = malloc((size_t)qw * qh * 3);
        b1 = malloc((size_t)qw * qh * 3);
        u1 = malloc((size_t)qw * qh * 3 * 4);
        u2 = malloc((size_t)W * H * 3);
        for(y = 0; y < qh; y++)
        {
            for(x = 0; x < qw; x++)
            {
                for(c = 0; c < 3; c++)
                {
                    int m = 0, i, j;
                    for(j = 0; j < 4; j++)
                    {
                        for(i = 0; i < 4; i++)
                        {
                            int v = sab_pfx_a[((size_t)(y * 4 + j) * W + x * 4 + i) * 3 + c];
                            m = v > m ? v : m;
                        }
                    }
                    q[(y * qw + x) * 3 + c] = (unsigned char)m;
                }
            }
        }
        sab_pfx_tent(q, qw, qh, tmp);
        for(x = 0; x < qw * qh * 3; x++)
        {
            q[x] = (unsigned char)(q[x] > th ? q[x] - th : 0);
        }
        sab_pfx_blur(q, b0, qw, qh, 2.5 * qh / 512.0, tmp);
        sab_pfx_blur(b0, b1, qw, qh, 3.125 * qh / 512.0, tmp);
        for(x = 0; x < qw * qh * 3; x++)
        {
            q[x] = b0[x] > b1[x] ? b0[x] : b1[x];
        }
        sab_pfx_tent(q, qw, qh, tmp);
        sab_pfx_up2(q, qw, qh, u1);
        sab_pfx_up2(u1, qw * 2, qh * 2, u2);
        {
            int ki = (int)(sab_pfx_intensity * 256 + 0.5);
            for(x = 0; x < W * H * 3; x++)
            {
                int v = sab_pfx_a[x] + ((ki * u2[x] + 128) >> 8);
                sab_pfx_a[x] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
        free(q);
        free(tmp);
        free(b0);
        free(b1);
        free(u1);
        free(u2);
    }
    for(y = 0; y < H; y++)
    {
        for(x = 0; x < W; x++)
        {
            unsigned char *d = px + ((size_t)y * W + x) * 4, *a = sab_pfx_a + ((size_t)y * W + x) * 3;
            if(sab_pfx_have_curves && sab_pfx_curves_on)
            {
                d[0] = sab_pfx_curves[0][a[0]];
                d[1] = sab_pfx_curves[1][a[1]];
                d[2] = sab_pfx_curves[2][a[2]];
            }
            else
            {
                d[0] = a[0];
                d[1] = a[1];
                d[2] = a[2];
            }
        }
    }
}

/* ============================================================ registration */

void sab_register_functions(List *list)
{
    List_InsertAfter(list, (void *)sab_profile, "sab_profile");
    List_InsertAfter(list, (void *)sab_profile_dump, "sab_profile_dump");
    List_InsertAfter(list, (void *)sab_nav_load, "sab_nav_load");
    List_InsertAfter(list, (void *)sab_nav_free, "sab_nav_free");
    List_InsertAfter(list, (void *)sab_nav_sample, "sab_nav_sample");
    List_InsertAfter(list, (void *)sab_nav_floor, "sab_nav_floor");
    List_InsertAfter(list, (void *)sab_nav_raycast, "sab_nav_raycast");
    List_InsertAfter(list, (void *)sab_nav_patch, "sab_nav_patch");
    List_InsertAfter(list, (void *)sab_camera, "sab_camera");
    List_InsertAfter(list, (void *)sab_postfx_load, "sab_postfx_load");
    List_InsertAfter(list, (void *)sab_postfx, "sab_postfx");
    List_InsertAfter(list, (void *)sab_postfx_bloom, "sab_postfx_bloom");
    List_InsertAfter(list, (void *)sab_postfx_zsplit, "sab_postfx_zsplit");
}
