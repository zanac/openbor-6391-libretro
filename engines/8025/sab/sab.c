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
 *     sab_postfx_bloom(intensity, threshold[, spread])
 *                                     Unity Standard Assets Bloom (High,
 *                                     2 blur iterations, spread 2.5 by
 *                                     default, Add); intensity 0 = off
 *     sab_postfx_zsplit(z)            sprite-queue z where the grading stops
 *                                     (default HUD_Z)
 *     sab_postfx_bullet(vig, darkness, sharpness, contrast, red, density, box, aspect)
 *                                     the duel's bullet time after the bloom
 *                                     (docs/postfx.md 5): CC_ContrastVignette
 *                                     (vig 1, centre (0.71, 0.5), green/blue
 *                                     coeff 1, edge 0), Colorful.Letterbox
 *                                     (box 1, black, aspect), Colorful
 *                                     PhotoFilter colour (1, 0.5, 0.2) at
 *                                     density (> 0.01), and the LoFiPalette
 *                                     with it; all 0 = off
 *     sab_postfx_lofi(lut, amount)    the LoFiPalette table: a 16^3 RGB file
 *                                     (node order as above), point sampled
 */

#include <time.h>
#include <math.h>
#include "sab/sab_prof.h"
#include "png.h"

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


/* the segment a -> b on the walk map at about height y (sab_nav_raycast's test) */
static int sab_nav_clear(const sab_nav_map *m, double ax, double az, double bx, double bz, float y)
{
    double len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az)), step = m->cell * 0.5, t;
    int i, n = (int)floor(len / step) + 1;
    float f;

    for(i = 1; i <= n && len > 0; i++)
    {
        double x, z;

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
            return 0;
        }
        y = f;
    }
    return 1;
}

#define SAB_NP_MARGIN 20.0

static int sab_nav_path_search(const sab_nav_map *m, double ax, double az, double bx, double bz, float y,
                               float *ox, float *oz);

//sab_nav_path(h, ax, az, bx, bz, y, out): Utils.GetFirstPathNode - the first
//corner of a shortest walkable path from a to b (A* on the map's cells, 8
//neighbours, within SAB_NP_MARGIN units around both, then the farthest path
//cell seen straight from a; a point off the map first goes back to its
//nearest point); out[0..1]; returns 1 when found (b itself when the way is
//clear), 0 when no path (out = b). The answer is kept while a and b stay in
//their cells.
HRESULT sab_nav_path(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    sab_nav_map *m = sab_nav_get(varlist, paramCount);
    DOUBLE ax, az, bx, bz, y;
    Varlist *out = sab_arg_array(varlist, 6, paramCount);
    float cell;
    int res;

    if(!m || !out || !sab_arg_double(varlist, 1, paramCount, &ax) || !sab_arg_double(varlist, 2, paramCount, &az)
       || !sab_arg_double(varlist, 3, paramCount, &bx) || !sab_arg_double(varlist, 4, paramCount, &bz)
       || !sab_arg_double(varlist, 5, paramCount, &y))
    {
        printf("Function requires: sab_nav_path(handle, ax, az, bx, bz, y, out_array)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    sab_array_set_double(out, 0, bx);
    sab_array_set_double(out, 1, bz);
    if(sab_nav_clear(m, ax, az, bx, bz, (float)y))
    {
        sab_ret_long(pretvar, 1);
        return S_OK;
    }
    {
        /* a off the map (pushed there): first back to its nearest point */
        float f, ox, oy, oz;

        if(!sab_nav_floor_at(m, (float)ax, (float)y, (float)az, &f))
        {
            f = (float)y;
        }
        if(!sab_nav_on(m, (float)ax, f, (float)az))
        {
            if(sab_nav_sample_pt(m, (float)ax, (float)y, (float)az, 3.0f, 0.5f, &ox, &oy, &oz))
            {
                sab_array_set_double(out, 0, ox);
                sab_array_set_double(out, 1, oz);
                sab_ret_long(pretvar, 1);
                return S_OK;
            }
        }
    }
    cell = m->cell;
    {
        /* the last answer while a and b stay in their cells */
        static const sab_nav_map *lm = NULL;
        static int la = -1, lb = -1, lr = 0;
        static float lx = 0, lz = 0;
        int ca = (int)floor(ax / cell) * 65536 + (int)floor(az / cell), cb = (int)floor(bx / cell) * 65536 + (int)floor(bz / cell);

        if(lm == m && la == ca && lb == cb)
        {
            sab_array_set_double(out, 0, lx);
            sab_array_set_double(out, 1, lz);
            sab_ret_long(pretvar, lr);
            return S_OK;
        }
        lm = m;
        la = ca;
        lb = cb;
        res = sab_nav_path_search(m, ax, az, bx, bz, (float)y, &lx, &lz);
        lr = res;
        sab_array_set_double(out, 0, lx);
        sab_array_set_double(out, 1, lz);
        sab_ret_long(pretvar, res);
        return S_OK;
    }
}

/* A* of sab_nav_path: 1 and the first corner in (*ox, *oz), or 0 (b) */
static int sab_nav_path_search(const sab_nav_map *m, double ax, double az, double bx, double bz, float y,
                               float *ox, float *oz)
{
    float cell = m->cell, x0, z0, *g = NULL, *fl = NULL, *hf = NULL;
    int nx, nz, n, sa, sb, found = 0, *par = NULL, *heap = NULL, hn = 0, hcap, res = 0;
    unsigned char *st = NULL;

    *ox = (float)bx;
    *oz = (float)bz;
    x0 = (float)((ax < bx ? ax : bx) - SAB_NP_MARGIN);
    z0 = (float)((az < bz ? az : bz) - SAB_NP_MARGIN);
    nx = (int)(((ax > bx ? ax : bx) + SAB_NP_MARGIN - x0) / cell) + 1;
    nz = (int)(((az > bz ? az : bz) + SAB_NP_MARGIN - z0) / cell) + 1;
    if(nx < 2 || nz < 2 || (long)nx * nz > 2000000)
    {
        return 0;
    }
    n = nx * nz;
    g = malloc(sizeof(float) * n);
    fl = malloc(sizeof(float) * n);
    par = malloc(sizeof(int) * n);
    hcap = n * 4;
    heap = malloc(sizeof(int) * hcap);
    hf = malloc(sizeof(float) * n);
    st = calloc(n, 1);              /* 0 new, 1 open, 2 closed, 3 blocked */
    if(!g || !fl || !par || !heap || !hf || !st)
    {
        goto done;
    }
#define SAB_NP_X(k) (x0 + ((k) % nx + 0.5f) * cell)
#define SAB_NP_Z(k) (z0 + ((k) / nx + 0.5f) * cell)
#define SAB_NP_H(k) (sqrtf((SAB_NP_X(k) - (float)bx) * (SAB_NP_X(k) - (float)bx) + (SAB_NP_Z(k) - (float)bz) * (SAB_NP_Z(k) - (float)bz)))
    sa = (int)((az - z0) / cell) * nx + (int)((ax - x0) / cell);
    sb = (int)((bz - z0) / cell) * nx + (int)((bx - x0) / cell);
    g[sa] = 0;
    fl[sa] = y;
    par[sa] = -1;
    st[sa] = 1;
    hf[sa] = SAB_NP_H(sa);
    heap[hn++] = sa;
    while(hn > 0)
    {
        int cur, k, dx, dz, cx, cz;

        /* pop the open cell of least g + h (binary heap; stale entries skipped) */
        cur = heap[0];
        heap[0] = heap[--hn];
        for(k = 0;;)
        {
            int l = 2 * k + 1, r = l + 1, mn = k, t;

            if(l < hn && hf[heap[l]] < hf[heap[mn]])
            {
                mn = l;
            }
            if(r < hn && hf[heap[r]] < hf[heap[mn]])
            {
                mn = r;
            }
            if(mn == k)
            {
                break;
            }
            t = heap[k];
            heap[k] = heap[mn];
            heap[mn] = t;
            k = mn;
        }
        if(st[cur] == 2)
        {
            continue;
        }
        st[cur] = 2;
        if(cur == sb)
        {
            found = 1;
            break;
        }
        cx = cur % nx;
        cz = cur / nx;
        for(dz = -1; dz <= 1; dz++)
        {
            for(dx = -1; dx <= 1; dx++)
            {
                int nb, ex = cx + dx, ez = cz + dz;
                float px, pz, f, ng;

                if((!dx && !dz) || ex < 0 || ez < 0 || ex >= nx || ez >= nz)
                {
                    continue;
                }
                nb = ez * nx + ex;
                if(st[nb] >= 2)
                {
                    continue;
                }
                px = SAB_NP_X(nb);
                pz = SAB_NP_Z(nb);
                if(!sab_nav_floor_at(m, px, fl[cur], pz, &f))
                {
                    f = fl[cur];
                }
                if(nb != sb && !sab_nav_on(m, px, f, pz))
                {
                    st[nb] = 3;
                    continue;
                }
                ng = g[cur] + cell * ((dx && dz) ? 1.41421356f : 1.0f);
                if(st[nb] == 1 && ng >= g[nb])
                {
                    continue;
                }
                g[nb] = ng;
                fl[nb] = f;
                par[nb] = cur;
                st[nb] = 1;
                hf[nb] = ng + SAB_NP_H(nb);
                if(hn < hcap)
                {
                    /* push (a cell may be in the heap twice: the old entry is
                       popped later and skipped as closed) */
                    int c2 = hn++;

                    heap[c2] = nb;
                    while(c2 > 0 && hf[heap[(c2 - 1) / 2]] > hf[heap[c2]])
                    {
                        int t = heap[c2];

                        heap[c2] = heap[(c2 - 1) / 2];
                        heap[(c2 - 1) / 2] = t;
                        c2 = (c2 - 1) / 2;
                    }
                }
            }
        }
    }
    if(found)
    {
        /* string pulling: the farthest cell of the path seen from a */
        int k = sb, best = sb;

        while(k >= 0 && k != sa)
        {
            if(sab_nav_clear(m, ax, az, SAB_NP_X(k), SAB_NP_Z(k), y))
            {
                best = k;
                break;
            }
            k = par[k];
        }
        if(best != sb)
        {
            *ox = SAB_NP_X(best);
            *oz = SAB_NP_Z(best);
        }
        else if(k != sb)
        {
            /* nothing seen straight (a starts at a wall): the first step */
            int f1 = sb;

            for(k = sb; k >= 0 && par[k] != sa && par[k] >= 0; k = par[k])
            {
                f1 = par[k];
            }
            *ox = SAB_NP_X(f1);
            *oz = SAB_NP_Z(f1);
        }
        res = 1;
    }
#undef SAB_NP_X
#undef SAB_NP_Z
#undef SAB_NP_H
done:
    free(g);
    free(fl);
    free(par);
    free(heap);
    free(hf);
    free(st);
    return res;
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
        /* "<name>" and its numbered twins "<name>#<k>" (one owner, several obstacles) */
        size_t ln = strlen(name);
        if(!strcmp(m->patch[i].name, name)
           || (!strncmp(m->patch[i].name, name, ln) && m->patch[i].name[ln] == '#'))
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
static double sab_pfx_intensity = 0.0, sab_pfx_threshold = 0.5, sab_pfx_spread = 2.5;
static unsigned char *sab_pfx_a = NULL;        /* graded frame before the bloom, RGB */
/* duel bullet time (sab_postfx_bullet / sab_postfx_lofi) */
static int sab_bt_vig = 0, sab_bt_box = 0;
static double sab_bt_dark = 0, sab_bt_sharp = 0, sab_bt_contrast = 0, sab_bt_red = 1, sab_bt_density = 0,
              sab_bt_aspect = 16.0 / 9.0, sab_bt_lofi_amount = 0.17;
static unsigned char *sab_bt_lofi = NULL;      /* 16^3 RGB */
/* SnB/ScreenQuad _FadeOut (the level-complete wipe): F, shader time, noise 512^2 */
static double sab_sq_fade = 0, sab_sq_time = 0;
static unsigned char *sab_sq_noise = NULL;
/* Colorful Grayscale (the train roof's quicktime): blend amount, 0 off */
static double sab_gs_amount = 0;

/* Direct-mapped cache of graded colours (pixel art: few colours per frame). */
#define SAB_PFX_CACHE 65536
static unsigned int sab_pfx_ckey[SAB_PFX_CACHE];
static unsigned char sab_pfx_cval[SAB_PFX_CACHE][3];

static int sab_postfx_grading(void)
{
    return sab_pfx_on && sab_pfx_lut;
}

int sab_postfx_active(void)
{
    /* the screen quad's fade too: the level complete wipe of a level
       without grading (the Luna park) */
    return sab_postfx_grading() || sab_gs_amount > 0 || (sab_sq_fade > 0 && sab_sq_noise);
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
    DOUBLE i = 0, t = 0.5, s = 2.5;

    sab_arg_double(varlist, 0, paramCount, &i);
    sab_arg_double(varlist, 1, paramCount, &t);
    if(paramCount > 2)
    {
        sab_arg_double(varlist, 2, paramCount, &s);
    }
    sab_pfx_intensity = i;
    sab_pfx_threshold = t;
    sab_pfx_spread = s;
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


HRESULT sab_postfx_bullet(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG vig = 0, box = 0;
    DOUBLE d = 0, sh = 0, c = 0, r = 1, den = 0, asp = 16.0 / 9.0;

    sab_arg_long(varlist, 0, paramCount, &vig);
    sab_arg_double(varlist, 1, paramCount, &d);
    sab_arg_double(varlist, 2, paramCount, &sh);
    sab_arg_double(varlist, 3, paramCount, &c);
    sab_arg_double(varlist, 4, paramCount, &r);
    sab_arg_double(varlist, 5, paramCount, &den);
    sab_arg_long(varlist, 6, paramCount, &box);
    sab_arg_double(varlist, 7, paramCount, &asp);
    sab_bt_vig = vig ? 1 : 0;
    sab_bt_dark = d;
    sab_bt_sharp = sh;
    sab_bt_contrast = c;
    sab_bt_red = r;
    sab_bt_density = den;
    sab_bt_box = box ? 1 : 0;
    sab_bt_aspect = asp > 0 ? asp : 16.0 / 9.0;
    *pretvar = NULL;
    return S_OK;
}

/*
 * sab_postfx_grayscale(amount): Colorful's Grayscale on the camera's image,
 * after the grading (lum = 0.299 r + 0.587 g + 0.114 b, rgb = lerp(rgb, lum,
 * amount)); 0 = off. Works without the grading table.
 */
HRESULT sab_postfx_grayscale(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    DOUBLE a = 0;

    sab_arg_double(varlist, 0, paramCount, &a);
    sab_gs_amount = a < 0 ? 0 : (a > 1 ? 1 : a);
    *pretvar = NULL;
    return S_OK;
}

static void sab_gs_apply(unsigned char *a, int W, int H)
{
    int i, n = W * H, k = (int)(sab_gs_amount * 256 + 0.5);

    if(k <= 0)
    {
        return;
    }
    for(i = 0; i < n; i++, a += 3)
    {
        int l = (299 * a[0] + 587 * a[1] + 114 * a[2] + 500) / 1000;
        a[0] = (unsigned char)(a[0] + (((l - a[0]) * k) >> 8));
        a[1] = (unsigned char)(a[1] + (((l - a[1]) * k) >> 8));
        a[2] = (unsigned char)(a[2] + (((l - a[2]) * k) >> 8));
    }
}

static void sab_lut3(const unsigned char *L, int n, const unsigned char *src, unsigned char *dst);

/* The post LUT (after the bloom and the bullet-time effects) and the static
   contrast vignette (CC_ContrastVignette enabled in the scene) of a camera
   chain such as the Train's (docs of the mod: postfx.md). */
static unsigned char *sab_pp_lut = NULL;
static int sab_pp_n = 0, sab_pv_on = 0;
static double sab_pv_c[2], sab_pv_d[4], sab_pv_k[3];
static unsigned int sab_pp_ckey[SAB_PFX_CACHE];
static unsigned char sab_pp_cval[SAB_PFX_CACHE][3];

//sab_postfx_post(path): an N^3 RGB table applied after the bloom ("" none)
HRESULT sab_postfx_post(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *lp = sab_arg_str(varlist, 0, paramCount);
    size_t len = 0;
    char *buf;
    int n;

    sab_ret_long(pretvar, 0);
    free(sab_pp_lut);
    sab_pp_lut = NULL;
    sab_pp_n = 0;
    if(!lp || !lp[0])
    {
        return S_OK;
    }
    buf = sab_read_file(lp, &len);
    if(!buf)
    {
        printf("sab_postfx_post: cannot open %s\n", lp);
        return S_OK;
    }
    for(n = 2; n <= 256 && (size_t)n * n * n * 3 < len; n++)
    {
    }
    if((size_t)n * n * n * 3 != len)
    {
        printf("sab_postfx_post: %s is not an N^3 RGB table (%d bytes)\n", lp, (int)len);
        free(buf);
        return S_OK;
    }
    sab_pp_lut = (unsigned char *)buf;
    sab_pp_n = n;
    memset(sab_pp_ckey, 0, sizeof(sab_pp_ckey));
    sab_ret_long(pretvar, 1);
    return S_OK;
}

//sab_postfx_vignette(on, cx, cy, sharpness, darkness, contrast, kr, kg, kb, edge): a static
//CC_ContrastVignette after the post LUT
HRESULT sab_postfx_vignette(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    LONG on = 0;
    DOUBLE v[9] = {0.5, 0.5, 0, 0, 0, 1, 1, 1, 0};
    int i;

    sab_arg_long(varlist, 0, paramCount, &on);
    for(i = 0; i < 9; i++)
    {
        sab_arg_double(varlist, i + 1, paramCount, &v[i]);
    }
    sab_pv_on = on != 0;
    sab_pv_c[0] = v[0];
    sab_pv_c[1] = v[1];
    sab_pv_d[0] = v[2] * 0.01;
    sab_pv_d[1] = v[3] * 0.02;
    sab_pv_d[2] = v[4] * 0.01;
    sab_pv_d[3] = v[8] * 0.01;
    sab_pv_k[0] = v[5];
    sab_pv_k[1] = v[6];
    sab_pv_k[2] = v[7];
    *pretvar = NULL;
    return S_OK;
}

static void sab_pp_apply(unsigned char *a, int W, int H)
{
    int x, y, ch;

    if(sab_pp_lut)
    {
        for(y = 0; y < W * H; y++)
        {
            unsigned char *p = a + (size_t)y * 3;
            unsigned int key = p[0] | (p[1] << 8) | (p[2] << 16) | 0x1000000u;
            unsigned int h = (key * 2654435761u) >> 16;

            if(sab_pp_ckey[h] != key)
            {
                sab_lut3(sab_pp_lut, sab_pp_n, p, sab_pp_cval[h]);
                sab_pp_ckey[h] = key;
            }
            p[0] = sab_pp_cval[h][0];
            p[1] = sab_pp_cval[h][1];
            p[2] = sab_pp_cval[h][2];
        }
    }
    if(!sab_pv_on)
    {
        return;
    }
    /* Hidden/CC_ContrastVignette: d = |uv - center| (uv y up),
       t = sat((d (Dx + Dy) - 0.8) / (Dx 0.799 - 0.8)), m = t^2 (3 - 2t),
       out = ((c - k) (1 - Dz (m - 1) - Dw) + k) m; the factor per row and
       column is cached in m per pixel */
    for(y = 0; y < H; y++)
    {
        double vy = (H - 1 - y + 0.5) / H - sab_pv_c[1];
        for(x = 0; x < W; x++)
        {
            unsigned char *p = a + ((size_t)y * W + x) * 3;
            double vx = (x + 0.5) / W - sab_pv_c[0], d = sqrt(vx * vx + vy * vy), t, m, f;

            t = (d * (sab_pv_d[0] + sab_pv_d[1]) - 0.8) / (sab_pv_d[0] * 0.799 - 0.8);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            m = t * t * (3 - 2 * t);
            f = 1 - sab_pv_d[2] * (m - 1) - sab_pv_d[3];
            for(ch = 0; ch < 3; ch++)
            {
                double c = ((p[ch] / 255.0 - sab_pv_k[ch]) * f + sab_pv_k[ch]) * m;
                int i = (int)(c * 255.0 + 0.5);
                p[ch] = (unsigned char)(i < 0 ? 0 : (i > 255 ? 255 : i));
            }
        }
    }
}

HRESULT sab_postfx_lofi(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *lp = sab_arg_str(varlist, 0, paramCount);
    DOUBLE a = 0.17;
    size_t len = 0;
    char *buf;

    sab_arg_double(varlist, 1, paramCount, &a);
    sab_ret_long(pretvar, 0);
    if(!lp)
    {
        return S_OK;
    }
    buf = sab_read_file(lp, &len);
    if(!buf || len != 16 * 16 * 16 * 3)
    {
        printf("sab_postfx_lofi: %s is not a 16^3 RGB table\n", lp);
        free(buf);
        return S_OK;
    }
    free(sab_bt_lofi);
    sab_bt_lofi = (unsigned char *)buf;
    sab_bt_lofi_amount = a;
    sab_ret_long(pretvar, 1);
    return S_OK;
}

/*
 * sab_postfx_screenquad(fade, time [, noise_path]): SnB/ScreenQuad's _FadeOut
 * on the graded frame (docs/level-complete.md 3): per pixel, uv snapped to the
 * frame grid, n = noise(0.3 v, 0.8 v + 0.05 cos(10 u + 4 t)) (red channel,
 * bilinear, repeat), u += 3 F (n + 0.2), u > 1 -> black. fade 0 = off.
 * noise_path: 512 x 512 bytes, top row first (loaded once).
 */
HRESULT sab_postfx_screenquad(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    DOUBLE f = 0, t = 0;
    const char *np = sab_arg_str(varlist, 2, paramCount);

    sab_arg_double(varlist, 0, paramCount, &f);
    sab_arg_double(varlist, 1, paramCount, &t);
    sab_sq_fade = f;
    sab_sq_time = t;
    if(np && !sab_sq_noise)
    {
        size_t len = 0;
        char *buf = sab_read_file(np, &len);
        if(buf && len == 512 * 512)
        {
            sab_sq_noise = (unsigned char *)buf;
        }
        else
        {
            printf("sab_postfx_screenquad: %s is not a 512^2 table\n", np);
            free(buf);
        }
    }
    sab_ret_long(pretvar, sab_sq_noise != NULL);
    return S_OK;
}

static double sab_sq_sample(double s, double t)
{
    /* Unity uv (v up) on the top-row-first table, bilinear, repeat */
    double x = s * 512 - 0.5, y = (1 - t) * 512 - 0.5, fx, fy;
    int x0, y0, x1, y1;

    x0 = (int)floor(x);
    y0 = (int)floor(y);
    fx = x - x0;
    fy = y - y0;
    x1 = ((x0 + 1) % 512 + 512) % 512;
    y1 = ((y0 + 1) % 512 + 512) % 512;
    x0 = (x0 % 512 + 512) % 512;
    y0 = (y0 % 512 + 512) % 512;
    return ((sab_sq_noise[y0 * 512 + x0] * (1 - fx) + sab_sq_noise[y0 * 512 + x1] * fx) * (1 - fy)
            + (sab_sq_noise[y1 * 512 + x0] * (1 - fx) + sab_sq_noise[y1 * 512 + x1] * fx) * fy) / 255.0;
}

/* on the 32-bit screen (4 bytes per pixel), after the curves: the quad shows the camera's render texture */
static void sab_sq_apply(unsigned char *a, int W, int H)
{
    int x, y, sx;
    unsigned char *row;

    if(sab_sq_fade <= 0 || !sab_sq_noise)
    {
        return;
    }
    row = malloc((size_t)W * 4);
    for(y = 0; y < H; y++)
    {
        double v = (H - 1 - y + 0.5) / H;
        memcpy(row, a + (size_t)y * W * 4, (size_t)W * 4);
        for(x = 0; x < W; x++)
        {
            double u = (x + 0.5) / W, n;
            unsigned char *d = a + ((size_t)y * W + x) * 4;
            n = sab_sq_sample(0.3 * v, 0.8 * v + 0.05 * cos(10 * u + 4 * sab_sq_time));
            u += 3 * sab_sq_fade * (n + 0.2);
            if(u > 1)
            {
                d[0] = d[1] = d[2] = 0;
                continue;
            }
            sx = (int)(u * W);
            sx = sx < 0 ? 0 : (sx >= W ? W - 1 : sx);
            d[0] = row[sx * 4];
            d[1] = row[sx * 4 + 1];
            d[2] = row[sx * 4 + 2];
        }
    }
    free(row);
}

static double sab_bt_sat(double v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* The bullet time chain on the graded RGB frame (vignette, letterbox, photo filter, lo-fi palette). */
static void sab_bt_apply(unsigned char *a, int W, int H)
{
    int x, y, ch, photo = sab_bt_density > 0.01;
    double Dx = sab_bt_sharp * 0.01, Dy = sab_bt_dark * 0.02, Dz = sab_bt_contrast * 0.01, Dw = 0.0;
    double k[3], off = 0;
    int top = 0, bot = H;

    if(!sab_bt_vig && !sab_bt_box && !photo)
    {
        return;
    }
    k[0] = sab_bt_red;
    k[1] = 1.0;
    k[2] = 1.0;
    if(sab_bt_box)
    {
        /* Letterbox: rows outside [off, 1 - off], off = (H - W / aspect) / 2 / H */
        off = (H - W / sab_bt_aspect) / 2.0 / H;
        if(off > 0)
        {
            top = (int)(off * H + 0.5);
            bot = H - top;
        }
    }
    for(y = 0; y < H; y++)
    {
        double vy = (H - 1 - y + 0.5) / H - 0.5;          /* uv y up */
        for(x = 0; x < W; x++)
        {
            unsigned char *p = a + ((size_t)y * W + x) * 3;
            double c[3], t, m, d, L, f;

            if(y < top || y >= bot)
            {
                p[0] = p[1] = p[2] = 0;
                continue;
            }
            for(ch = 0; ch < 3; ch++)
            {
                c[ch] = p[ch] / 255.0;
            }
            if(sab_bt_vig)
            {
                double vx = (x + 0.5) / W - 0.71;
                d = sqrt(vx * vx + vy * vy);
                t = sab_bt_sat((d * (Dx + Dy) - 0.8) / (Dx * 0.799 - 0.8));
                m = t * t * (3 - 2 * t);
                for(ch = 0; ch < 3; ch++)
                {
                    c[ch] = sab_bt_sat(((c[ch] - k[ch]) * (1 - Dz * (m - 1) - Dw) + k[ch]) * m);
                }
            }
            if(photo)
            {
                static const double col[3] = {1.0, 0.5, 0.2};
                L = c[0] * 0.222 + c[1] * 0.707 + c[2] * 0.071;
                for(ch = 0; ch < 3; ch++)
                {
                    f = col[ch] * sab_bt_sat(2 * L);
                    f = f + (1 - f) * sab_bt_sat(L - 0.5) * 2;
                    c[ch] = c[ch] + (f - c[ch]) * sab_bt_sat(L * sab_bt_density);
                }
                if(sab_bt_lofi)
                {
                    int r = (int)(sab_bt_sat(c[0]) * 15 + 0.5), g = (int)(sab_bt_sat(c[1]) * 15 + 0.5),
                        b = (int)(sab_bt_sat(c[2]) * 15 + 0.5);
                    const unsigned char *q = sab_bt_lofi + ((b * 16 + g) * 16 + r) * 3;
                    for(ch = 0; ch < 3; ch++)
                    {
                        c[ch] = c[ch] + sab_bt_lofi_amount * (q[ch] / 255.0 - c[ch]);
                    }
                }
            }
            for(ch = 0; ch < 3; ch++)
            {
                p[ch] = (unsigned char)(sab_bt_sat(c[ch]) * 255 + 0.5);
            }
        }
    }
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
    sab_lut3(sab_pfx_lut, sab_pfx_n, src, dst);
}

static void sab_lut3(const unsigned char *L, int n, const unsigned char *src, unsigned char *dst)
{
    int ch, i;
    int fr = src[0] * (n - 1), fg = src[1] * (n - 1), fb = src[2] * (n - 1);
    int r0 = fr / 255, g0 = fg / 255, b0 = fb / 255;
    int r1 = r0 < n - 1 ? r0 + 1 : r0, g1 = g0 < n - 1 ? g0 + 1 : g0, b1 = b0 < n - 1 ? b0 + 1 : b0;
    int wr = (fr - r0 * 255) * 256 / 255, wg = (fg - g0 * 255) * 256 / 255, wb = (fb - b0 * 255) * 256 / 255;
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
    int W, H, x, y, c, qw, qh, grading;
    unsigned char *px, *q = NULL, *t = NULL, *b0 = NULL, *b1 = NULL, *tmp = NULL, *u1 = NULL, *u2 = NULL;

    if(!sab_postfx_active() || !scr || scr->pixelformat != PIXEL_32)
    {
        return;
    }
    W = scr->width;
    H = scr->height;
    sab_pfx_a = realloc(sab_pfx_a, (size_t)W * H * 3);
    px = scr->data;
    grading = sab_postfx_grading();
    for(y = 0; y < H; y++)
    {
        for(x = 0; x < W; x++)
        {
            unsigned char *s = px + ((size_t)y * W + x) * 4, *d = sab_pfx_a + ((size_t)y * W + x) * 3;
            if(grading)
            {
                sab_pfx_lookup(s, d);
            }
            else
            {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
    }
    if(grading && sab_pfx_intensity > 0 && W % 4 == 0 && H % 4 == 0)
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
        /* BloomOptimized-style iterations: spread, then spread x 1.25 */
        sab_pfx_blur(q, b0, qw, qh, sab_pfx_spread * qh / 512.0, tmp);
        sab_pfx_blur(b0, b1, qw, qh, sab_pfx_spread * 1.25 * qh / 512.0, tmp);
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
    sab_bt_apply(sab_pfx_a, W, H);
    if(grading)
    {
        sab_pp_apply(sab_pfx_a, W, H);
    }
    sab_gs_apply(sab_pfx_a, W, H);
    for(y = 0; y < H; y++)
    {
        for(x = 0; x < W; x++)
        {
            unsigned char *d = px + ((size_t)y * W + x) * 4, *a = sab_pfx_a + ((size_t)y * W + x) * 3;
            if(grading && sab_pfx_have_curves && sab_pfx_curves_on)
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
    sab_sq_apply(px, W, H);
}


/* ===================================================== scrolling background */

/*
 * Unity's ScrollRecycler (docs of the mod: level2-train.md): layers of
 * sprites moved every frame by dt x v (per layer of a recycler) and
 * recycled to the right when they leave the camera on the left. The file
 * (pipeline/scroll_build.py):
 *
 *   level <x0> <top>         game x of the level picture's column 0, y + z of row 0
 *   image <n> <file>
 *   layer <index> <owner> <pad> "<path>"
 *   recycler <index> <active> <margin> <n> <layer> <v> ... "<path>"
 *   piece <layer> <image> <col> <row> <depth> <blend> <order> <group> <x> <half width> <on> "<path>"
 *   uv <px per second> "<path>"   the pieces at or below the path (listed
 *                            before) scroll their image left at that speed,
 *                            wrapping (a material whose texture offset moves
 *                            with _Time: the Train's SnB/Scroller windows)
 *   emitter <owner> <x> <y> <z> <depth> <dx> <dy> <dz> <ux> <uy> <uz> <vx> <vy> <vz> <radius> <rate>
 *           <life> <speed> <width> <length> <alpha min> <alpha max> "<path>"
 *   end
 *
 * An emitter is a particle system shown with its owner recycler: particles
 * start on the disc of the radius around (x, y, z) (axes u, v), fly along
 * d at the speed for the lifetime, and are drawn as white streaks (alpha
 * between the two values) of the length back along d, at the depth.
 *
 * col, row: image top-left in level pixels at rest; x, half width: world
 * units (SAB_SC_PPU pixels each). A piece is drawn when it is on and its
 * layer's owner recycler (if any) is active. group 0 is queued below the
 * level's bglayers (z MIN_INT, negative sort ids in file order), 1 at its
 * depth (sorted with the entities), 2 below the fglayers (FRONTPANEL_Z).
 *
 * The images are RGBA PNGs drawn by the core itself (sprite queue
 * callbacks, 32-bit screens): per-pixel alpha, blend 1 additive, 2 multiply
 * (the image is the multiplier). The engine's sprites are 8-bit indexed.
 */

#define SAB_SC_PPU 20.0f
#define SAB_SC_CAMW 32.0f       /* CameraManager.WORLD_CAMERA_WIDTH */
#define SAB_SC_MAXV 32

typedef struct
{
    int w, h;
    unsigned char *px;          /* RGBA */
} sab_sc_image;

typedef struct
{
    int layer, img, col, row, depth, blend, group, on, w, h;
    float x0, hw, dx, alpha, alpha0;
    char *path;
    int anim, nfr, *fr;         /* animated piece: anim index, frames (image, col, row; image -1 hidden) */
    int u;                      /* texture offset in px, wrapped (a scrolling material) */
    float uspeed, uacc;         /* "uv" line: px per second, accumulated */
} sab_sc_piece;

typedef struct
{
    int layer, nfr, fps, frame;
    char *path;
} sab_sc_anim;

typedef struct
{
    int owner, pad;
    float off, ox, oy;          /* recycler motion; script offset (sab_scroll_offset) */
    char *path;
} sab_sc_layer;

typedef struct
{
    int active, active0, margin, n;
    int layer[SAB_SC_MAXV];
    float v[SAB_SC_MAXV];
    char *path;
} sab_sc_rec;

#define SAB_SC_MAXP 256

typedef struct
{
    float x, y, z, a, t;
} sab_sc_particle;

typedef struct
{
    int owner, depth, n;
    float x, y, z, d[3], u[3], v[3], radius, rate, life, speed, width, length, amin, amax, acc;
    sab_sc_particle p[SAB_SC_MAXP];
} sab_sc_emitter;

static struct
{
    char path[256];
    int nimg, nlayer, nrec, npiece, nemit, nanim, ok;
    sab_sc_anim *anim;
    float x0, top;              /* "level <x0> <top>": game x of column 0, y + z of row 0 */
    sab_sc_emitter *emit;
    sab_sc_image *img;
    sab_sc_layer *layer;
    sab_sc_rec *rec;
    sab_sc_piece *piece;
} sab_sc;

static void sab_sc_free(void)
{
    int i;

    for(i = 0; i < sab_sc.nimg; i++)
    {
        free(sab_sc.img[i].px);
    }
    for(i = 0; i < sab_sc.nlayer; i++)
    {
        free(sab_sc.layer[i].path);
    }
    for(i = 0; i < sab_sc.nrec; i++)
    {
        free(sab_sc.rec[i].path);
    }
    for(i = 0; i < sab_sc.npiece; i++)
    {
        free(sab_sc.piece[i].path);
        free(sab_sc.piece[i].fr);
    }
    for(i = 0; i < sab_sc.nanim; i++)
    {
        free(sab_sc.anim[i].path);
    }
    free(sab_sc.anim);
    free(sab_sc.img);
    free(sab_sc.layer);
    free(sab_sc.rec);
    free(sab_sc.piece);
    free(sab_sc.emit);
    memset(&sab_sc, 0, sizeof(sab_sc));
}

/* the quoted tail of a line ("<path>"), or "" */
static char *sab_sc_quoted(const char *line)
{
    const char *a = strchr(line, '"'), *b;
    char *r;

    if(!a || !(b = strrchr(line, '"')) || b <= a)
    {
        return strdup("");
    }
    r = malloc(b - a);
    memcpy(r, a + 1, b - a - 1);
    r[b - a - 1] = 0;
    return r;
}

static void sab_sc_reset(void)
{
    int i;

    for(i = 0; i < sab_sc.nlayer; i++)
    {
        sab_sc.layer[i].off = 0;
        sab_sc.layer[i].ox = 0;
        sab_sc.layer[i].oy = 0;
    }
    for(i = 0; i < sab_sc.nrec; i++)
    {
        sab_sc.rec[i].active = sab_sc.rec[i].active0;
    }
    for(i = 0; i < sab_sc.nemit; i++)
    {
        sab_sc.emit[i].n = 0;
        sab_sc.emit[i].acc = 0;
    }
    for(i = 0; i < sab_sc.nanim; i++)
    {
        sab_sc.anim[i].frame = 0;
    }
}

/* an RGBA PNG of the pak */
static int sab_sc_load_png(const char *file, sab_sc_image *im)
{
    size_t len;
    char *buf = sab_read_file(file, &len);
    png_image pi;

    if(!buf)
    {
        return 0;
    }
    memset(&pi, 0, sizeof(pi));
    pi.version = PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_memory(&pi, buf, len))
    {
        free(buf);
        return 0;
    }
    pi.format = PNG_FORMAT_RGBA;
    im->w = pi.width;
    im->h = pi.height;
    im->px = malloc(PNG_IMAGE_SIZE(pi));
    if(!im->px || !png_image_finish_read(&pi, NULL, im->px, 0, NULL))
    {
        free(im->px);
        im->px = NULL;
        png_image_free(&pi);
        free(buf);
        return 0;
    }
    free(buf);
    return 1;
}

static int sab_sc_parse(const char *path)
{
    size_t len;
    char *buf = sab_read_file(path, &len), *line, *next;
    int pass;

    if(!buf)
    {
        printf("sab_scroll_load: cannot open %s\n", path);
        return 0;
    }
    for(pass = 0; pass < 2; pass++)
    {
        int ni = 0, nl = 0, nr = 0, np = 0, ne = 0, na = 0;
        char *copy = strdup(buf);

        for(line = copy; line && *line; line = next)
        {
            char kw[16] = "";

            next = strpbrk(line, "\r\n");
            if(next)
            {
                *next++ = 0;
                while(*next == '\r' || *next == '\n')
                {
                    next++;
                }
            }
            if(sscanf(line, "%15s", kw) != 1 || kw[0] == '#')
            {
                continue;
            }
            if(!strcmp(kw, "level"))
            {
                if(pass == 1)
                {
                    sscanf(line, "level %f %f", &sab_sc.x0, &sab_sc.top);
                }
            }
            else if(!strcmp(kw, "image"))
            {
                int n;
                char file[256];

                if(sscanf(line, "image %d %255s", &n, file) == 2)
                {
                    if(pass == 0)
                    {
                        if(n + 1 > ni)
                        {
                            ni = n + 1;
                        }
                    }
                    else if(n >= 0 && n < sab_sc.nimg)
                    {
                        if(!sab_sc_load_png(file, &sab_sc.img[n]))
                        {
                            printf("sab_scroll_load: cannot load %s\n", file);
                        }
                    }
                }
            }
            else if(!strcmp(kw, "layer"))
            {
                if(pass == 1)
                {
                    sab_sc_layer *l = &sab_sc.layer[nl];

                    sscanf(line, "layer %*d %d %d", &l->owner, &l->pad);
                    l->path = sab_sc_quoted(line);
                }
                nl++;
            }
            else if(!strcmp(kw, "recycler"))
            {
                if(pass == 1)
                {
                    sab_sc_rec *r = &sab_sc.rec[nr];
                    char *p = line;
                    int k, used;

                    sscanf(p, "recycler %*d %d %d %d%n", &r->active0, &r->margin, &r->n, &used);
                    p += used;
                    if(r->n > SAB_SC_MAXV)
                    {
                        r->n = SAB_SC_MAXV;
                    }
                    for(k = 0; k < r->n; k++)
                    {
                        sscanf(p, "%d %f%n", &r->layer[k], &r->v[k], &used);
                        p += used;
                    }
                    r->path = sab_sc_quoted(line);
                }
                nr++;
            }
            else if(!strcmp(kw, "emitter"))
            {
                if(pass == 1)
                {
                    sab_sc_emitter *e = &sab_sc.emit[ne];

                    sscanf(line, "emitter %d %f %f %f %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                           &e->owner, &e->x, &e->y, &e->z, &e->depth, &e->d[0], &e->d[1], &e->d[2], &e->u[0],
                           &e->u[1], &e->u[2], &e->v[0], &e->v[1], &e->v[2], &e->radius, &e->rate, &e->life,
                           &e->speed, &e->width, &e->length, &e->amin, &e->amax);
                }
                ne++;
            }
            else if(!strcmp(kw, "piece"))
            {
                if(pass == 1)
                {
                    sab_sc_piece *q = &sab_sc.piece[np];
                    int order;

                    q->alpha = 1;
                    q->anim = -1;
                    sscanf(line, "piece %d %d %d %d %d %d %d %d %f %f %d %f", &q->layer, &q->img, &q->col, &q->row,
                           &q->depth, &q->blend, &order, &q->group, &q->x0, &q->hw, &q->on, &q->alpha);
                    q->path = sab_sc_quoted(line);
                }
                np++;
            }
            else if(!strcmp(kw, "uv"))
            {
                float sp = 0;

                if(pass == 1 && sscanf(line, "uv %f", &sp) == 1)
                {
                    char *path = sab_sc_quoted(line);
                    size_t ln = strlen(path);
                    int k;

                    for(k = 0; k < np; k++)
                    {
                        const char *pp = sab_sc.piece[k].path;

                        if(pp && !strncmp(pp, path, ln) && (pp[ln] == 0 || pp[ln] == '/'))
                        {
                            sab_sc.piece[k].uspeed = sp;
                        }
                    }
                    free(path);
                }
            }
            else if(!strcmp(kw, "anim"))
            {
                if(pass == 1)
                {
                    sab_sc_anim *an = &sab_sc.anim[na];

                    sscanf(line, "anim %*d %d %d %d", &an->layer, &an->nfr, &an->fps);
                    an->path = sab_sc_quoted(line);
                }
                na++;
            }
            else if(!strcmp(kw, "apiece"))
            {
                if(pass == 1)
                {
                    sab_sc_piece *q = &sab_sc.piece[np];
                    int order, used, k;
                    char *p = line, tok[64];

                    q->alpha = 1;
                    sscanf(p, "apiece %d %d %d %d %d %d%n", &q->anim, &q->depth, &q->blend, &order, &q->group, &q->on,
                           &used);
                    p += used;
                    if(q->anim < 0 || q->anim >= na)
                    {
                        q->anim = 0;
                    }
                    q->nfr = 0;
                    q->fr = NULL;
                    q->path = sab_sc_quoted(line);
                    /* frames up to the quoted path */
                    for(k = 0; sscanf(p, " %63s%n", tok, &used) == 1 && tok[0] != '"'; k++)
                    {
                        int im = -1, c = 0, r = 0;

                        p += used;
                        if(strcmp(tok, "-"))
                        {
                            sscanf(tok, "%d:%d:%d", &im, &c, &r);
                        }
                        q->fr = realloc(q->fr, sizeof(int) * 3 * (k + 1));
                        q->fr[k * 3] = im;
                        q->fr[k * 3 + 1] = c;
                        q->fr[k * 3 + 2] = r;
                        q->nfr = k + 1;
                    }
                    q->img = q->nfr ? q->fr[0] : -1;
                    q->col = q->nfr ? q->fr[1] : 0;
                    q->row = q->nfr ? q->fr[2] : 0;
                }
                np++;
            }
        }
        free(copy);
        if(pass == 0)
        {
            sab_sc.nimg = ni;
            sab_sc.nlayer = nl;
            sab_sc.nrec = nr;
            sab_sc.npiece = np;
            sab_sc.nemit = ne;
            sab_sc.emit = calloc(ne ? ne : 1, sizeof(*sab_sc.emit));
            sab_sc.nanim = na;
            sab_sc.anim = calloc(na ? na : 1, sizeof(*sab_sc.anim));
            sab_sc.img = calloc(ni ? ni : 1, sizeof(*sab_sc.img));
            sab_sc.layer = calloc(nl ? nl : 1, sizeof(*sab_sc.layer));
            sab_sc.rec = calloc(nr ? nr : 1, sizeof(*sab_sc.rec));
            sab_sc.piece = calloc(np ? np : 1, sizeof(*sab_sc.piece));
        }
    }
    free(buf);
    {
        int i;

        for(i = 0; i < sab_sc.npiece; i++)
        {
            sab_sc_piece *q = &sab_sc.piece[i];
            sab_sc_image *im = (q->img >= 0 && q->img < sab_sc.nimg) ? &sab_sc.img[q->img] : NULL;

            q->alpha0 = q->alpha;
            if(q->fr)
            {
                int k;

                q->layer = sab_sc.anim[q->anim].layer;
                for(k = 0; k < q->nfr; k++)
                {
                    int n = q->fr[k * 3];

                    if(n >= 0 && (n >= sab_sc.nimg || !sab_sc.img[n].px))
                    {
                        q->fr[k * 3] = -1;
                    }
                }
            }
            if(q->layer < 0 || q->layer >= sab_sc.nlayer)
            {
                q->layer = 0;
            }
            if(!q->fr && (!im || !im->px))
            {
                q->on = 0;
                q->img = -1;
            }
            q->w = im ? im->w : 0;
            q->h = im ? im->h : 0;
        }
    }
    strncpy(sab_sc.path, path, sizeof(sab_sc.path) - 1);
    sab_sc.ok = 1;
    return 1;
}

/* sprite queue callback: the piece's image at (x, y) of a 32-bit screen */
static void sab_sc_blit(s_screen *scr, int x, int y, void *arg)
{
    sab_sc_piece *q = (sab_sc_piece *)arg;
    sab_sc_image *im;
    int i, j, x0, y0, x1, y1;

    if(!scr || scr->pixelformat != PIXEL_32 || q->img < 0 || q->img >= sab_sc.nimg)
    {
        return;
    }
    im = &sab_sc.img[q->img];
    if(!im->px)
    {
        fprintf(stderr, "sab_sc_blit: image %d not loaded (%s)\n", q->img, q->path);
        return;
    }
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + im->w > scr->width ? scr->width : x + im->w;
    y1 = y + im->h > scr->height ? scr->height : y + im->h;
    for(j = y0; j < y1; j++)
    {
        const unsigned char *row = im->px + (size_t)(j - y) * im->w * 4;
        const unsigned char *s = row + (size_t)(x0 - x) * 4;
        unsigned char *d = (unsigned char *)scr->data + ((size_t)j * scr->width + x0) * 4;
        int sx = ((x0 - x + q->u) % im->w + im->w) % im->w;

        for(i = x0; i < x1; i++, s += 4, d += 4)
        {
            if(q->u)
            {
                /* the texture wraps (a UV-scrolled material) */
                s = row + (size_t)sx * 4;
                if(++sx == im->w)
                {
                    sx = 0;
                }
            }
            unsigned a = s[3];

            if(q->alpha < 1)
            {
                a = (unsigned)(a * q->alpha + 0.5f);
            }
            if(!a)
            {
                continue;
            }
            if(q->blend == 2)
            {
                d[0] = (unsigned char)((d[0] * s[0] + 127) / 255);
                d[1] = (unsigned char)((d[1] * s[1] + 127) / 255);
                d[2] = (unsigned char)((d[2] * s[2] + 127) / 255);
            }
            else if(q->blend == 1)
            {
                unsigned r = d[0] + (s[0] * a + 127) / 255, g = d[1] + (s[1] * a + 127) / 255,
                         b = d[2] + (s[2] * a + 127) / 255;

                d[0] = r > 255 ? 255 : r;
                d[1] = g > 255 ? 255 : g;
                d[2] = b > 255 ? 255 : b;
            }
            else if(a == 255)
            {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
            else
            {
                d[0] = (unsigned char)((s[0] * a + d[0] * (255 - a) + 127) / 255);
                d[1] = (unsigned char)((s[1] * a + d[1] * (255 - a) + 127) / 255);
                d[2] = (unsigned char)((s[2] * a + d[2] * (255 - a) + 127) / 255);
            }
        }
    }
}

/* sprite queue callback: the streaks of an emitter (stretched billboards
   along -x or +x: horizontal, length and width in world units) */
static void sab_sc_streaks(s_screen *scr, int x, int y, void *arg)
{
    sab_sc_emitter *e = (sab_sc_emitter *)arg;
    int k, i, j, w;

    if(!scr || scr->pixelformat != PIXEL_32)
    {
        return;
    }
    w = (int)(e->width * SAB_SC_PPU + 0.5f);
    if(w < 1)
    {
        w = 1;
    }
    for(k = 0; k < e->n; k++)
    {
        const sab_sc_particle *p = &e->p[k];
        /* the billboard reaches back from the particle, against its motion */
        float xa = p->x, xb = p->x - e->d[0] * e->length;
        int c0 = x + (int)floorf(((xa < xb ? xa : xb) - sab_sc.x0) * SAB_SC_PPU);
        int c1 = x + (int)floorf(((xa < xb ? xb : xa) - sab_sc.x0) * SAB_SC_PPU);
        int r0 = y + (int)floorf((sab_sc.top - (p->y + p->z)) * SAB_SC_PPU - w * 0.5f + 0.5f);
        unsigned a = (unsigned)(p->a * 255.0f + 0.5f);

        if(!a)
        {
            continue;
        }
        if(c0 < 0)
        {
            c0 = 0;
        }
        if(c1 > scr->width)
        {
            c1 = scr->width;
        }
        for(j = r0; j < r0 + w; j++)
        {
            unsigned char *d;

            if(j < 0 || j >= scr->height)
            {
                continue;
            }
            d = (unsigned char *)scr->data + ((size_t)j * scr->width + c0) * 4;
            for(i = c0; i < c1; i++, d += 4)
            {
                d[0] = (unsigned char)((255 * a + d[0] * (255 - a) + 127) / 255);
                d[1] = (unsigned char)((255 * a + d[1] * (255 - a) + 127) / 255);
                d[2] = (unsigned char)((255 * a + d[2] * (255 - a) + 127) / 255);
            }
        }
    }
}

/* own generator: the engine's random sequence is left alone */
static unsigned sab_sc_seed = 12345u;

static float sab_sc_rand01(void)
{
    sab_sc_seed = sab_sc_seed * 1664525u + 1013904223u;
    return (float)(sab_sc_seed >> 8) / 16777216.0f;
}

//sab_scroll_load(path): "" unloads
HRESULT sab_scroll_load(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    int i;

    if(!path)
    {
        printf("Function requires a path: sab_scroll_load(path)\n");
        *pretvar = NULL;
        return E_FAIL;
    }
    if(sab_sc.ok && !strcmp(sab_sc.path, path))
    {
        /* same file: back to the rest state */
        sab_sc_reset();
        for(i = 0; i < sab_sc.npiece; i++)
        {
            sab_sc.piece[i].dx = 0;
            sab_sc.piece[i].alpha = sab_sc.piece[i].alpha0;
        }
        sab_ret_long(pretvar, 1);
        return S_OK;
    }
    sab_sc_free();
    if(!path[0])
    {
        sab_ret_long(pretvar, 1);
        return S_OK;
    }
    sab_ret_long(pretvar, sab_sc_parse(path));
    sab_sc_reset();
    return S_OK;
}

/* the pieces' "on" flags of the file are reloaded with the file only: they
   change through sab_scroll_show */

//sab_scroll_active(path, on): a recycler (its object's active state)
HRESULT sab_scroll_active(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    LONG on = 0;
    int i, found = 0;

    sab_arg_long(varlist, 1, paramCount, &on);
    for(i = 0; path && i < sab_sc.nrec; i++)
    {
        if(!strcmp(sab_sc.rec[i].path, path))
        {
            sab_sc.rec[i].active = on ? 1 : 0;
            found = 1;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_show(path, on): every piece at or below that object
HRESULT sab_scroll_show(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    LONG on = 0;
    int i, found = 0;
    size_t ln;

    sab_arg_long(varlist, 1, paramCount, &on);
    if(!path)
    {
        sab_ret_long(pretvar, 0);
        return S_OK;
    }
    ln = strlen(path);
    for(i = 0; i < sab_sc.npiece; i++)
    {
        const char *p = sab_sc.piece[i].path;

        if(!strncmp(p, path, ln) && (p[ln] == 0 || p[ln] == '/'))
        {
            sab_sc.piece[i].on = on ? 1 : 0;
            found++;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_offset(path, dx, dy): a layer drawn moved by (dx, dy) world units
//(y up), e.g. a rigid layer no recycler moves
HRESULT sab_scroll_offset(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    DOUBLE dx = 0, dy = 0;
    int i, found = 0;

    sab_arg_double(varlist, 1, paramCount, &dx);
    sab_arg_double(varlist, 2, paramCount, &dy);
    for(i = 0; path && i < sab_sc.nlayer; i++)
    {
        if(!strcmp(sab_sc.layer[i].path, path))
        {
            sab_sc.layer[i].ox = (float)dx;
            sab_sc.layer[i].oy = (float)dy;
            found = 1;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_anim(root, t): the clip of an animated root at t seconds (frame
//1 + t x fps, the last one held); t < 0: the rest pose
HRESULT sab_scroll_anim(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    DOUBLE t = -1;
    int i, found = 0;

    sab_arg_double(varlist, 1, paramCount, &t);
    for(i = 0; path && i < sab_sc.nanim; i++)
    {
        sab_sc_anim *an = &sab_sc.anim[i];

        if(!strcmp(an->path, path))
        {
            an->frame = t < 0 ? 0 : 1 + (int)floor(t * an->fps + 1e-6);
            if(an->frame >= an->nfr)
            {
                an->frame = an->nfr - 1;
            }
            found = 1;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_alpha(path, a): the opacity (0..1) of every piece at or below that
//object path; returns the count
HRESULT sab_scroll_alpha(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    DOUBLE a = 1;
    int i, found = 0;
    size_t ln;

    sab_arg_double(varlist, 1, paramCount, &a);
    if(!path)
    {
        sab_ret_long(pretvar, 0);
        return S_OK;
    }
    ln = strlen(path);
    for(i = 0; i < sab_sc.npiece; i++)
    {
        const char *p = sab_sc.piece[i].path;

        if(!strncmp(p, path, ln) && (p[ln] == 0 || p[ln] == '/'))
        {
            sab_sc.piece[i].alpha = a < 0 ? 0 : (a > 1 ? 1 : (float)a);
            found++;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_uv(path, px): the pieces of that path (or below it) drawn with
//their image shifted left by px, wrapping (a material whose texture offset
//scrolls, e.g. the Train's SnB/Scroller window quads); returns the count
HRESULT sab_scroll_uv(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    const char *path = sab_arg_str(varlist, 0, paramCount);
    LONG u = 0;
    int i, found = 0;
    size_t ln;

    sab_arg_long(varlist, 1, paramCount, &u);
    if(!path)
    {
        sab_ret_long(pretvar, 0);
        return S_OK;
    }
    ln = strlen(path);
    for(i = 0; i < sab_sc.npiece; i++)
    {
        const char *p = sab_sc.piece[i].path;

        if(!strncmp(p, path, ln) && (p[ln] == 0 || p[ln] == '/'))
        {
            sab_sc.piece[i].u = (int)u;
            found++;
        }
    }
    sab_ret_long(pretvar, found);
    return S_OK;
}

//sab_scroll_frame(dt, camleft): moves and recycles the active recyclers'
//layers (camleft: world x of the screen's left edge), then queues the shown
//pieces; once per frame
HRESULT sab_scroll_frame(ScriptVariant **varlist, ScriptVariant **pretvar, int paramCount)
{
    DOUBLE dt = 0, cl = 0;
    int i, k, j;

    *pretvar = NULL;
    if(!sab_sc.ok || !sab_arg_double(varlist, 0, paramCount, &dt) || !sab_arg_double(varlist, 1, paramCount, &cl))
    {
        return S_OK;
    }
    for(i = 0; i < sab_sc.nrec; i++)
    {
        sab_sc_rec *r = &sab_sc.rec[i];

        if(!r->active)
        {
            continue;
        }
        for(k = 0; k < r->n; k++)
        {
            int li = r->layer[k];
            sab_sc_layer *l;

            if(li < 0 || li >= sab_sc.nlayer)
            {
                continue;
            }
            l = &sab_sc.layer[li];
            l->off += (float)dt * r->v[k];
            /* Layer.Recylce(extraMargin) */
            for(j = 0; j < sab_sc.npiece; j++)
            {
                sab_sc_piece *q = &sab_sc.piece[j];
                float x;

                if(q->layer != li)
                {
                    continue;
                }
                x = q->x0 + l->off + q->dx;
                if(x < (float)cl - r->margin - q->hw - l->pad)
                {
                    q->dx += SAB_SC_CAMW + r->margin * 2 + q->hw + l->pad;
                }
            }
        }
    }
    /* the scrolling materials (uv lines) */
    for(j = 0; j < sab_sc.npiece; j++)
    {
        sab_sc_piece *q = &sab_sc.piece[j];

        if(q->uspeed != 0 && q->img >= 0 && q->img < sab_sc.nimg && sab_sc.img[q->img].w > 0)
        {
            float w = (float)sab_sc.img[q->img].w;

            q->uacc = fmodf(q->uacc + (float)dt * q->uspeed, w);
            if(q->uacc < 0)
            {
                q->uacc += w;
            }
            q->u = (int)q->uacc;
        }
    }
    /* the emitters of the active recyclers: emission, motion, death */
    for(i = 0; i < sab_sc.nemit; i++)
    {
        sab_sc_emitter *e = &sab_sc.emit[i];

        if(e->owner >= 0 && e->owner < sab_sc.nrec && !sab_sc.rec[e->owner].active)
        {
            continue;
        }
        for(k = 0; k < e->n; k++)
        {
            sab_sc_particle *p = &e->p[k];

            p->t += (float)dt;
            if(p->t >= e->life)
            {
                e->p[k--] = e->p[--e->n];
                continue;
            }
            p->x += e->d[0] * e->speed * (float)dt;
            p->y += e->d[1] * e->speed * (float)dt;
            p->z += e->d[2] * e->speed * (float)dt;
        }
        e->acc += (float)dt * e->rate;
        while(e->acc >= 1.0f)
        {
            float r = e->radius * sqrtf(sab_sc_rand01()), th = 6.2831853f * sab_sc_rand01();
            float cu = r * cosf(th), cv = r * sinf(th);
            sab_sc_particle *p;

            e->acc -= 1.0f;
            if(e->n >= SAB_SC_MAXP)
            {
                continue;
            }
            p = &e->p[e->n++];
            p->x = e->x + e->u[0] * cu + e->v[0] * cv;
            p->y = e->y + e->u[1] * cu + e->v[1] * cv;
            p->z = e->z + e->u[2] * cu + e->v[2] * cv;
            p->a = e->amin + (e->amax - e->amin) * sab_sc_rand01();
            p->t = 0;
        }
        /* screen origin of the level: x = -advancex, row of y + z = 0 */
        /* (x, y): screen place of the level picture's top-left */
        spriteq_add_callback((int)floorf(-advancex + 0.5f), (int)floorf(-advancey + 0.5f), e->depth, 0, sab_sc_streaks, e);
    }
    for(j = 0; j < sab_sc.npiece; j++)
    {
        sab_sc_piece *q = &sab_sc.piece[j];
        sab_sc_layer *l = &sab_sc.layer[q->layer];
        int x, y, z, sid;

        if(q->fr)
        {
            /* animated piece: the frame of its clip (0: the rest pose, shown
               when on; the clip's frames show what the clip shows) */
            int f = sab_sc.anim[q->anim].frame;

            if(f >= q->nfr)
            {
                f = q->nfr - 1;
            }
            if(f == 0 && !q->on)
            {
                continue;
            }
            q->img = q->fr[f * 3];
            q->col = q->fr[f * 3 + 1];
            q->row = q->fr[f * 3 + 2];
            if(q->img < 0)
            {
                continue;
            }
            q->w = sab_sc.img[q->img].w;
            q->h = sab_sc.img[q->img].h;
        }
        else if(!q->on)
        {
            continue;
        }
        if(q->img < 0 || q->alpha <= 0 || (l->owner >= 0 && l->owner < sab_sc.nrec && !sab_sc.rec[l->owner].active))
        {
            continue;
        }
        x = (int)floorf(q->col + (l->off + q->dx + l->ox) * SAB_SC_PPU - advancex + 0.5f);
        y = (int)floorf(q->row - l->oy * SAB_SC_PPU - advancey + 0.5f);
        if(x >= videomodes.hRes || x + q->w <= 0 || y >= videomodes.vRes || y + q->h <= 0)
        {
            continue;
        }
        if(q->group == 0)
        {
            z = MIN_INT;
            sid = -1000000 + j;
        }
        else if(q->group == 1)
        {
            z = q->depth;
            sid = 0;
        }
        else
        {
            z = FRONTPANEL_Z;
            sid = -1000000 + j;
        }
        spriteq_add_callback(x, y, z, sid, sab_sc_blit, q);
    }
    return S_OK;
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
    List_InsertAfter(list, (void *)sab_nav_path, "sab_nav_path");
    List_InsertAfter(list, (void *)sab_camera, "sab_camera");
    List_InsertAfter(list, (void *)sab_postfx_load, "sab_postfx_load");
    List_InsertAfter(list, (void *)sab_postfx, "sab_postfx");
    List_InsertAfter(list, (void *)sab_postfx_bloom, "sab_postfx_bloom");
    List_InsertAfter(list, (void *)sab_postfx_zsplit, "sab_postfx_zsplit");
    List_InsertAfter(list, (void *)sab_postfx_bullet, "sab_postfx_bullet");
    List_InsertAfter(list, (void *)sab_postfx_lofi, "sab_postfx_lofi");
    List_InsertAfter(list, (void *)sab_postfx_screenquad, "sab_postfx_screenquad");
    List_InsertAfter(list, (void *)sab_postfx_grayscale, "sab_postfx_grayscale");
    List_InsertAfter(list, (void *)sab_scroll_load, "sab_scroll_load");
    List_InsertAfter(list, (void *)sab_scroll_active, "sab_scroll_active");
    List_InsertAfter(list, (void *)sab_scroll_show, "sab_scroll_show");
    List_InsertAfter(list, (void *)sab_scroll_frame, "sab_scroll_frame");
    List_InsertAfter(list, (void *)sab_scroll_offset, "sab_scroll_offset");
    List_InsertAfter(list, (void *)sab_scroll_anim, "sab_scroll_anim");
    List_InsertAfter(list, (void *)sab_scroll_alpha, "sab_scroll_alpha");
    List_InsertAfter(list, (void *)sab_scroll_uv, "sab_scroll_uv");
    List_InsertAfter(list, (void *)sab_postfx_post, "sab_postfx_post");
    List_InsertAfter(list, (void *)sab_postfx_vignette, "sab_postfx_vignette");
}
