/*
 * OpenBOR multi-engine libretro core - libretro entry points.
 *
 * Several OpenBOR engine versions are linked into the core (see ob_api.h).
 * One is chosen per game: by the "Engine version" core option, by an
 * engine tag in the pak's file name (pak_tag.h), or automatically from the
 * contents of the .pak (detect.c). If the chosen
 * engine fails while loading the game, the next candidate is tried.
 *
 * OpenBOR is written as a blocking program: openborMain() never returns and
 * presents frames from deep inside its own loops. The engine therefore runs
 * on a libco cooperative thread and gives control back after each frame:
 *
 *   retro_run() --co_switch--> engine ... video_copy_screen()
 *                <--co_switch-- host yield_frame()
 *
 * Everything runs on the frontend's thread; the mixer is driven from
 * retro_run() while the engine is suspended. Only WebM playback (engine
 * 8025_webm) decodes in threads of its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "libretro.h"
#include "libco.h"

#include "ob_api.h"
#include "detect.h"
#include "pak_tag.h"

#define FPS              60.0
#define AUDIO_RATE       44100
#define ENGINE_STACK     (16 * 1024 * 1024)
#define PATH_LEN         1024

/* ------------------------------------------------------------------------ */
/* Available engines (oldest first)                                         */
/* ------------------------------------------------------------------------ */

typedef const ob_engine *(*engine_accessor)(void);

/* id: upstream build (detection vocabulary, data directory);
 * name: engine name in the core option and in pak tags (pak_tag.h);
 * webm: built with WebM video playback (preferred for paks with videos) */
static const struct { int id; const char *name; int webm; engine_accessor get; } engines[] = {
#ifdef HAVE_ENGINE_3711
    { 3711, "3711", 0, ob_engine_3711 },
#endif
#ifdef HAVE_ENGINE_4153
    { 4153, "4153", 0, ob_engine_4153 },
#endif
#ifdef HAVE_ENGINE_6391
    { 6391, "6391", 0, ob_engine_6391 },
#endif
#ifdef HAVE_ENGINE_7555
    { 7555, "7555", 0, ob_engine_7555 },
#endif
#ifdef HAVE_ENGINE_7735
    { 7735, "7735", 0, ob_engine_7735 },
#endif
#ifdef HAVE_ENGINE_8025
    { 8025, "8025", 0, ob_engine_8025 },
#endif
#ifdef HAVE_ENGINE_8025_webm
    { 8025, "8025_webm", 1, ob_engine_8025_webm },
#endif
};
#define NUM_ENGINES ((int)(sizeof(engines) / sizeof(engines[0])))

/* ------------------------------------------------------------------------ */
/* Frontend callbacks                                                       */
/* ------------------------------------------------------------------------ */

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;
static struct retro_rumble_interface rumble;
static int have_rumble;
static retro_perf_get_time_usec_t perf_get_time_usec;
static bool can_dupe;
static bool have_bitmasks;

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

enum { ENGINE_NONE, ENGINE_RUNNING, ENGINE_EXITED };

static const ob_engine *engine;            /* engine in use */
static int engine_used[16];                /* engines already started (single use each) */
static cothread_t main_thread;
static cothread_t engine_thread;
static int engine_state = ENGINE_NONE;
static int engine_exit_status = 0;
static int frame_ready = 0;
static int unloading = 0;
static int loading = 0;                     /* inside retro_load_game() */
static int shutdown_requested = 0;

static int mode_known = 0;
static int av_width = 320, av_height = 240;
static int av_max_width = 320, av_max_height = 240;

static uint64_t frame_count = 0;
static uint64_t virtual_usec = 0;

static int16_t audio_buffer[(AUDIO_RATE / 50 + 16) * 2];
static double audio_frac = 0.0;

static char last_error[512];

/* core options read while running (see update_options()) */
static int opt_analog = 1;
static int opt_rumble = 1;
static int opt_gamma = 0;                  /* OpenBOR scale, -255..255, 0 = off */
static uint8_t gamma_lut[256];
static uint32_t *gamma_buffer = NULL;
static size_t gamma_buffer_size = 0;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void core_log(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_cb)
        log_cb((enum retro_log_level)level, "[OpenBOR] %s", buf);
    else
        fprintf(stderr, "[OpenBOR] %s", buf);
}

static void show_message(const char *msg, unsigned frames)
{
    struct retro_message rmsg;
    if (!environ_cb) return;
    rmsg.msg = msg;
    rmsg.frames = frames;
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rmsg);
}

static void advance_virtual_clock(void)
{
    frame_count++;
    virtual_usec = (uint64_t)(frame_count * 1000000.0 / FPS);
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

/* ------------------------------------------------------------------------ */
/* Host services (called on the engine coroutine)                           */
/* ------------------------------------------------------------------------ */

static void engine_resumed(void)
{
    if (unloading)
        engine->shutdown(); /* normally never returns */
}

static void host_yield_frame(void)
{
    if (unloading || engine_state != ENGINE_RUNNING)
        return;
    frame_ready = 1;
    co_switch(main_thread);
    engine_resumed();
}

static void host_yield_idle(void)
{
    if (unloading || engine_state != ENGINE_RUNNING)
    {
        advance_virtual_clock(); /* cannot hand control back: let time pass */
        return;
    }
    co_switch(main_thread);
    engine_resumed();
}

static int host_unloading(void)
{
    return unloading;
}

static void host_shutdown_message(int status, const char *msg)
{
    if (status == 0 || !msg)
        return;
    snprintf(last_error, sizeof(last_error), "%s", msg);
    core_log(RETRO_LOG_ERROR, "[%s] %s\n", engine->id, msg);
    if (!loading)
        show_message(last_error, 600);
}

static unsigned host_realtime_ms(void)
{
    if (perf_get_time_usec)
        return (unsigned)(perf_get_time_usec() / 1000);
#if defined(CLOCK_MONOTONIC)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
#else
    return (unsigned)(clock() * 1000 / CLOCKS_PER_SEC);
#endif
}

static uint64_t host_virtual_usec(void)
{
    return virtual_usec;
}

void retro_get_system_av_info(struct retro_system_av_info *info);

static void host_video_mode_changed(int width, int height)
{
    av_width = width;
    av_height = height;

    if (!mode_known || loading)
    {
        /* reported by retro_get_system_av_info() after loading */
        mode_known = 1;
        if (width > av_max_width) av_max_width = width;
        if (height > av_max_height) av_max_height = height;
        return;
    }

    if (width > av_max_width || height > av_max_height)
    {
        struct retro_system_av_info info;
        if (width > av_max_width) av_max_width = width;
        if (height > av_max_height) av_max_height = height;
        retro_get_system_av_info(&info);
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
    }
    else
    {
        struct retro_game_geometry geom;
        geom.base_width = width;
        geom.base_height = height;
        geom.max_width = av_max_width;
        geom.max_height = av_max_height;
        geom.aspect_ratio = (float)width / (float)height;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
    }
}

static void host_set_rumble(int port, uint16_t strength)
{
    if (!have_rumble) return;
    if (!opt_rumble) strength = 0;
    rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, strength);
    rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, strength);
}

static void host_log(int level, const char *msg)
{
    core_log(level, "[%s] %s", engine ? engine->id : "-", msg);
}

static void host_engine_exit(int status)
{
    engine_exit_status = status;
    engine_state = ENGINE_EXITED;
    for (;;)
        co_switch(main_thread);
}

static const ob_host host = {
    OB_API_VERSION,
    host_yield_frame,
    host_yield_idle,
    host_unloading,
    host_shutdown_message,
    host_realtime_ms,
    host_virtual_usec,
    host_video_mode_changed,
    host_set_rumble,
    host_log,
    host_engine_exit,
};

static void engine_entry(void)
{
    engine->main();
    host_engine_exit(0);
}

/* ------------------------------------------------------------------------ */
/* Core options                                                             */
/* ------------------------------------------------------------------------ */

static int engine_by_name(const char *name)
{
    int i;
    for (i = 0; i < NUM_ENGINES; i++)
        if (!strcasecmp(engines[i].name, name)) return i;
    return -1;
}

/* engine index forced in the core options, -1 for auto */
static int option_engine(void)
{
    struct retro_variable var = { "openbor_engine", NULL };
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return engine_by_name(var.value);
    return -1;
}

static const char *get_option(const char *key)
{
    struct retro_variable var;
    var.key = key;
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        return var.value;
    return NULL;
}

/* OpenBOR's own gamma curve (palette.c), -255..255 */
static void build_gamma_lut(int g)
{
    int v;
    for (v = 0; v < 256; v++)
    {
        long r = g <= 0 ? ((long)v * (65025 + (255 - v) * g)) / 65025
                        : 255 - (((long)(255 - v) * (65025 + v * -g)) / 65025);
        gamma_lut[v] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    }
}

/* Options that apply immediately (the engine version applies on load). */
static void update_options(void)
{
    const char *v;
    int gamma;

    v = get_option("openbor_analog");
    opt_analog = !v || strcmp(v, "disabled") != 0;

    v = get_option("openbor_rumble");
    if (v)
    {
        int enabled = strcmp(v, "disabled") != 0;
        if (opt_rumble && !enabled && have_rumble)
        {
            int port;
            for (port = 0; port < 4; port++)
            {
                rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
                rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, 0);
            }
        }
        opt_rumble = enabled;
    }

    v = get_option("openbor_gamma");
    gamma = v ? atoi(v) : 0;
    if (gamma < -255) gamma = -255;
    if (gamma > 255) gamma = 255;
    if (gamma != opt_gamma)
    {
        opt_gamma = gamma;
        build_gamma_lut(gamma);
    }
}

/* Applies the gamma option to a frame; returns the buffer to present. */
static const uint32_t *apply_gamma(const uint32_t *fb, int w, int h)
{
    size_t i, n = (size_t)w * h;

    if (!opt_gamma || !fb || n == 0)
        return fb;
    if (n > gamma_buffer_size)
    {
        uint32_t *b = (uint32_t *)realloc(gamma_buffer, n * sizeof(uint32_t));
        if (!b)
            return fb;
        gamma_buffer = b;
        gamma_buffer_size = n;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t p = fb[i];
        gamma_buffer[i] = ((uint32_t)gamma_lut[(p >> 16) & 0xFF] << 16) |
                          ((uint32_t)gamma_lut[(p >> 8) & 0xFF] << 8) |
                          gamma_lut[p & 0xFF];
    }
    return gamma_buffer;
}

static void set_core_options(void)
{
    static char engine_values[256];
    static struct retro_variable vars[5];
    int i;

    strcpy(engine_values, "Engine version (applies on next load); auto");
    for (i = NUM_ENGINES - 1; i >= 0; i--)
    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "|%s", engines[i].name);
        strcat(engine_values, tmp);
    }
    vars[0].key = "openbor_engine";
    vars[0].value = engine_values;
    vars[1].key = "openbor_analog";
    vars[1].value = "Analog stick movement (left stick moves like the D-Pad); enabled|disabled";
    vars[2].key = "openbor_rumble";
    vars[2].value = "Rumble; enabled|disabled";
    vars[3].key = "openbor_gamma";
    vars[3].value = "Gamma (OpenBOR scale, 0 = off); 0|32|64|96|128|160|192|-192|-160|-128|-96|-64|-32";
    vars[4].key = NULL;
    vars[4].value = NULL;
    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

/* ------------------------------------------------------------------------ */
/* libretro API                                                             */
/* ------------------------------------------------------------------------ */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    static const struct retro_controller_description port_desc[] = {
        { "RetroPad", RETRO_DEVICE_JOYPAD },
    };
    static const struct retro_controller_info ports[] = {
        { port_desc, 1 }, { port_desc, 1 }, { port_desc, 1 }, { port_desc, 1 },
        { NULL, 0 },
    };
    bool no_content = false;

    environ_cb = cb;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
    set_core_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_init(void)
{
    struct retro_log_callback logging;
    struct retro_perf_callback perf;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf))
        perf_get_time_usec = perf.get_time_usec;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
        can_dupe = false;
    have_rumble = environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) ? 1 : 0;
    have_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

RETRO_API void retro_deinit(void)
{
    log_cb = NULL;
    perf_get_time_usec = NULL;
    free(gamma_buffer);
    gamma_buffer = NULL;
    gamma_buffer_size = 0;
    have_rumble = 0;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "OpenBOR";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
    info->library_version  = "multi-engine" GIT_VERSION;
    info->valid_extensions = "pak";
    info->need_fullpath    = true;
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = av_width;
    info->geometry.base_height  = av_height;
    info->geometry.max_width    = av_max_width;
    info->geometry.max_height   = av_max_height;
    info->geometry.aspect_ratio = (float)av_width / (float)av_height;
    info->timing.fps            = FPS;
    info->timing.sample_rate    = AUDIO_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

RETRO_API void retro_reset(void)
{
    /* OpenBOR keeps a lot of global state that cannot be reinitialised
     * without restarting the process; reset is not supported. */
    show_message("OpenBOR: reset is not supported, reload the content instead", 180);
}

static void set_input_descriptors(void)
{
#define DESC(p) \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Attack" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Jump" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Special" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Attack 2" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Attack 3" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Attack 4" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Menu / Back (Esc)" }, \
    { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Screenshot" }, \
    { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Move (analog, if enabled)" }, \
    { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move (analog, if enabled)" },
    struct retro_input_descriptor desc[] = {
        DESC(0) DESC(1) DESC(2) DESC(3)
        { 0, 0, 0, 0, NULL }
    };
#undef DESC
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

/* root_dir: "<save dir>/OpenBOR/", paks_dir: directory of the content */
static int compute_paths(const char *content_path, char *root_dir, char *paks_dir)
{
    const char *base = NULL;
    char *slash;

    snprintf(paks_dir, PATH_LEN, "%s", content_path);
    slash = strrchr(paks_dir, '/');
#ifdef _WIN32
    {
        char *bslash = strrchr(paks_dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
    }
#endif
    if (slash) *slash = 0;
    else strcpy(paks_dir, ".");

    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base) || !base || !base[0])
        if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &base) || !base || !base[0])
            base = paks_dir;

    snprintf(root_dir, PATH_LEN, "%s/OpenBOR/", base);
    make_dir(root_dir);
    return 1;
}

/* The engine to use for build `id`: its WebM variant when the pak has
 * videos, the plain one otherwise (or whichever exists). */
static int engine_for_build(int id, int want_webm)
{
    int i, found = -1;
    for (i = 0; i < NUM_ENGINES; i++)
        if (engines[i].id == id)
        {
            if (engines[i].webm == want_webm) return i;
            if (found < 0) found = i;
        }
    return found;
}

/* Starts engine `idx` and runs it until it has finished loading the game.
 * Returns 1 on success, 0 if the engine stopped (error message in last_error). */
static int start_engine(int idx, const char *pak, const char *root_dir, const char *paks_dir)
{
    static char engine_root[PATH_LEN];
    unsigned t0;

    engine = engines[idx].get();
    if (engine->api_version != OB_API_VERSION)
    {
        core_log(RETRO_LOG_ERROR, "Engine %s: interface mismatch.\n", engines[idx].name);
        return 0;
    }
    engine_used[idx] = 1;
    engine->set_host(&host);

    /* Each engine version gets its own data directory: the settings, high
     * score and save game files are raw structures whose layout differs
     * between versions (the same for a build and its WebM variant). */
    snprintf(engine_root, sizeof(engine_root), "%s%d/", root_dir, engines[idx].id);
    make_dir(engine_root);
    if (!engine->set_paths(pak, engine_root, paks_dir))
    {
        snprintf(last_error, sizeof(last_error), "Path too long for the OpenBOR engine: %s", pak);
        return 0;
    }

    core_log(RETRO_LOG_INFO, "Starting engine %s (%s)\n", engine->id, engine->name);

    last_error[0] = 0;
    mode_known = 0;
    av_width = 320; av_height = 240;
    av_max_width = 320; av_max_height = 240;
    frame_count = 0;
    virtual_usec = 0;
    engine_state = ENGINE_RUNNING;
    loading = 1;

    engine_thread = co_create(ENGINE_STACK, engine_entry);
    if (!engine_thread)
    {
        snprintf(last_error, sizeof(last_error), "Unable to create the engine thread");
        engine_state = ENGINE_NONE;
        loading = 0;
        return 0;
    }

    /* Run startup() to completion here so that compatibility errors (unknown
     * commands, script errors...) are caught while another engine can still
     * be tried. Loading-screen frames are not presented. */
    t0 = host_realtime_ms();
    while (engine_state == ENGINE_RUNNING && !engine->startup_done())
    {
        frame_ready = 0;
        co_switch(engine_thread);
        if (host_realtime_ms() - t0 > 300000)
        {
            snprintf(last_error, sizeof(last_error), "The game took too long to load");
            break;
        }
    }
    loading = 0;

    if (engine_state != ENGINE_RUNNING || !engine->startup_done())
    {
        if (!last_error[0])
            snprintf(last_error, sizeof(last_error), "engine stopped while loading (status %d)", engine_exit_status);
        if (engine_state == ENGINE_RUNNING)
        {
            /* timed out: make it release what it can */
            unloading = 1;
            co_switch(engine_thread);
            unloading = 0;
        }
        co_delete(engine_thread);
        engine_thread = NULL;
        engine->video_free();
        engine_state = ENGINE_NONE;
        return 0;
    }
    return 1;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    static char root_dir[PATH_LEN], paks_dir[PATH_LEN];
    int order[16], cand[16], ncand = 0, n = 0, i, forced, first = -1;
    ob_detect_result det;
    char msg[600], tag[32];

    if (!game || !game->path || NUM_ENGINES == 0)
        return false;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        core_log(RETRO_LOG_ERROR, "XRGB8888 is not supported by the frontend.\n");
        return false;
    }

    compute_paths(game->path, root_dir, paks_dir);
    set_input_descriptors();
    update_options();
    main_thread = co_active();
    unloading = 0;
    shutdown_requested = 0;

    /* 1. "Engine version" core option, 2. engine tag in the pak's file name
     * ("Name [ob8025_webm].pak", see pak_tag.h): the engine is used as is.
     * 3. Auto-detection, then the other engines as fallback: newer ones
     * first, then older ones. */
    forced = option_engine();
    if (forced >= 0)
        core_log(RETRO_LOG_INFO, "Engine %s selected in the core options.\n", engines[forced].name);
    else if (ob_pak_find_tag(game->path, tag, sizeof(tag), NULL))
    {
        forced = engine_by_name(tag);
        if (forced >= 0)
            core_log(RETRO_LOG_INFO, "Engine %s selected by the file name tag.\n", engines[forced].name);
        else
            core_log(RETRO_LOG_WARN, "Unknown engine '%s' in the file name tag, detecting.\n", tag);
    }

    if (forced >= 0)
        order[n++] = forced;
    else
    {
        int ids[16], nids = 0;
        /* one engine per upstream build, oldest first */
        for (i = 0; i < NUM_ENGINES; i++)
            if (!nids || ids[nids - 1] != engines[i].id)
                ids[nids++] = engines[i].id;
        ob_detect_version(game->path, ids, nids, &det);
        for (ncand = 0; ncand < nids; ncand++)
            cand[ncand] = engine_for_build(ids[ncand], det.webm_files > 0 || det.sab_calls > 0);
        for (i = 0; i < ncand; i++)
            if (ids[i] == det.version) first = i;
        if (first < 0) first = ncand - 1;
        core_log(RETRO_LOG_INFO, "Auto-detected engine %s (%s%s).\n", engines[cand[first]].name, det.reason,
                 engines[cand[first]].webm ? (det.sab_calls > 0 ? "; SAB extensions" : "; the pak has WebM videos") : "");
        order[n++] = cand[first];
        for (i = first + 1; i < ncand; i++) order[n++] = cand[i];
        for (i = first - 1; i >= 0; i--) order[n++] = cand[i];
    }

    msg[0] = 0;
    for (i = 0; i < n; i++)
    {
        int idx = order[i];
        if (engine_used[idx])
        {
            core_log(RETRO_LOG_WARN, "Engine %s was already used in this session; restart the core to use it again.\n", engines[idx].name);
            continue;
        }
        if (start_engine(idx, game->path, root_dir, paks_dir))
        {
            if (i > 0)
            {
                snprintf(msg, sizeof(msg), "OpenBOR: using engine %s", engine->id);
                show_message(msg, 240);
            }
            return true;
        }
        core_log(RETRO_LOG_WARN, "Engine %s could not load the game: %s\n", engines[idx].name, last_error);
        snprintf(msg, sizeof(msg), "OpenBOR %s: %s", engines[idx].name, last_error);
    }

    show_message(msg[0] ? msg : "OpenBOR: no engine could load this game", 600);
    engine = NULL;
    return false;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    if (engine_thread)
    {
        if (engine_state == ENGINE_RUNNING)
        {
            /* Let the engine save its settings and free everything. */
            unloading = 1;
            co_switch(engine_thread);
        }
        co_delete(engine_thread);
        engine_thread = NULL;
    }
    engine_state = ENGINE_EXITED;
    if (engine)
        engine->video_free();
}

static void poll_input(void)
{
    int port;
    input_poll_cb();

    for (port = 0; port < 4; port++)
    {
        uint32_t buttons = 0;
        unsigned id;

        if (have_bitmasks)
            buttons = (uint16_t)input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        else
            for (id = 0; id < 16; id++)
                if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id))
                    buttons |= 1u << id;

        if (opt_analog)
        {
            const int threshold = 0x4000;
            int ax = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
            int ay = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
            if (ax <= -threshold) buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
            if (ax >=  threshold) buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
            if (ay <= -threshold) buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
            if (ay >=  threshold) buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        }

        engine->set_pad(port, buttons);
    }
}

static void output_audio(void)
{
    size_t frames;

    audio_frac += (double)AUDIO_RATE / FPS;
    frames = (size_t)audio_frac;
    audio_frac -= frames;
    if (frames > sizeof(audio_buffer) / (2 * sizeof(int16_t)))
        frames = sizeof(audio_buffer) / (2 * sizeof(int16_t));

    if (engine_state != ENGINE_RUNNING || !engine->mix_audio(audio_buffer, (int)frames))
        memset(audio_buffer, 0, frames * 2 * sizeof(int16_t));

    audio_batch_cb(audio_buffer, frames);
}

RETRO_API void retro_run(void)
{
    const uint32_t *fb = NULL;
    int fbw = 0, fbh = 0;

    {
        bool updated = false;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
            update_options();
    }

    if (engine_state == ENGINE_RUNNING)
    {
        unsigned before = (unsigned)(virtual_usec / 1000);

        poll_input();
        advance_virtual_clock();
        engine->tick_rumble((unsigned)(virtual_usec / 1000) - before);

        frame_ready = 0;
        co_switch(engine_thread);
    }
    else
    {
        input_poll_cb();
        frame_ready = 0;
    }

    if (engine_state == ENGINE_EXITED && !shutdown_requested)
    {
        shutdown_requested = 1;
        core_log(RETRO_LOG_INFO, "Engine exited (status %d).\n", engine_exit_status);
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }

    if (engine)
    {
        fb = engine->framebuffer(&fbw, &fbh);
        fb = apply_gamma(fb, fbw, fbh);
    }

    if (frame_ready && fb)
        video_cb(fb, fbw, fbh, fbw * sizeof(uint32_t));
    else if (can_dupe)
        video_cb(NULL, fbw ? fbw : av_width, fbh ? fbh : av_height, 0);
    else if (fb)
        video_cb(fb, fbw, fbh, fbw * sizeof(uint32_t));

    output_audio();
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
