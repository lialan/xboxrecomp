/**
 * SDL2 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux, so the APU
 * monitor path that feeds XAudio2 on Windows reaches a real device here.
 * SDL's push API (SDL_QueueAudio) is the queue; its device thread pulls one
 * device buffer per callback and pads a short pull with silence.
 *
 * Like the XAudio2 backend, a submission is refused while XA2_NUM_BUFS
 * buffers are still queued: the monitor drops that period rather than let
 * the queue (unbounded in SDL) grow latency without limit.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIODRIVER is already set: it drains the queue in real time without
 * a device, so nothing reaches the speakers.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

#if !defined(_WIN32)

#include <SDL.h>

#define SDLA_SAMPLE_RATE  48000
#define SDLA_CHANNELS     2
#define SDLA_BUF_SAMPLES  1024   /* same as XA2_BUF_SAMPLES: ~21 ms */
#define SDLA_NUM_BUFS     3
#define SDLA_FRAME_BYTES  (SDLA_CHANNELS * (int)sizeof(int16_t))

static SDL_AudioDeviceID g_dev;
static int g_initialized;
static int g_audio_opened;      /* this backend brought SDL audio up */
static int g_frames_written;

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *e, *driver;

    if (g_initialized) return 1;

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
    want.samples  = SDLA_BUF_SAMPLES;
    want.callback = NULL;                       /* push: SDL_QueueAudio */

    /* No format changes allowed: the mixer's 48 kHz s16 stereo is what the
     * queue takes, and SDL converts for the device if it has to. */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_dev == 0) {
        fprintf(stderr, "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }

    g_frames_written = 0;
    g_initialized = 1;
    SDL_PauseAudioDevice(g_dev, 0);

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL2] audio initialized: driver %s (48kHz stereo 16-bit, %d x %d-sample buffers)%s\n",
            driver ? driver : "?", SDLA_NUM_BUFS, SDLA_BUF_SAMPLES,
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
        fprintf(stderr, "[SDL2] audio shut down (%d frames written)\n", g_frames_written);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Hand one buffer to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!g_initialized || !g_dev || num_samples <= 0) return 0;
    if (num_samples > SDLA_BUF_SAMPLES) num_samples = SDLA_BUF_SAMPLES;
    /* XAudio2 counts the buffer that is playing; SDL's queue does not, and
     * the device holds one buffer's worth of its own. */
    if (SDL_GetQueuedAudioSize(g_dev) >=
        (Uint32)((SDLA_NUM_BUFS - 1) * SDLA_BUF_SAMPLES * SDLA_FRAME_BYTES))
        return 0;
    if (SDL_QueueAudio(g_dev, samples, (Uint32)(num_samples * SDLA_FRAME_BYTES)) != 0)
        return 0;
    g_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return SDLA_BUF_SAMPLES;
}

#endif /* !_WIN32 */
