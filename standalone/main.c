/*
 * OpenBOR multi-engine standalone: the libretro core linked into a small
 * SDL2 host (window, audio, keyboard and game controllers), no frontend
 * needed. The host answers the core's environment calls itself, so the
 * game runs exactly as in the core; the engine version is chosen on the
 * command line (core option "openbor_engine").
 *
 *   openbor [options] <game.pak>
 *
 *   --engine <name>   auto (default: the pak's file-name tag, then
 *                     detection) or one of --list-engines
 *   --list-engines    print the engine versions built in
 *   --fullscreen      start in fullscreen (F11 / Alt+Enter toggle)
 *   --scale <n>       window size: n x the game's resolution (default 2)
 *   --gamma <n>       OpenBOR gamma, -255 ... 255 (default 0)
 *   --no-analog       the left stick does not move like the D-pad
 *   --no-rumble       no controller rumble
 *   --save-dir <dir>  where OpenBOR/ (settings, saves, logs) is written
 *                     (default: next to the executable)
 *
 * Keys (player 1, RetroArch's defaults): arrows = D-pad, Z = B, X = A,
 * A = Y, S = X, Q = L, W = R, Enter = Start, Right Shift = Select;
 * Esc quits. Game controllers (SDL GameController mapping) are players
 * 1-4 in connection order; with a controller on player 1 the keyboard still
 * drives player 1.
 *
 * Build: make standalone (see the Makefile).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "libretro.h"

#define MAX_PORTS 4

static const char *opt_engine = "auto";
static int opt_fullscreen = 0;
static int opt_scale = 2;
static char opt_gamma[16] = "0";
static int opt_analog = 1;
static int opt_rumble = 1;
static char save_dir[1024];

static char engine_values[512];        /* "a|b|c" of the core's openbor_engine option */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h;
static SDL_AudioDeviceID audio_dev;
static int audio_rate = 44100;
static double core_fps = 60.0;
static int quit;

static SDL_GameController *pads[MAX_PORTS];
static uint16_t key_buttons;            /* player 1 from the keyboard */

/* ------------------------------------------------------------------ log */

static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;

    if (level == RETRO_LOG_DEBUG)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------------------- rumble */

static bool RETRO_CALLCONV rumble_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    static uint16_t strong[MAX_PORTS], weak[MAX_PORTS];

    if (port >= MAX_PORTS || !pads[port] || !opt_rumble)
        return false;
    if (effect == RETRO_RUMBLE_STRONG)
        strong[port] = strength;
    else
        weak[port] = strength;
    return SDL_GameControllerRumble(pads[port], strong[port], weak[port], 5000) == 0;
}

/* ---------------------------------------------------------- environment */

static void set_geometry(unsigned w, unsigned h)
{
    if ((int)w == tex_w && (int)h == tex_h && texture)
        return;
    if (texture)
        SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
    tex_w = (int)w;
    tex_h = (int)h;
    SDL_RenderSetLogicalSize(renderer, (int)w, (int)h);
    if (!opt_fullscreen)
        SDL_SetWindowSize(window, (int)w * opt_scale, (int)h * opt_scale);
}

static bool RETRO_CALLCONV environment_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    {
        const struct retro_variable *v;
        for (v = (const struct retro_variable *)data; v->key; v++)
            if (!strcmp(v->key, "openbor_engine"))
            {
                const char *p = strchr(v->value, ';');
                snprintf(engine_values, sizeof(engine_values), "%s", p ? p + 2 : v->value);
            }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *v = (struct retro_variable *)data;
        if (!strcmp(v->key, "openbor_engine"))
            v->value = opt_engine;
        else if (!strcmp(v->key, "openbor_analog"))
            v->value = opt_analog ? "enabled" : "disabled";
        else if (!strcmp(v->key, "openbor_rumble"))
            v->value = opt_rumble ? "enabled" : "disabled";
        else if (!strcmp(v->key, "openbor_gamma"))
            v->value = opt_gamma;
        else
            return false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_cb;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        const struct retro_system_av_info *av = (const struct retro_system_av_info *)data;
        core_fps = av->timing.fps > 0 ? av->timing.fps : 60.0;
        set_geometry(av->geometry.base_width, av->geometry.base_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        const struct retro_game_geometry *g = (const struct retro_game_geometry *)data;
        set_geometry(g->base_width, g->base_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        fprintf(stderr, "%s\n", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        quit = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- video */

static void RETRO_CALLCONV video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data)
    {
        set_geometry(w, h);
        SDL_UpdateTexture(texture, NULL, data, (int)pitch);
    }
    SDL_RenderClear(renderer);
    if (texture)
        SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* ---------------------------------------------------------------- audio */

static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
    if (audio_dev)
        SDL_QueueAudio(audio_dev, data, (Uint32)(frames * 4));
    return frames;
}

static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    audio_batch_cb(s, 1);
}

/* ---------------------------------------------------------------- input */

static void RETRO_CALLCONV input_poll_cb(void) {}

static int key_id(SDL_Keycode k)
{
    switch (k)
    {
    case SDLK_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
    case SDLK_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
    case SDLK_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
    case SDLK_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    case SDLK_z: return RETRO_DEVICE_ID_JOYPAD_B;
    case SDLK_x: return RETRO_DEVICE_ID_JOYPAD_A;
    case SDLK_a: return RETRO_DEVICE_ID_JOYPAD_Y;
    case SDLK_s: return RETRO_DEVICE_ID_JOYPAD_X;
    case SDLK_q: return RETRO_DEVICE_ID_JOYPAD_L;
    case SDLK_w: return RETRO_DEVICE_ID_JOYPAD_R;
    case SDLK_RETURN: return RETRO_DEVICE_ID_JOYPAD_START;
    case SDLK_RSHIFT: return RETRO_DEVICE_ID_JOYPAD_SELECT;
    default: return -1;
    }
}

static int pad_button(SDL_GameController *c, unsigned id)
{
    static const int map[16] = {
        SDL_CONTROLLER_BUTTON_A,            /* B (bottom) */
        SDL_CONTROLLER_BUTTON_X,            /* Y (left) */
        SDL_CONTROLLER_BUTTON_BACK,         /* SELECT */
        SDL_CONTROLLER_BUTTON_START,        /* START */
        SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_B,            /* A (right) */
        SDL_CONTROLLER_BUTTON_Y,            /* X (top) */
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        -1, -1,                             /* L2, R2: triggers */
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    };

    if (id == RETRO_DEVICE_ID_JOYPAD_L2)
        return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
    if (id == RETRO_DEVICE_ID_JOYPAD_R2)
        return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
    return map[id] >= 0 && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)map[id]);
}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    SDL_GameController *c = port < MAX_PORTS ? pads[port] : NULL;

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        {
            int16_t m = 0;
            unsigned i;
            for (i = 0; i < 16; i++)
                if (input_state_cb(port, device, index, i))
                    m |= (int16_t)(1 << i);
            return m;
        }
        if (id >= 16)
            return 0;
        if (port == 0 && (key_buttons & (1 << id)))
            return 1;
        return c ? (int16_t)pad_button(c, id) : 0;
    }
    if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT && c)
        return SDL_GameControllerGetAxis(c, id == RETRO_DEVICE_ID_ANALOG_X ? SDL_CONTROLLER_AXIS_LEFTX
                                                                            : SDL_CONTROLLER_AXIS_LEFTY);
    return 0;
}

static void open_pads(void)
{
    int i, n = 0;

    for (i = 0; i < MAX_PORTS; i++)
        if (pads[i])
        {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
    for (i = 0; i < SDL_NumJoysticks() && n < MAX_PORTS; i++)
        if (SDL_IsGameController(i) && (pads[n] = SDL_GameControllerOpen(i)) != NULL)
            n++;
}

static void toggle_fullscreen(void)
{
    opt_fullscreen = !opt_fullscreen;
    SDL_SetWindowFullscreen(window, opt_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void handle_events(void)
{
    SDL_Event e;
    int id;

    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_QUIT:
            quit = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                quit = 1;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.sym == SDLK_F11 ||
                     (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))))
                toggle_fullscreen();
            else if ((id = key_id(e.key.keysym.sym)) >= 0)
            {
                if (e.type == SDL_KEYDOWN)
                    key_buttons |= (uint16_t)(1 << id);
                else
                    key_buttons &= (uint16_t)~(1 << id);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            open_pads();
            break;
        default:
            break;
        }
    }
}

/* ----------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "OpenBOR multi-engine standalone\n\n"
            "usage: %s [options] <game.pak>\n\n"
            "  --engine <name>   auto (default) or a version of --list-engines\n"
            "  --list-engines    print the engine versions built in\n"
            "  --fullscreen      start in fullscreen (F11 / Alt+Enter toggle)\n"
            "  --scale <n>       window size: n x the game's resolution (default 2)\n"
            "  --gamma <n>       OpenBOR gamma, -255 ... 255 (default 0)\n"
            "  --no-analog       the left stick does not move like the D-pad\n"
            "  --no-rumble       no controller rumble\n"
            "  --save-dir <dir>  where OpenBOR/ is written (default: next to the program)\n\n"
            "keys: arrows, Z X A S Q W, Enter = Start, Right Shift = Select, Esc = quit\n",
            argv0);
}

static void default_save_dir(void)
{
    char *base = SDL_GetBasePath();

    if (base)
    {
        size_t n;
        snprintf(save_dir, sizeof(save_dir), "%s", base);
        SDL_free(base);
        n = strlen(save_dir);
        /* no trailing separator: the core appends "/OpenBOR/" */
        while (n > 1 && (save_dir[n - 1] == '/' || save_dir[n - 1] == '\\'))
            save_dir[--n] = 0;
    }
    else
        strcpy(save_dir, ".");
}

/* the engine names from the core's option (set_environment declares them) */
static int valid_engine(const char *name)
{
    char buf[sizeof(engine_values)], *tok;

    snprintf(buf, sizeof(buf), "%s", engine_values);
    for (tok = strtok(buf, "|"); tok; tok = strtok(NULL, "|"))
        if (!strcmp(tok, name))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *pak = NULL;
    struct retro_game_info game;
    struct retro_system_av_info av;
    Uint64 freq, next;
    int i, list = 0;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            opt_engine = argv[++i];
        else if (!strcmp(argv[i], "--list-engines"))
            list = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            opt_fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            opt_scale = atoi(argv[++i]) > 0 ? atoi(argv[i]) : 1;
        else if (!strcmp(argv[i], "--gamma") && i + 1 < argc)
            snprintf(opt_gamma, sizeof(opt_gamma), "%d", atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-analog"))
            opt_analog = 0;
        else if (!strcmp(argv[i], "--no-rumble"))
            opt_rumble = 0;
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc)
            snprintf(save_dir, sizeof(save_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-' && argv[i][1])
        {
            fprintf(stderr, "unknown option %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        else
            pak = argv[i];
    }

    retro_set_environment(environment_cb);
    if (list)
    {
        char *tok;
        printf("auto\n");
        for (tok = strtok(engine_values, "|"); tok; tok = strtok(NULL, "|"))
            if (strcmp(tok, "auto"))
                printf("%s\n", tok);
        return 0;
    }
    if (!pak)
    {
        usage(argv[0]);
        return 1;
    }
    if (!valid_engine(opt_engine))
    {
        fprintf(stderr, "unknown engine '%s' (see --list-engines)\n", opt_engine);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
    {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!save_dir[0])
        default_save_dir();

    window = SDL_CreateWindow("OpenBOR", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                              SDL_WINDOW_RESIZABLE | (opt_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!renderer && window)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
    {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_ShowCursor(opt_fullscreen ? SDL_DISABLE : SDL_ENABLE);
    open_pads();

    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    memset(&game, 0, sizeof(game));
    game.path = pak;
    if (!retro_load_game(&game))
    {
        fprintf(stderr, "could not load %s\n", pak);
        retro_deinit();
        SDL_Quit();
        return 1;
    }
    retro_get_system_av_info(&av);
    core_fps = av.timing.fps > 0 ? av.timing.fps : 60.0;
    audio_rate = av.timing.sample_rate > 0 ? (int)(av.timing.sample_rate + 0.5) : 44100;
    set_geometry(av.geometry.base_width, av.geometry.base_height);
    {
        char title[300];
        const char *name = strrchr(pak, '/');
        const char *bs = strrchr(pak, '\\');
        if (bs && (!name || bs > name))
            name = bs;
        snprintf(title, sizeof(title), "OpenBOR - %s", name ? name + 1 : pak);
        SDL_SetWindowTitle(window, title);
    }

    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = audio_rate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev)
            SDL_PauseAudioDevice(audio_dev, 0);
        else
            fprintf(stderr, "no audio: %s\n", SDL_GetError());
    }

    /* the core's frame rate from the performance counter; the audio queue is
       kept short (a late frame skips the wait, a full queue waits) */
    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    while (!quit)
    {
        Uint64 now;

        handle_events();
        retro_run();
        next += (Uint64)(freq / core_fps);
        now = SDL_GetPerformanceCounter();
        if (now < next)
        {
            Uint32 ms = (Uint32)((next - now) * 1000 / freq);
            if (ms > 0)
                SDL_Delay(ms);
        }
        else if (now - next > freq / 4)
            next = now;                 /* far behind: no catching up */
        if (audio_dev)
            while (SDL_GetQueuedAudioSize(audio_dev) > (Uint32)(audio_rate / 5 * 4) && !quit)
                SDL_Delay(1);
    }

    retro_unload_game();
    retro_deinit();
    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);
    for (i = 0; i < MAX_PORTS; i++)
        if (pads[i])
            SDL_GameControllerClose(pads[i]);
    if (texture)
        SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
