/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* The device backend behind these functions: XAudio2 on Windows
 * (apu_xaudio2.c), SDL2 on POSIX hosts (apu_sdl2.c). Same contract. */
#if defined(_WIN32)
#define XA2_BACKEND_NAME "XAudio2"
#else
#define XA2_BACKEND_NAME "SDL2"
#endif

/* Initialize the device backend. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

#endif /* APU_XAUDIO2_H */
