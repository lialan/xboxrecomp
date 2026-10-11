/* mcpx_apu_monitor_frame must hand the device what the DSP stage produced.
 *
 * It used to clear frame_buf and render only the test tone and the software
 * mixer into it, so every sample the VP and DSP made was thrown away before
 * the device saw it, and it rendered a 1024-sample device buffer for every
 * 256-sample EP period. Here the device is a fake that records what it is
 * given, and the frame thread's sequence (dsp_frame then monitor_frame, eight
 * times per period) is driven by hand. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define PERIOD 256
#define SUB    NUM_SAMPLES_PER_FRAME      /* 32 */

extern volatile int g_audio_muted;

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ----
 * It records what it is given and, for the pacing case, models a queue that
 * a consumption clock drains (dev_*). */
static int16_t got[PERIOD * 8][2];
static int got_samples, got_calls;

static int      dev_model;          /* pacing case: check sequence numbers  */
static uint64_t dev_tail, dev_head; /* samples submitted / consumed         */
static int      dev_seq_errors;

int  xa2_init(void)          { return 1; }
void xa2_shutdown(void)      {}
int  xa2_is_active(void)     { return 1; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void) { return 0; }
int  xa2_queued_samples(void) { return (int)(dev_tail - dev_head); }
void xa2_expect_drain(void)  {}
int  xa2_submit_samples(const int16_t *s, int n)
{
    got_calls++;
    if (dev_model) {
        /* Sample k carries its sequence number: left = k & 0x7FFF,
         * right = k >> 15. Anything dropped or doubled breaks the run. */
        for (int i = 0; i < n; i++) {
            uint64_t k = dev_tail + (uint64_t)i;
            if ((uint16_t)s[2 * i] != (k & 0x7FFF)
                    || (uint16_t)s[2 * i + 1] != (uint16_t)(k >> 15))
                dev_seq_errors++;
        }
        dev_tail += (uint64_t)n;
        return 1;
    }
    if (got_samples + n <= PERIOD * 8)
        memcpy(got[got_samples], s, (size_t)n * 2 * sizeof(int16_t));
    got_samples += n;
    return 1;
}

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* What dsp_frame writes for bin value v: (int16_t)(v * 32767). Bin 0 is
 * left and bin 1 right in both mixdown modes. */
static float left_of(int k, int i)  { return (float)(k * SUB + i) / 4096.0f; }
static float right_of(int k, int i) { return -(float)(k * SUB + i) / 8192.0f; }

/* One EP period the way the frame thread runs it. */
static void run_period(MCPXAPUState *d)
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int calls0 = got_calls;
    for (int k = 0; k < 8; k++) {
        memset(mixbins, 0, sizeof(mixbins));
        for (int i = 0; i < SUB; i++) {
            mixbins[0][i] = left_of(k, i);
            mixbins[1][i] = right_of(k, i);
        }
        d->ep_frame_div = k;
        mcpx_apu_dsp_frame(d, mixbins);
        mcpx_apu_monitor_frame(d);
        if (k < 7)
            CHECK("nothing submitted mid-period", got_calls == calls0);
    }
}

static int16_t want_l(int s) { return (int16_t)(left_of(s / SUB, s % SUB) * 32767.0f); }
static int16_t want_r(int s) { return (int16_t)(right_of(s / SUB, s % SUB) * 32767.0f); }

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* ---- pacing by queue depth ----
 * The frame thread's decision (mcpx_apu_pace_step) against the fake queue,
 * on a simulated clock: the device consumes 48 samples per millisecond
 * except during a stall. Rendering takes no simulated time, which is the
 * worst case for running ahead. */
static uint64_t sim_consumed_at(int64_t now_us, int64_t stall_from,
                                int64_t stall_us)
{
    int64_t t = now_us;
    if (t > stall_from)
        t = (t < stall_from + stall_us) ? stall_from : t - stall_us;
    return (uint64_t)(t * 48 / 1000);
}

static void pacing_case(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    const int low = 4, high = 8;             /* granules: 512 x 4 marks */
    const int64_t stall_from = 3000000, stall_us = 100000;
    int64_t now = 0;
    uint64_t seq = 0, underruns = 0;
    int periods = 0, minq = 1 << 30, maxq = 0, max_q_stall = 0;
    int submits_in_stall = 0, waits = 0;

    if (!d) { failures++; return; }
    d->monitor.queued_bytes_low  = low  * PERIOD * 4;
    d->monitor.queued_bytes_high = high * PERIOD * 4;
    dev_model = 1; dev_tail = dev_head = 0; dev_seq_errors = 0;

    while (periods < 1000) {
        uint64_t c = sim_consumed_at(now, stall_from, stall_us);
        int q;
        if (c > dev_tail) {                  /* ran dry: plays silence */
            underruns++;
            dev_head = dev_tail;
        } else {
            dev_head = c > dev_head ? c : dev_head;
        }
        q = xa2_queued_samples() / PERIOD;
        if (now > 200000 && !(now >= stall_from && now < stall_from + stall_us + 200000)) {
            if (q < minq) minq = q;
            if (q > maxq) maxq = q;
        }
        if (now >= stall_from && now < stall_from + stall_us && q > max_q_stall)
            max_q_stall = q;

        d->ep_frame_div = 0;
        int64_t w = mcpx_apu_pace_step(d, now);
        if (w > 0) { now += w; waits++; continue; }

        for (int i = 0; i < PERIOD; i++) {
            uint64_t k = seq + (uint64_t)i;
            d->monitor.frame_buf[i][0] = (int16_t)(k & 0x7FFF);
            d->monitor.frame_buf[i][1] = (int16_t)(uint16_t)(k >> 15);
        }
        d->ep_frame_div = 7;
        mcpx_apu_monitor_frame(d);
        mcpx_apu_pace_rendered(d);
        seq += PERIOD;
        periods++;
        if (now >= stall_from && now < stall_from + stall_us)
            submits_in_stall++;
        /* Rendering starts only below high, so one period takes the queue
         * at most PERIOD - 1 samples past it (this fake drains sample by
         * sample; a real voice reports whole buffers). */
        if (xa2_queued_samples() > high * PERIOD + PERIOD - 1) {
            printf("FAIL: queue %d samples > high %d + one period after a submit\n",
                   xa2_queued_samples(), high * PERIOD);
            failures++;
            break;
        }
    }
    printf("pacing: 1000 periods in %.3f s simulated, queue %d..%d steady, "
           "%d at most in the stall, %llu underruns, %d submits during the stall\n",
           now / 1e6, minq, maxq, max_q_stall, (unsigned long long)underruns,
           submits_in_stall);
    CHECK("steady queue stays within [low, high]", minq >= low && maxq <= high);
    CHECK("stall stops submission at high", max_q_stall <= high);
    CHECK("no underrun after start", underruns <= 1);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0
          && dev_tail == 1000ull * PERIOD);
    CHECK("runs at the device rate (1000 periods ~ 5.33 s)",
          now > 5200000 + stall_us - 100000 && now < 5500000 + stall_us);
    dev_model = 0;
    free(d);
}

/* ---- the frame loop around the pacer ----
 * One frame-thread pass at a period boundary, as mcpx_apu_frame_thread runs
 * it: throttle (pace_step until it says go), then, unless the front end is
 * trapped, render and submit a period and charge it (pace_rendered). A
 * trapped pass renders nothing, charges nothing and waits a millisecond. Sample k of the stream carries k (as the fake checks)
 * counted from what the device was given, so dropped periods are not gaps. */
struct loop_stats {
    int periods, submits, underruns, minq, maxq;
    int trapped_passes, trapped_charges;
};

static void consume(int64_t now, int64_t stall_from, int64_t stall_us,
                    struct loop_stats *st)
{
    uint64_t c = sim_consumed_at(now, stall_from, stall_us);
    if (c > dev_tail) {                      /* ran dry: plays silence */
        st->underruns++;
        dev_head = dev_tail;
    } else {
        dev_head = c > dev_head ? c : dev_head;
    }
}

static int64_t loop_pass(MCPXAPUState *d, int64_t now, int64_t stall_from,
                         int64_t stall_us, struct loop_stats *st)
{
    bool trapped = (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) != 0;
    int64_t sched = d->next_frame_time_us;

    d->ep_frame_div = 0;
    for (;;) {                               /* throttle */
        consume(now, stall_from, stall_us, st);
        int64_t w = mcpx_apu_pace_step(d, now);
        if (w <= 0) break;
        now += w;
    }
    if (trapped) {
        st->trapped_passes++;
        if (d->next_frame_time_us != sched)
            st->trapped_charges++;
        return now + 1000;
    }
    int tail0 = (int)dev_tail;
    for (int i = 0; i < PERIOD; i++) {
        uint64_t k = dev_tail + (uint64_t)i;
        d->monitor.frame_buf[i][0] = (int16_t)(k & 0x7FFF);
        d->monitor.frame_buf[i][1] = (int16_t)(uint16_t)(k >> 15);
    }
    d->ep_frame_div = 7;
    mcpx_apu_monitor_frame(d);
    mcpx_apu_pace_rendered(d);
    st->periods++;
    if ((int)dev_tail != tail0)
        st->submits++;
    return now;
}

static MCPXAPUState *loop_device(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->monitor.queued_bytes_low  = 4 * PERIOD * 4;   /* granules, as above */
    d->monitor.queued_bytes_high = 8 * PERIOD * 4;
    /* XCNTMODE on, front end free running. */
    d->regs[NV_PAPU_SECTL] = 1u << 3;
    dev_model = 1; dev_tail = dev_head = 0; dev_seq_errors = 0;
    return d;
}

/* A trap that starts on a period boundary must not spend pacing credit:
 * the trapped loop passes once a millisecond, and each pass used to advance
 * the schedule a whole period, so the lead the queue had built drained
 * after the trap. Here a 10 ms trap begins at a boundary every 60 ms. */
static void trapped_case(void)
{
    MCPXAPUState *d = loop_device();
    struct loop_stats st = { 0, 0, 0, 1 << 30, 0, 0, 0 };
    int64_t now = 0, trap_end = 0, next_trap = 500000;

    if (!d) { failures++; return; }
    while (now < 4000000) {
        if (!trap_end && now >= next_trap) {
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            trap_end = now + 10000;
        } else if (trap_end && now >= trap_end) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            trap_end = 0;
            next_trap = now + 60000;
        }
        now = loop_pass(d, now, 1LL << 60, 0, &st);
        if (now > 300000) {
            int q = xa2_queued_samples() / PERIOD;
            if (q < st.minq) st.minq = q;
            if (q > st.maxq) st.maxq = q;
        }
    }
    printf("trapped: %d periods, %d trapped passes, %d charged, queue %d..%d,"
           " %d underruns\n", st.periods, st.trapped_passes,
           st.trapped_charges, st.minq, st.maxq, st.underruns);
    CHECK("traps happened", st.trapped_passes > 100);
    CHECK("a trapped pass spends no pacing credit", st.trapped_charges == 0);
    CHECK("queue never drains across a trap", st.underruns <= 1 && st.minq >= 1);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0);
    dev_model = 0;
    free(d);
}

/* A device that stops consuming for good must not stop the APU clock: after
 * the queue has sat at high for 200 ms the host clock paces, periods are
 * dropped rather than queued, and when the device drains again the queue
 * paces once more. Here the device stops at 2 s and resumes at 3 s. */
static void stall_fallback_case(void)
{
    MCPXAPUState *d = loop_device();
    struct loop_stats st = { 0, 0, 0, 1 << 30, 0, 0, 0 };
    const int64_t stall_from = 2000000, stall_us = 1000000;
    int64_t now = 0;
    int in_stall_periods = 0, in_stall_submits = 0, after_underruns = 0;
    int periods0, submits0, underruns0, max_q = 0;

    if (!d) { failures++; return; }
    while (now < 5000000) {
        periods0 = st.periods; submits0 = st.submits; underruns0 = st.underruns;
        int64_t t = now;
        now = loop_pass(d, now, stall_from, stall_us, &st);
        if (t >= stall_from + 250000 && now < stall_from + stall_us) {
            in_stall_periods += st.periods - periods0;
            in_stall_submits += st.submits - submits0;
        }
        if (t >= stall_from + stall_us)
            after_underruns += st.underruns - underruns0;
        if (t >= stall_from + stall_us + 300000) {
            int q = xa2_queued_samples() / PERIOD;
            if (q < st.minq) st.minq = q;
            if (q > st.maxq) st.maxq = q;
        }
        if (xa2_queued_samples() / PERIOD > max_q)
            max_q = xa2_queued_samples() / PERIOD;
    }
    printf("stall fallback: %d periods rendered and %d submitted in the last"
           " 750 ms of a 1 s stall; after it queue %d..%d, %d underruns;"
           " queue %d at most\n", in_stall_periods, in_stall_submits,
           st.minq, st.maxq, after_underruns, max_q);
    CHECK("stalled device: the clock keeps running (~140 periods)",
          in_stall_periods > 120 && in_stall_periods < 160);
    CHECK("stalled device: periods are dropped, not queued", in_stall_submits == 0);
    CHECK("queue never grows past high + one period", max_q <= 9);
    CHECK("device back: the queue paces again", st.minq >= 4 && st.maxq <= 8);
    CHECK("device back: no underrun", after_underruns == 0);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0);
    dev_model = 0;
    free(d);
}

int main(void)
{
    /* Zeroed: ram_ptr NULL keeps dsp_ack_frame out of guest memory, and
     * monitor.point 0 is MON_AC97, so dsp_frame writes frame_buf. */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    int ok, i;
    if (!d) return 2;
    printf("apu_monitor: running\n");

    /* 1. The DSP's period reaches the device unchanged, once, 256 samples. */
    got_calls = got_samples = 0;
    run_period(d);
    CHECK("one submit per period", got_calls == 1);
    CHECK("exactly 256 samples", got_samples == PERIOD);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (got[i][0] != want_l(i) || got[i][1] != want_r(i)) ok = 0;
    CHECK("DSP frame reaches the device unchanged", ok);
    CHECK("signal is not silence", got[PERIOD - 1][0] != 0 && got[PERIOD - 1][1] != 0);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (d->monitor.frame_buf[i][0] || d->monitor.frame_buf[i][1]) ok = 0;
    CHECK("frame_buf starts the next period silent", ok);

    /* 2. With a software-mixer voice playing, the device gets the sum. */
    {
        static int16_t pcm[64][2];
        int slot = apu_mixer_alloc_voice();
        APUMixerVoice *v = apu_mixer_get_voice(slot);
        CHECK("mixer voice allocated", slot >= 0 && v);
        for (i = 0; i < 64; i++) { pcm[i][0] = 1000; pcm[i][1] = -2000; }
        v->pcm_data = &pcm[0][0];
        v->pcm_bytes = sizeof(pcm);
        v->num_channels = 2;
        v->sample_rate = 48000;
        v->volume = 1.0f;
        apu_mixer_play(slot, 1);

        got_calls = got_samples = 0;
        run_period(d);
        CHECK("one submit with the mixer on", got_calls == 1 && got_samples == PERIOD);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] != clamp16(want_l(i) + 1000)
                    || got[i][1] != clamp16(want_r(i) - 2000)) ok = 0;
        CHECK("mixer is added to the DSP frame", ok);

        /* 3. Muted: silence, still one period. */
        g_audio_muted = 1;
        got_calls = got_samples = 0;
        run_period(d);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] || got[i][1]) ok = 0;
        CHECK("muted period is silent", ok && got_samples == PERIOD);
        g_audio_muted = 0;
        apu_mixer_free_voice(slot);
    }

    /* 4. Ten periods: 2560 samples, no more, no fewer. */
    got_calls = got_samples = 0;
    for (i = 0; i < 10; i++) run_period(d);
    CHECK("ten periods, ten submits", got_calls == 10);
    CHECK("ten periods, 2560 samples", got_samples == 10 * PERIOD);

    free(d);
    pacing_case();
    trapped_case();
    stall_fallback_case();
    if (failures == 0) { printf("apu_monitor: ALL PASS\n"); return 0; }
    printf("apu_monitor: %d FAILURE(S)\n", failures);
    return 1;
}
