/* PVIDEO, the NV2A video overlay, and the register model the title's D3D
 * drives it with.
 *
 *   - stop_*: STOP is a command. The title waits for BUFFER's pending bits to
 *     clear after it; on main STOP was stored and BUFFER kept its bits.
 *
 * Run with: ctest --test-dir <build> */
#include "nv2a_state.h"
#include "nv2a_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int s_failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

#define PVIDEO 0x8000u        /* PVIDEO's block in the register aperture */

static void stop_clears_pending_banks(NV2AState *d)
{
    nv2a_mmio_write(d, PVIDEO + NV_PVIDEO_BUFFER,
                    NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE, 4);
    nv2a_mmio_write(d, PVIDEO + NV_PVIDEO_STOP, 1, 4);
    CHECK(nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_BUFFER, 4) == 0,
          "BUFFER kept %08llX after STOP",
          (unsigned long long)nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_BUFFER, 4));
    CHECK(nv2a_mmio_read(d, PVIDEO + NV_PVIDEO_STOP, 4) == 0,
          "STOP was stored");
}

int main(void)
{
    static uint8_t vram[0x10000], ramin[0x10000];
    NV2AState *d;

    d = nv2a_init_standalone(vram, sizeof(vram), ramin, sizeof(ramin));
    if (!d) { fprintf(stderr, "no NV2A model\n"); return 2; }
    stop_clears_pending_banks(d);

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: PVIDEO STOP clears the pending banks.");
    return 0;
}
