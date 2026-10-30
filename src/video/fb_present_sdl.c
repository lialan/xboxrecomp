/**
 * Show the guest framebuffer in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, no display, or SDL video failing, the guest runs
 * on the calling thread exactly as before.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a mutex held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. The surface
 * format is the GDI window's: 640x480, bpp from pitch / 640, 2 = R5G6B5,
 * 4 = X8R8G8B8. Before the first flip the window reads the AvSetDisplayMode
 * surface directly, as the GDI window does, for a title that never flips.
 *
 * Closing the window ends the process: the window owns the main thread, and
 * lifted code cannot be stopped mid-flight. atexit handlers still run.
 *
 * A back end may hand the window a group of frames per flip instead
 * (xbox_FramebufferWindowPresentFrames): in-between frames it made, then the
 * real one. The window spreads a group over the flip interval it measures --
 * frame i of n at arrival + i * interval / n -- on this thread, so a higher
 * display rate costs the executor one copy per frame and never a vsync. A
 * flip that delivered a group skips the plain copy. A group's frames may be
 * larger than the guest surface (a back end drawing at a higher internal
 * resolution) and of another shape, and fill the picture.
 *
 * The picture's logical size is FB_W x FB_H unless a back end sets another
 * (xbox_FramebufferWindowSetPicture, e.g. 16:9); it is the renderer's
 * logical size, so mouse coordinates stay in picture space, and a window
 * that is not fullscreen or maximised takes its shape at its height. A
 * plain frame is shown at its own 4:3 shape in the middle of it.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2).
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "video_player.h"
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_InputSetMainPump(int on);   /* src/input/xinput_device.c */
extern int  xbox_InputAnyOpen(void);

/* The displayed surface. 640x480 unless RECOMP_FB_SIZE=WxH says otherwise
 * (Sega Chihiro titles run at 1024x768); read once, before the window opens
 * and before any flip is copied. */
static uint32_t s_fb_w = 640u, s_fb_h = 480u;
#define FB_W s_fb_w
#define FB_H s_fb_h

static void fb_size_from_env(void)
{
    const char *e = getenv("RECOMP_FB_SIZE");
    unsigned w, h;

    if (e && sscanf(e, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64
            && w <= 4096 && h <= 4096) {
        s_fb_w = w;
        s_fb_h = h;
    }
}

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

/* A slot holds a group of n frames, back to back, each w * h * bpp; fill
 * is set for a back end's group, which fills the picture. A plain flip is a
 * group of one FB_W x FB_H frame. */
#define FB_GROUP_MAX 8

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t bpp;
    int      n, fill;
    uint32_t w, h;
};

/* The picture's logical size, 0 for FB_W x FB_H. */
static volatile uint32_t s_pic_w, s_pic_h;

void xbox_FramebufferWindowSetPicture(uint32_t lw, uint32_t lh)
{
    if (lw && lh && lw <= 8192 && lh <= 8192) {
        __atomic_store_n(&s_pic_w, lw, __ATOMIC_RELAXED);
        __atomic_store_n(&s_pic_h, lh, __ATOMIC_RELAXED);
    }
}

/* What the window takes from SDL, and gives back on every way out. */
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static volatile int    s_wake_pending;
static volatile int    s_in_present;         /* executor inside Present */
static volatile uint32_t s_fb_va, s_fb_pitch; /* AvSetDisplayMode surface */

/* Counted at the flip, read once a second for the title bar. */
static uint64_t s_flips;
static uint32_t s_frame_draws;

/* The flip (s_flips) that delivered a group: its plain present stands down.
 * Executor thread only. */
static uint64_t s_group_flip = (uint64_t)-1;

/* Frames put on screen, for a back end's rate log. */
static uint64_t s_presents;

uint64_t xbox_FramebufferWindowPresentCount(void)
{
    return __atomic_load_n(&s_presents, __ATOMIC_RELAXED);
}

int xbox_FramebufferWindowOpen(void)
{
    return __atomic_load_n(&s_running, __ATOMIC_RELAXED);
}

/* Called on the main thread once the window and renderer exist: a title
 * adds its menus there (AppKit wants them on that thread, after SDL has
 * built the menu bar). */
static void (*s_open_hook)(void);

void xbox_HostWindowOnOpen(void (*fn)(void))
{
    s_open_hook = fn;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ponytail: the SDL window shows the surface unscaled; 2x2 anti-aliased
 * surfaces (#162) come through at twice the height until it honours this. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t bpp, int n, uint32_t w, uint32_t h, int fill)
{
    s_slot[s_back].bpp = bpp;          /* only this thread touches s_back */
    s_slot[s_back].n = n;
    s_slot[s_back].w = w;
    s_slot[s_back].h = h;
    s_slot[s_back].fill = fill;
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);

    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
    }
}

/* Copy n frames, each w x h rows of `pitch` bytes, into the back slot and
 * publish them as one group. */
static void group_copy(const uint8_t *const *src, int n, uint32_t w,
                       uint32_t h, uint32_t pitch, uint32_t bpp, int fill)
{
    struct fb_slot *s = &s_slot[s_back];
    size_t frame, need;
    uint32_t y;
    int i;

    if ((bpp != 2 && bpp != 4) || pitch < w * bpp || n < 1
            || n > FB_GROUP_MAX || !w || !h || w > 8192 || h > 8192)
        return;
    frame = (size_t)w * h * bpp;
    need = frame * (size_t)n;
    if (s->cap < need) {
        uint8_t *p = (uint8_t *)realloc(s->px, need);
        if (!p)
            return;
        s->px = p;
        s->cap = need;
    }
    for (i = 0; i < n; i++) {
        if (pitch == w * bpp) {
            memcpy(s->px + frame * i, src[i], frame);
            continue;
        }
        for (y = 0; y < h; y++)
            memcpy(s->px + frame * i + (size_t)y * w * bpp,
                   src[i] + (size_t)y * pitch, (size_t)w * bpp);
    }
    publish(bpp, n, w, h, fill);
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src =
        (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    group_copy(&src, 1, FB_W, FB_H, pitch, pitch / FB_W, 0);
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    if (s_group_flip == __atomic_load_n(&s_flips, __ATOMIC_RELAXED))
        return;                         /* the back end showed this flip */
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Called by a back end from inside the flip, on the executor thread, before
 * the plain present: n frames of w x h at `pitch`, the last one real, the
 * others to be shown before it at even steps of the flip interval, each
 * filling the picture. The flip's plain present then does nothing. */
void xbox_FramebufferWindowPresentFrames(const void *const *frames, int n,
                                         uint32_t w, uint32_t h, uint32_t pitch,
                                         uint32_t bpp)
{
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !frames || n < 1)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
        group_copy((const uint8_t *const *)frames, n, w, h, pitch, bpp, 1);
        s_group_flip = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it.
 * Written by the loader and the flip, read once a second by the main
 * thread; a torn read shows one stale value for a second. */
static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    uint32_t tex_bpp = 0, tex_w = 0, tex_h = 0, log_w, log_h;
    int tex_fill = 0;
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0;
    const char *e;
    int vsync, scale, quit = 0, guest_done = 0, dirty = 1;
    /* The group on screen: frame `next` of `n` is due at arrival + next *
     * period / n. The period is the flip interval, measured between groups
     * of more than one frame (a group of one is shown at once). */
    int group_n = 0, group_next = 0;
    double t_group = 0.0, t_group_prev = 0.0, period = 1000.0 / 60.0;
    const double tick_ms = 1000.0 / (double)SDL_GetPerformanceFrequency();

    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();

    fb_size_from_env();
    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : 2;
    if (scale < 1 || scale > 8)
        scale = 2;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)FB_W * scale, (int)FB_H * scale,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1,
                                   vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
              : NULL;
    if (!ren) {
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
    log_w = FB_W;
    log_h = FB_H;
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL_RegisterEvents failed; running headless\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
    s_guest_main = guest_main;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the input layer then only reads them. */
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    s_running = 1;
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        s_running = 0;
        xbox_InputSetMainPump(0);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    if (s_open_hook)
        s_open_hook();

    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t bpp = 0, pitch = 0, fw = FB_W, fh = FB_H, lw, lh;
        int fill = 0;
        int timeout = shown_seq ? 100 : 16;
        double t_now;

        /* Before the first flip, poll at the display rate: the window then
         * reads the display surface itself. After, the flip's event wakes
         * the loop -- or the next frame of a group falls due. */
        if (group_next < group_n) {
            double due = t_group + period * group_next / group_n;
            double left = due - (double)SDL_GetPerformanceCounter() * tick_ms;
            timeout = left <= 0.0 ? 0 : (int)left + 1;
        }
        if (timeout == 0 ? SDL_PollEvent(&ev) : SDL_WaitEventTimeout(&ev, timeout)) {
            do {
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. */
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event)
                    guest_done = 1;
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
            } while (SDL_PollEvent(&ev));
        }

        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        t_now = (double)SDL_GetPerformanceCounter() * tick_ms;
        if (seq != shown_seq) {
            /* A new group replaces whatever is left of the last one: its
             * first frame lies between that one's real frame and its own. */
            shown_seq = seq;
            group_n = s_slot[s_front].n;
            group_next = 0;
            if (group_n > 1) {
                double dt = t_now - t_group_prev;
                if (t_group_prev > 0.0 && dt > 5.0 && dt < 50.0)
                    period += (dt - period) * 0.1;
                t_group_prev = t_now;
            }
            t_group = t_now;
        }
        if (group_next < group_n
                && t_now >= t_group + period * group_next / group_n) {
            /* The latest frame that is due; any earlier one is too late. */
            while (group_next + 1 < group_n
                   && t_now >= t_group + period * (group_next + 1) / group_n)
                group_next++;
            bpp = s_slot[s_front].bpp;
            fw = s_slot[s_front].w;
            fh = s_slot[s_front].h;
            fill = s_slot[s_front].fill;
            pitch = fw * bpp;
            px = s_slot[s_front].px + (size_t)pitch * fh * group_next;
            group_next++;
        } else if (!shown_seq && s_fb_va && s_fb_pitch) {
            /* No flip yet: the display surface, as the GDI window reads it. */
            bpp = s_fb_pitch / FB_W;
            pitch = s_fb_pitch;
            if (bpp == 2 || bpp == 4)
                px = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
        }
        lw = __atomic_load_n(&s_pic_w, __ATOMIC_RELAXED);
        lh = __atomic_load_n(&s_pic_h, __ATOMIC_RELAXED);
        if (!lw || !lh) {
            lw = FB_W;
            lh = FB_H;
        }
        if (lw != log_w || lh != log_h) {
            /* Another picture shape (an aspect switch): the renderer's
             * logical size follows, and a window that is neither
             * fullscreen nor maximised takes the shape at its height. */
            int ww, wh;
            Uint32 fl = SDL_GetWindowFlags(win);
            SDL_RenderSetLogicalSize(ren, (int)lw, (int)lh);
            if (!(fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
                SDL_GetWindowSize(win, &ww, &wh);
                SDL_SetWindowSize(win, (int)((double)wh * lw / lh + 0.5), wh);
            }
            log_w = lw;
            log_h = lh;
            dirty = 1;
            fprintf(stderr, "  [FBWIN] picture %ux%u\n", lw, lh);
        }
        if (px) {
            tex_fill = fill;
            if (!tex || bpp != tex_bpp || fw != tex_w || fh != tex_h) {
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, bpp == 4 ? SDL_PIXELFORMAT_RGB888
                                                      : SDL_PIXELFORMAT_RGB565,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        (int)fw, (int)fh);
                /* A frame drawn larger than the guest's is scaled, not
                 * pixel-doubled: filter it. */
                if (tex && (fw != FB_W || fh != FB_H))
                    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                tex_bpp = bpp;
                tex_w = fw;
                tex_h = fh;
            }
            if (tex)
                SDL_UpdateTexture(tex, NULL, px, (int)pitch);
            dirty = 1;
        }
        if (dirty) {
            dirty = 0;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex && (tex_fill || (log_w == FB_W && log_h == FB_H))) {
                SDL_RenderCopy(ren, tex, NULL, NULL);
            } else if (tex) {
                /* A guest frame in a picture of another shape: at its own
                 * shape, in the middle. */
                SDL_Rect d;
                d.h = (int)log_h;
                d.w = (int)((double)log_h * FB_W / FB_H + 0.5);
                d.x = ((int)log_w - d.w) / 2;
                d.y = 0;
                SDL_RenderCopy(ren, tex, NULL, &d);
            }
            SDL_RenderPresent(ren);
            __atomic_add_fetch(&s_presents, 1, __ATOMIC_RELAXED);
        }

        now = now_ms();
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char tb[192];
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", s_title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)__atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
            SDL_SetWindowTitle(win, tb);
            t_title = now;
            flips_title = f;
        }
    }

    /* No present may be mid-publish (it pushes an SDL event) once SDL goes. */
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    /* Window and renderer go first, on this thread as AppKit wants. SDL_Quit
     * is only safe once nothing else uses SDL: the USB and guest threads read
     * the game controllers, so with one open SDL stays up and exit()
     * reclaims it. The main-pump flag stays set, so those threads do not
     * start calling SDL_GameControllerUpdate while SDL goes down. */
    if (tex)
        SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (!xbox_InputAnyOpen())
        SDL_Quit();
    else
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        exit(0);
    return s_guest_rc;
}

#endif /* !_WIN32 */
