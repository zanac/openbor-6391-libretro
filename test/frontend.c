/*
 * Minimal headless libretro frontend used to smoke-test the OpenBOR core.
 *
 *   frontend <core.so> <game.pak> <workdir> <frames> [events...]
 *
 * events:  F:PORT:BUTTON[:HOLD]   press BUTTON on PORT at frame F for HOLD
 *                                 frames (default 4). BUTTON is one of
 *                                 B Y SELECT START UP DOWN LEFT RIGHT A X L R
 *                                 L2 R2 L3 R3
 *          dump=F1,F2,...          write <workdir>/frame_<F>.ppm
 *          unload=F                stop at frame F and unload
 *
 * VIDEO=<file.mp4> (environment) pipes every frame to ffmpeg (H.264, the
 * core's frame rate; a duplicated frame repeats the previous one); mux
 * audio.wav afterwards.
 *
 * Writes <workdir>/audio.wav and prints a summary on stdout.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

#include "libretro.h"

#define MAX_EVENTS 8192
#define MAX_DUMPS  64

typedef struct { long frame; int port; int id; int hold; } event_t;

static event_t events[MAX_EVENTS];
static int n_events;
static long dumps[MAX_DUMPS];
static int n_dumps;
static long frame_no;
static const char *workdir;
static char save_dir[1024];

static int fmt_xrgb = 0;
static int shutdown_flag = 0;
static unsigned geom_w, geom_h;
static long frames_presented, frames_duped;
static uint32_t last_hash;
static long distinct_frames;

static FILE *wav;
static long audio_frames;
static double audio_energy;

static const char *btn_names[16] = {
    "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
    "A", "X", "L", "R", "L2", "R2", "L3", "R3"
};

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    static const char *lv[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    printf("[core %s] ", lv[level & 3]);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static retro_time_t perf_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool rumble_set(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    if (effect == RETRO_RUMBLE_STRONG)
        printf("[rumble] frame %ld port %u strength %u\n", frame_no, port, strength);
    return true;
}

static bool environ_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        fmt_xrgb = *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
        return fmt_xrgb;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_set;
        return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        memset(data, 0, sizeof(struct retro_perf_callback));
        ((struct retro_perf_callback *)data)->get_time_usec = perf_usec;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        const struct retro_game_geometry *g = data;
        printf("[env] frame %ld SET_GEOMETRY %ux%u (max %ux%u)\n", frame_no, g->base_width, g->base_height, g->max_width, g->max_height);
        geom_w = g->base_width; geom_h = g->base_height;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        const struct retro_system_av_info *i = data;
        printf("[env] frame %ld SET_SYSTEM_AV_INFO %ux%u\n", frame_no, i->geometry.base_width, i->geometry.base_height);
        geom_w = i->geometry.base_width; geom_h = i->geometry.base_height;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        printf("[env] frame %ld MESSAGE: %s\n", frame_no, ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        printf("[env] frame %ld SHUTDOWN\n", frame_no);
        shutdown_flag = 1;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* core options from the environment: openbor_engine -> OPENBOR_ENGINE */
        struct retro_variable *v = data;
        char name[128];
        size_t k;
        for (k = 0; v->key[k] && k < sizeof(name) - 1; k++)
            name[k] = (v->key[k] >= 'a' && v->key[k] <= 'z') ? v->key[k] - 32 : v->key[k];
        name[k] = 0;
        v->value = getenv(name);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    {
        const struct retro_variable *v = data;
        for (; v->key; v++) printf("[env] option %s = %s\n", v->key, v->value);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

static uint32_t hash_frame(const uint32_t *p, unsigned w, unsigned h, size_t pitch)
{
    uint32_t hsh = 2166136261u;
    unsigned x, y;
    for (y = 0; y < h; y++)
    {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)p + y * pitch);
        for (x = 0; x < w; x++)
            hsh = (hsh ^ row[x]) * 16777619u;
    }
    return hsh;
}

static void dump_ppm(const uint32_t *p, unsigned w, unsigned h, size_t pitch)
{
    char path[1200];
    FILE *f;
    unsigned x, y;
    snprintf(path, sizeof(path), "%s/frame_%05ld.ppm", workdir, frame_no);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++)
    {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)p + y * pitch);
        for (x = 0; x < w; x++)
        {
            unsigned char rgb[3] = { (row[x] >> 16) & 0xFF, (row[x] >> 8) & 0xFF, row[x] & 0xFF };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("[dump] %s (%ux%u)\n", path, w, h);
}

static FILE *video_pipe;
static uint8_t *video_last;
static unsigned video_w, video_h;
static double video_fps = 60.0;

/* VIDEO=<file>: one BGRA frame per retro_run() into ffmpeg. */
static void video_write(const void *data, unsigned w, unsigned h, size_t pitch)
{
    unsigned y;
    if (!getenv("VIDEO"))
        return;
    if (!video_pipe && data)
    {
        char cmd[1400];
        video_w = w;
        video_h = h;
        video_last = (uint8_t *)calloc((size_t)w * h, 4);
        snprintf(cmd, sizeof(cmd), "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s %ux%u -r %.4f -i - "
                 "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p '%s'", w, h, video_fps, getenv("VIDEO"));
        video_pipe = popen(cmd, "w");
    }
    if (!video_pipe)
        return;
    if (data && w == video_w && h == video_h)
        for (y = 0; y < h; y++)
            memcpy(video_last + (size_t)y * w * 4, (const uint8_t *)data + y * pitch, (size_t)w * 4);
    fwrite(video_last, 4, (size_t)video_w * video_h, video_pipe);
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    int i;
    video_write(data, w, h, pitch);
    if (!data)
    {
        frames_duped++;
        return;
    }
    frames_presented++;
    {
        uint32_t hsh = hash_frame(data, w, h, pitch);
        if (hsh != last_hash) distinct_frames++;
        last_hash = hsh;
    }
    for (i = 0; i < n_dumps; i++)
        if (dumps[i] == frame_no)
            dump_ppm(data, w, h, pitch);
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    size_t i;
    if (wav) fwrite(data, sizeof(int16_t) * 2, frames, wav);
    for (i = 0; i < frames * 2; i++)
        audio_energy += (double)data[i] * data[i];
    audio_frames += frames;
    return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    int i;
    int16_t mask = 0;
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    for (i = 0; i < n_events; i++)
    {
        if ((unsigned)events[i].port != port) continue;
        if (frame_no >= events[i].frame && frame_no < events[i].frame + events[i].hold)
            mask |= 1 << events[i].id;
    }
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return mask;
    return (mask >> id) & 1;
}

static void wav_header(FILE *f, long frames)
{
    uint32_t data_bytes = (uint32_t)(frames * 4), riff = 36 + data_bytes;
    uint32_t rate = 44100, byte_rate = 44100 * 4, fmt_len = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
}

#define SYM(name) do { *(void **)&p_##name = dlsym(h, #name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; } } while (0)

int main(int argc, char **argv)
{
    void *h;
    long frames, unload_at = -1;
    int i;
    struct retro_game_info game;
    struct retro_system_av_info av;
    char wavpath[1200];

    void (*p_retro_set_environment)(retro_environment_t);
    void (*p_retro_set_video_refresh)(retro_video_refresh_t);
    void (*p_retro_set_audio_sample)(retro_audio_sample_t);
    void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*p_retro_set_input_poll)(retro_input_poll_t);
    void (*p_retro_set_input_state)(retro_input_state_t);
    void (*p_retro_init)(void);
    void (*p_retro_deinit)(void);
    bool (*p_retro_load_game)(const struct retro_game_info *);
    void (*p_retro_unload_game)(void);
    void (*p_retro_run)(void);
    void (*p_retro_get_system_av_info)(struct retro_system_av_info *);
    void (*p_retro_get_system_info)(struct retro_system_info *);

    if (argc < 5)
    {
        fprintf(stderr, "usage: %s core.so game.pak workdir frames [events]\n", argv[0]);
        return 2;
    }
    workdir = argv[3];
    frames = atol(argv[4]);
    mkdir(workdir, 0777);
    snprintf(save_dir, sizeof(save_dir), "%s/save", workdir);
    mkdir(save_dir, 0777);

    for (i = 5; i < argc; i++)
    {
        if (!strncmp(argv[i], "dump=", 5))
        {
            char *s = argv[i] + 5;
            while (*s && n_dumps < MAX_DUMPS)
            {
                dumps[n_dumps++] = strtol(s, &s, 10);
                if (*s == ',') s++;
            }
        }
        else if (!strncmp(argv[i], "unload=", 7))
            unload_at = atol(argv[i] + 7);
        else if (n_events < MAX_EVENTS)
        {
            char btn[16] = "";
            event_t *e = &events[n_events];
            int k;
            e->hold = 4;
            if (sscanf(argv[i], "%ld:%d:%15[A-Z0-9]:%d", &e->frame, &e->port, btn, &e->hold) < 3)
            {
                fprintf(stderr, "bad event %s\n", argv[i]);
                return 2;
            }
            e->id = -1;
            for (k = 0; k < 16; k++)
                if (!strcmp(btn, btn_names[k])) e->id = k;
            if (e->id < 0) { fprintf(stderr, "bad button %s\n", btn); return 2; }
            n_events++;
        }
    }

    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_deinit); SYM(retro_load_game); SYM(retro_unload_game);
    SYM(retro_run); SYM(retro_get_system_av_info); SYM(retro_get_system_info);

    {
        struct retro_system_info si;
        p_retro_get_system_info(&si);
        printf("[info] %s %s (ext: %s, fullpath %d)\n", si.library_name, si.library_version, si.valid_extensions, si.need_fullpath);
    }

    p_retro_set_environment(environ_cb);
    p_retro_set_video_refresh(video_cb);
    p_retro_set_audio_sample(audio_cb);
    p_retro_set_audio_sample_batch(audio_batch_cb);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    p_retro_init();

    memset(&game, 0, sizeof(game));
    game.path = argv[2];
    if (!p_retro_load_game(&game))
    {
        printf("[result] retro_load_game FAILED\n");
        p_retro_deinit();
        return 1;
    }
    p_retro_get_system_av_info(&av);
    printf("[av] %ux%u max %ux%u aspect %.3f fps %.2f rate %.0f\n",
           av.geometry.base_width, av.geometry.base_height, av.geometry.max_width,
           av.geometry.max_height, av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

    video_fps = av.timing.fps > 0 ? av.timing.fps : 60.0;
    snprintf(wavpath, sizeof(wavpath), "%s/audio.wav", workdir);
    wav = fopen(wavpath, "wb");
    if (wav) wav_header(wav, 0);

    {
        double t0 = (double)perf_usec();
        /* REALTIME=1 paces retro_run() at the core's frame rate, like a real
         * frontend (needed for threaded work such as video decoding). */
        int realtime = getenv("REALTIME") && atoi(getenv("REALTIME"));
        long perflog = getenv("PERFLOG") ? atol(getenv("PERFLOG")) : 0;
        double tw = t0;
        for (frame_no = 0; frame_no < frames && !shutdown_flag; frame_no++)
        {
            if (unload_at >= 0 && frame_no == unload_at) break;
            p_retro_run();
            /* PERFLOG=n: the speed of each window of n frames (slowdowns
               that build up over a long run) */
            if (perflog > 0 && (frame_no + 1) % perflog == 0)
            {
                double now = (double)perf_usec();
                printf("[perf] frame %ld: %.1f fps\n", frame_no + 1, perflog * 1e6 / (now - tw));
                fflush(stdout);
                tw = now;
            }
            if (realtime)
            {
                retro_time_t due = (retro_time_t)t0 + (retro_time_t)((frame_no + 1) * 1e6 / av.timing.fps);
                retro_time_t now = perf_usec();
                if (due > now) usleep((useconds_t)(due - now));
            }
        }
        printf("[timing] %ld frames in %.2f s\n", frame_no, ((double)perf_usec() - t0) / 1e6);
    }

    printf("[result] frames run %ld, presented %ld, duped %ld, distinct %ld, shutdown %d\n",
           frame_no, frames_presented, frames_duped, distinct_frames, shutdown_flag);
    printf("[result] audio frames %ld (expected ~%ld), rms %.1f\n",
           audio_frames, (long)(frame_no * 735), audio_frames ? sqrt(audio_energy / (audio_frames * 2.0)) : 0.0);

    if (video_pipe)
        pclose(video_pipe);
    p_retro_unload_game();
    printf("[result] unload ok\n");
    p_retro_deinit();
    if (wav) { wav_header(wav, audio_frames); fclose(wav); }
    if (!getenv("NO_DLCLOSE")) dlclose(h);
    return 0;
}
