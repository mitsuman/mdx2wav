#ifndef __YM3438_H__
#define __YM3438_H__

#include "../../stdtype.h"
#include "../snddef.h"

void nukedopn2_write(void *chip, UINT8 port, UINT8 data);
/* mdx2wav visualizer support (per-channel output, captured at the output rate) */
#ifdef __cplusplus
extern "C" {
#endif
const INT16* nukedopn2_vis_wave(int ch);
UINT32 nukedopn2_vis_pos(int ch);
const INT16* nukedopn2_vis_dac(void);
UINT32 nukedopn2_vis_dac_pos(void);
int nukedopn2_vis_dac_enabled(void);
void nukedopn2_vis_state(void* chip, int ch, int* fnum, int* block, int* alg, int* fb,
                         int* panl, int* panr, int* egout, int* kon);
#ifdef __cplusplus
}
#endif
UINT8 nukedopn2_read(void *chip, UINT8 port);
void nukedopn2_update(void *chip, UINT32 numsamples, DEV_SMPL **sndptr);
void nukedopn2_set_options(void *chip, UINT32 flags);
void nukedopn2_set_mute_mask(void *chip, UINT32 mute);
void* nukedopn2_init(UINT32 clock, UINT32 rate);
void nukedopn2_shutdown(void *chip);
void nukedopn2_reset_chip(void *chip);

#endif	// __YM3438_H__
