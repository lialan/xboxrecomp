#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wma_decoder.h"

int main(void)
{
    XboxWmaPcm pcm;
    memset(&pcm, 0xA5, sizeof(pcm));

    if (xbox_wma_decode_file("__xboxrecomp_missing_wma_file__.wma", &pcm) != -1) {
        fprintf(stderr, "missing input unexpectedly decoded\n");
        return 1;
    }

    if (pcm.data != NULL || pcm.size != 0 || pcm.sample_rate != 0 ||
        pcm.channels != 0 || pcm.bits_per_sample != 0) {
        fprintf(stderr, "failed decode did not leave a cleared result\n");
        return 1;
    }

    /* Cleanup must remain idempotent after a failed decode. */
    xbox_wma_pcm_free(&pcm);
    xbox_wma_pcm_free(&pcm);
    if (pcm.data != NULL || pcm.size != 0) {
        fprintf(stderr, "cleanup did not remain zeroed\n");
        return 1;
    }

    return 0;
}
