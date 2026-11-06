#ifndef LITEFX_CRYSTALIZER_ZITA_H
#define LITEFX_CRYSTALIZER_ZITA_H

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CrystalizerZita CrystalizerZita;

CrystalizerZita *crystalizer_zita_create(uint32_t sample_rate, uint32_t blocksize);
void crystalizer_zita_destroy(CrystalizerZita *cz);
void crystalizer_zita_reconfigure(CrystalizerZita *cz, uint32_t sample_rate, uint32_t blocksize);
void crystalizer_zita_reset_state(CrystalizerZita *cz);

void crystalizer_zita_process(CrystalizerZita *cz,
                              const float *in_l, const float *in_r,
                              float *out_l, float *out_r,
                              uint32_t n_samples,
                              const float *intensities_db,
                              const bool *bypasses,
                              const bool *mutes,
                              bool adaptive);

#ifdef __cplusplus
}
#endif

#endif /* LITEFX_CRYSTALIZER_ZITA_H */
