/*
 * ohci.c -- OHCI 1.0a host controller registers for the MCPX.
 *
 * See ohci.h for what this is and is not. The short version: enough for a
 * title's own USB driver to find a controller and a populated root hub port,
 * plus a trace of every register access, because what the driver does after
 * that decides how the rest gets built.
 */
#include "ohci.h"
#include "../platform/mmio_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* The runtime maps guest memory at a fixed host offset. */
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- OHCI 1.0a operational registers, by byte offset ------------------- */
#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54
#define OHCI_REG_MAX            0x60

/* HcCommandStatus */
#define CS_HCR                  0x00000001u   /* host controller reset       */

/* HcInterruptStatus / Enable */
#define INTR_SO                 0x00000001u   /* scheduling overrun          */
#define INTR_WDH                0x00000002u   /* writeback done head         */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_RD                 0x00000008u   /* resume detected             */
#define INTR_UE                 0x00000010u   /* unrecoverable error         */
#define INTR_FNO                0x00000020u   /* frame number overflow       */
#define INTR_RHSC               0x00000040u   /* root hub status change      */
#define INTR_MIE                0x80000000u   /* master interrupt enable     */

/* HcRhPortStatus */
#define PORT_CCS                0x00000001u   /* current connect status      */
#define PORT_PES                0x00000002u   /* port enable status          */
#define PORT_PSS                0x00000004u   /* port suspend status         */
#define PORT_PPS                0x00000100u   /* port power status           */
#define PORT_LSDA               0x00000200u   /* low speed device attached   */
#define PORT_CSC                0x00010000u   /* connect status change       */
#define PORT_PESC               0x00020000u   /* enable status change        */
#define PORT_PRSC               0x00100000u   /* reset status change         */

/* Writes to HcRhPortStatus set/clear by bit position rather than by value. */
#define PORT_W_CCS_CLEAR_ENABLE 0x00000001u   /* ClearPortEnable             */
#define PORT_W_PES_SET_ENABLE   0x00000002u   /* SetPortEnable               */
#define PORT_W_PRS_SET_RESET    0x00000010u   /* SetPortReset                */
#define PORT_W_PPS_SET_POWER    0x00000100u   /* SetPortPower                */
#define PORT_W_CLEAR_POWER      0x00000200u   /* ClearPortPower              */

/* The MCPX gives each controller a small root hub. Two ports apiece covers
 * the console's four front sockets, which is what a title enumerates over. */
#define OHCI_PORTS              2

typedef struct {
    uint32_t base;                      /* Xbox VA of the register block   */
    uint32_t reg[OHCI_REG_MAX / 4];
    unsigned reads, writes, decode_fail;
    int      index;
} OhciController;

static OhciController s_hc[2];
static int s_enabled;
static int s_trace;

static uint32_t *reg_of(OhciController *hc, uint32_t off)
{
    return (off < OHCI_REG_MAX) ? &hc->reg[off / 4] : NULL;
}

/* ---- register semantics ------------------------------------------------ */

static uint64_t ohci_read(void *dev, uint32_t off, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = r ? *r : 0;

    hc->reads++;

    /* HcFmRemaining and HcFmNumber advance on their own. A driver that waits
     * for the frame counter to move is waiting for the controller to be
     * running, and a counter that never changes is a controller that is not.
     * Derived from the read count rather than a timer: it only has to be
     * monotonic, and a timer here would need a thread to be worth having. */
    if (aligned == HcFmNumber)
        v = (hc->reads >> 3) & 0xFFFFu;
    else if (aligned == HcFmRemaining)
        v = (hc->reads * 977u) & 0x3FFFu;

    /* Sub-dword reads take their slice of the containing register. */
    if (size < 4) {
        unsigned shift = (off & 3u) * 8u;
        v >>= shift;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n",
                    hc->index, off, (uint32_t)v);
    }
    return v;
}

static void ohci_write(void *dev, uint32_t off, uint64_t val, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = (uint32_t)val;

    hc->writes++;
    if (!r)
        return;

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n",
                    hc->index, off, v);
    }

    switch (aligned) {
    case HcRevision:                    /* read-only */
        return;

    case HcCommandStatus:
        /* HCR is self-clearing: the controller resets and drops the bit, and
         * a driver polls for exactly that. Leaving it set is a hang, and it
         * is the first thing a driver does, so it would be the only thing
         * anyone ever saw of this file. */
        *r |= v;
        if (*r & CS_HCR) {
            *r &= ~CS_HCR;
            hc->reg[HcControl / 4] &= ~0xC0u;    /* back to UsbReset state  */
            hc->reg[HcInterruptStatus / 4] = 0;
            hc->reg[HcInterruptEnable / 4] = 0;
        }
        return;

    case HcInterruptStatus:
        *r &= ~v;                       /* write 1 to clear                 */
        return;

    case HcInterruptEnable:
        hc->reg[HcInterruptEnable / 4] |= v;
        return;

    case HcInterruptDisable:
        hc->reg[HcInterruptEnable / 4] &= ~v;
        return;

    case HcFmNumber:
    case HcFmRemaining:
        return;                         /* driven by the controller         */

    case HcRhDescriptorA:
        /* NumberDownstreamPorts is ours; the driver may set the power and
         * over-current policy bits above it. */
        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        return;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4: {
        /* Writes here are set/clear requests by bit position, not a value to
         * store. Getting that wrong looks like a port that will not enable. */
        unsigned port = (aligned - HcRhPortStatus1) / 4;
        uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];

        if (v & PORT_W_CCS_CLEAR_ENABLE) *ps &= ~PORT_PES;
        if (v & PORT_W_PES_SET_ENABLE)   *ps |= (*ps & PORT_CCS) ? PORT_PES : 0;
        if (v & PORT_W_PPS_SET_POWER)    *ps |= PORT_PPS;
        if (v & PORT_W_CLEAR_POWER)      *ps &= ~PORT_PPS;
        if (v & PORT_W_PRS_SET_RESET) {
            /* Reset completes immediately: there is no wire to settle. A
             * device that is present comes back enabled, which is what the
             * driver is about to check. */
            if (*ps & PORT_CCS)
                *ps |= PORT_PES;
            *ps |= PORT_PRSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
        }
        /* The change bits are write-1-to-clear, in the high half. */
        *ps &= ~(v & 0xFFFF0000u);
        return;
    }

    default:
        break;
    }

    if (size == 4) {
        *r = v;
    } else {
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        *r = (*r & ~mask) | ((v << shift) & mask);
    }
}

/* ---- bring-up ---------------------------------------------------------- */

static void ohci_reset(OhciController *hc, uint32_t base, int index)
{
    memset(hc, 0, sizeof *hc);
    hc->base  = base;
    hc->index = index;

    hc->reg[HcRevision / 4]       = 0x00000010u;   /* OHCI 1.0             */
    hc->reg[HcFmInterval / 4]     = 0x27782EDFu;   /* 11999, FSMPS default */
    hc->reg[HcPeriodicStart / 4]  = 0x00003E67u;   /* 90% of the frame     */
    hc->reg[HcLSThreshold / 4]    = 0x00000628u;

    /* Root hub: OHCI_PORTS downstream, ports always powered, no over-current
     * reporting. NoPowerSwitching keeps a driver from waiting on a power-on
     * sequence that has nothing to switch. */
    hc->reg[HcRhDescriptorA / 4]  = (uint32_t)OHCI_PORTS | (1u << 9);
    hc->reg[HcRhDescriptorB / 4]  = 0x00000000u;
    hc->reg[HcRhStatus / 4]       = 0x00000000u;

    /* Controller 0 port 1 has the gamepad on it. Powered and connected, with
     * the connect-status-change bit set so the first root hub poll sees a new
     * device rather than one that was always there. */
    hc->reg[HcRhPortStatus1 / 4]     = PORT_PPS;
    hc->reg[(HcRhPortStatus1 + 4) / 4] = PORT_PPS;
    if (index == 0) {
        hc->reg[HcRhPortStatus1 / 4] |= PORT_CCS | PORT_CSC;
        hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
    }
}

void xbox_OhciInit(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    /* Opt-in. Without a descriptor list walker behind it a driver that finds
     * a port has nothing to enumerate, so this must not change how a title
     * behaves until the rest of it exists. */
    if (!getenv("RECOMP_USB"))
        return;

    s_enabled = 1;
    s_trace   = getenv("RECOMP_USB_TRACE") != NULL;
    ohci_reset(&s_hc[0], XBOX_OHCI0_BASE, 0);
    ohci_reset(&s_hc[1], XBOX_OHCI1_BASE, 1);

#if defined(_WIN32)
    /* The registers have to fault to be answered. The MCPX aperture is mapped
     * as plain committed memory, so both blocks are made inaccessible here and
     * the title's VEH routes the faults back to xbox_OhciHandleMmio.
     *
     * A failed protect switches the model off rather than leaving it half on:
     * a controller whose registers read as zero out of RAM is exactly the
     * situation this exists to end, and it would look identical. */
    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        int i;

        if (!off) {
            s_enabled = 0;
            fprintf(stderr, "  OHCI: guest memory not mapped yet; disabled\n");
            return;
        }
        for (i = 0; i < 2; i++) {
            DWORD old_protect;
            LPVOID at = (LPVOID)((uintptr_t)off + s_hc[i].base);
            if (!VirtualProtect(at, XBOX_OHCI_SIZE, PAGE_NOACCESS,
                                &old_protect)) {
                s_enabled = 0;
                fprintf(stderr, "  OHCI: cannot trap 0x%08X (error %lu); "
                                "disabled\n", s_hc[i].base, GetLastError());
                return;
            }
        }
    }
#endif

    fprintf(stderr, "  OHCI: two controllers at 0x%08X and 0x%08X, "
                    "%d ports each, one device on HC0 port 1\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, OHCI_PORTS);
    fflush(stderr);
}

static OhciController *hc_for(uint32_t va)
{
    int i;

    if (!s_enabled)
        return NULL;
    for (i = 0; i < 2; i++)
        if (va >= s_hc[i].base && va < s_hc[i].base + XBOX_OHCI_SIZE)
            return &s_hc[i];
    return NULL;
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    return hc_for(xbox_va) != NULL;
}

int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va)
{
#if defined(_WIN32)
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    ok = mmio_emulate((PCONTEXT)ctx, xbox_va - hc->base, hc,
                      ohci_read, ohci_write);
    if (!ok && hc->decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                hc->index, xbox_va - hc->base,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
#else
    (void)ctx; (void)xbox_va;
    return 0;
#endif
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u undecoded; "
                        "HcControl=%08X HcIntStatus=%08X port1=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].decode_fail,
                s_hc[i].reg[HcControl / 4],
                s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[HcRhPortStatus1 / 4]);
    fflush(stderr);
}
