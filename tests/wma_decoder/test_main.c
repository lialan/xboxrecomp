#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wma_decoder.h"

static int is_cleared(const XboxWmaPcm *pcm)
{
    return pcm->data == NULL && pcm->size == 0 && pcm->sample_rate == 0 &&
           pcm->channels == 0 && pcm->bits_per_sample == 0;
}

int main(void)
{
    XboxWmaPcm pcm;

    /* Even argument-validation failures must honor the public failure contract
     * and clear a caller-provided result structure. */
    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file(NULL, &pcm) != -1) {
        fprintf(stderr, "NULL input unexpectedly decoded\n");
        return 1;
    }
    if (!is_cleared(&pcm)) {
        fprintf(stderr, "NULL-path failure did not leave a cleared result\n");
        return 1;
    }

    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file("__xboxrecomp_missing_wma_file__.wma", &pcm) != -1) {
        fprintf(stderr, "missing input unexpectedly decoded\n");
        return 1;
    }

    if (!is_cleared(&pcm)) {
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
