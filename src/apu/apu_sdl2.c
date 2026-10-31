/**
 * SDL2 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux: the APU frame
 * thread hands over one EP period (256 stereo frames) at a time, and the
 * device queue paces it (mcpx_apu_pace_step reads xa2_queued_samples).
 * SDL's push API (SDL_QueueAudio) is the queue; its device thread pulls one
 * device buffer per callback and pads a short pull with silence, which is
 * why the pacing marks keep at least one device buffer queued.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIODRIVER is already set: it drains the queue in real time without
 * a device, so pacing behaves as on a real device and nothing reaches the
 * speakers. SDL_AUDIODRIVER=coreaudio (or pulseaudio, pipewire)
 * with RECOMP_HEADLESS=1 is the way to hear a headless run.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

#if !defined(_WIN32)

#include <SDL.h>
#include <time.h>

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS      2
/* Device buffer size and count: the same defaults and knobs as the XAudio2
 * backend (512 x 4: 10.7 ms per buffer, 43 ms queued at the high mark). The
 * count is a pacing mark here, not a slot limit: SDL's queue is unbounded. */
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT    4
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        8

static SDL_AudioDeviceID g_dev;
static int       g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = SDLA_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static unsigned  g_underruns;
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static int       g_dry;
static int64_t   g_underrun_logged_ms;
static SDL_AudioSpec g_want;        /* what xa2_init asked for, to reopen with */
static unsigned  g_recoveries;
/* xa2_recover's helper thread: the device it opened, for the APU thread to
 * take (dev_take), and whether it is still at work. */
static SDL_atomic_t g_new_dev, g_recovering;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *e, *driver;

    if (g_initialized) return 1;

    g_buf_samples = env_int("RECOMP_AUDIO_BUF_SAMPLES", SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int("RECOMP_AUDIO_BUF_COUNT", SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    /* SDL installs SIGINT/SIGTERM handlers that only post SDL_QUIT, which
     * nothing reads in a headless run; keep the default handlers so Ctrl-C
     * still ends the process. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    /* Headless: no speakers unless the caller names a driver. Must be set
     * before the audio subsystem starts; 0 keeps an existing value. */
    e = getenv("RECOMP_HEADLESS");
    if (e && *e && *e != '0')
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
            return 0;
        }
        g_audio_opened = 1;
    }

    memset(&want, 0, sizeof(want));
    want.freq     = SDLA_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = SDLA_CHANNELS;
    want.samples  = (Uint16)g_buf_samples;
    want.callback = NULL;                       /* push: SDL_QueueAudio */

    /* No format changes allowed: the mixer's 48 kHz s16 stereo is what the
     * queue takes, and SDL converts for the device if it has to. */
    g_want = want;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_dev == 0) {
        fprintf(stderr, "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    if (have.samples > 0 && have.samples != g_buf_samples) {
        fprintf(stderr, "[SDL2] device buffer is %d samples (asked %d)\n",
                have.samples, g_buf_samples);
        g_buf_samples = have.samples;
        /* The queue marks and the pacer's stall window assume buffers no
         * larger than the env limit. A driver that grants more is still
         * fed, against marks sized for the limit, and may underrun. */
        if (g_buf_samples > SDLA_BUF_SAMPLES_MAX)
            g_buf_samples = SDLA_BUF_SAMPLES_MAX;
    }

    g_frames_written = 0;
    g_samples_queued = 0;
    g_dry = 0;
    g_initialized = 1;
    SDL_PauseAudioDevice(g_dev, 0);

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL2] audio initialized: driver %s (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE, g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: no device output]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_ClearQueuedAudio(g_dev);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL2] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Hand one EP period to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. Nothing is ever refused by depth: the queue is
 * unbounded and pacing by queue depth holds it at the marks. */
/* A device xa2_recover's helper opened replaces the stalled one here, on
 * the APU thread, the only thread that uses g_dev. The stalled device is
 * left as it is -- not closed, cleared or paused: on Android each of those
 * waits on its stuck audio thread (SDL_CloseAudioDevice joins it), which is
 * how the first version of this hung the game. It is lost; one a stall. */
static void dev_take(void)
{
    SDL_AudioDeviceID d = (SDL_AudioDeviceID)SDL_AtomicSet(&g_new_dev, 0);
    if (d) {
        fprintf(stderr, "[SDL2] audio: now on the new device %u (the stalled %u left alone)\n",
                (unsigned)d, (unsigned)g_dev);
        g_dev = d;
        g_dry = 0;
    }
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    dev_take();
    if (!g_initialized || !g_dev || num_samples <= 0) return 0;
    {   /* RECOMP_AUDIO_STALL_TEST=<s>: that many seconds in, the device stops
         * pulling (paused behind the backend's back) -- the stall the pacer
         * detects and xa2_recover answers, without a headset */
        static int64_t at = -1, t0;
        if (at < 0) {
            const char *e = getenv("RECOMP_AUDIO_STALL_TEST");
            at = e ? (int64_t)atoi(e) * 1000 : 0;
            t0 = now_ms();
        }
        if (at > 0 && now_ms() - t0 >= at) {
            at = 0;
            fprintf(stderr, "[SDL2] RECOMP_AUDIO_STALL_TEST: the device stops pulling\n");
            SDL_PauseAudioDevice(g_dev, 1);
        }
    }
    if (SDL_QueueAudio(g_dev, samples,
                       (Uint32)num_samples * SDLA_CHANNELS * sizeof(int16_t)) != 0) {
        fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread. The device holds
 * up to one more buffer in flight, which this does not count, the same way
 * XAudio2's BuffersQueued does not count the one playing. Also the underrun
 * detector: an empty queue after audio has started means the device's last
 * pull was short and padded with silence; counted once per dry spell and
 * reported at most once a second. A drain announced by xa2_expect_drain
 * (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    Uint32 bytes;

    dev_take();
    if (!g_initialized || !g_dev) return 0;
    bytes = SDL_GetQueuedAudioSize(g_dev);
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL2] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)(bytes / (SDLA_CHANNELS * sizeof(int16_t)));
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* The device stopped consuming: log what SDL says about it, then close and
 * reopen it. The queue's contents go with the old device (they are stale by
 * now); the pacer refills the new one. Called by the APU's pacer when it
 * declares the stall and again every few seconds while it lasts. */
/* Open a fresh device, off the APU thread: opening one can take a while on
 * Android, and nothing about it may stall the game. */
static int recover_thread(void *arg)
{
    SDL_AudioSpec have;
    SDL_AudioDeviceID d;
    uint64_t t0 = SDL_GetTicks64();
    (void)arg;
    memset(&have, 0, sizeof have);
    d = SDL_OpenAudioDevice(NULL, 0, &g_want, &have, 0);
    if (!d) {
        fprintf(stderr, "[SDL2] audio: opening a new device failed: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(d, 0);
        fprintf(stderr, "[SDL2] audio: new device %u open in %llu ms (%d-sample buffers)\n",
                (unsigned)d, (unsigned long long)(SDL_GetTicks64() - t0),
                have.samples ? have.samples : g_buf_samples);
        SDL_AtomicSet(&g_new_dev, (int)d);
    }
    SDL_AtomicSet(&g_recovering, 0);
    return 0;
}

/* The device stopped consuming: say what SDL reports of it and have a
 * helper thread open a new one (recover_thread; dev_take swaps it in). The
 * APU meanwhile paces on the host clock and drops periods, as a stall
 * always did: the game never waits on audio. RECOMP_AUDIO_RECOVER=0 only
 * logs. Returns 0: any new device arrives later. */
int xa2_recover(void)
{
    static int off = -1;
    SDL_AudioStatus st;
    const char *driver;
    SDL_Thread *t;

    if (!g_initialized || !g_dev)
        return 0;
    if (off < 0) {
        const char *e = getenv("RECOMP_AUDIO_RECOVER");
        off = e && *e == '0';
    }
    st = SDL_GetAudioDeviceStatus(g_dev);       /* reads flags; takes no lock */
    driver = SDL_GetCurrentAudioDriver();
    g_recoveries++;
    fprintf(stderr, "[SDL2] audio device %u not consuming: SDL says %s, driver %s, %u bytes"
            " queued; %s (attempt %u)\n", (unsigned)g_dev,
            st == SDL_AUDIO_PLAYING ? "playing" : st == SDL_AUDIO_PAUSED ? "paused" : "stopped",
            driver ? driver : "?", SDL_GetQueuedAudioSize(g_dev),
            off ? "left alone (RECOMP_AUDIO_RECOVER=0)"
                : SDL_AtomicGet(&g_recovering) ? "a new one is still being opened"
                : "opening a new one", g_recoveries);
    if (off || SDL_AtomicGet(&g_recovering))
        return 0;
    SDL_AtomicSet(&g_recovering, 1);
    t = SDL_CreateThread(recover_thread, "audio recover", NULL);
    if (!t) {
        SDL_AtomicSet(&g_recovering, 0);
        return 0;
    }
    SDL_DetachThread(t);
    return 0;
}

/* What the device thread has pulled off the queue; it pulls one device
 * buffer per callback, so this leads the speaker by up to one buffer. */
uint64_t xa2_samples_played(void)
{
    uint64_t queued;

    if (!g_initialized || !g_dev) return 0;
    queued = SDL_GetQueuedAudioSize(g_dev) / (SDLA_CHANNELS * sizeof(int16_t));
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */
