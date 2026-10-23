/////////////////////////////////////////////////////////////////////////////
//
// spu - Top-level SPU emulation, for SPU and SPU2
//
/////////////////////////////////////////////////////////////////////////////

#ifndef __PSX_SPU_H__
#define __PSX_SPU_H__

#include "emuconfig.h"
#include "spucore.h"

#ifdef __cplusplus
extern "C" {
#endif

sint32 EMU_CALL spu_init(void);
/* version = 1 for PS1, 2 for PS2 */
uint32 EMU_CALL spu_get_state_size(uint8 version);
void   EMU_CALL spu_clear_state(void *state, uint8 version);

void   EMU_CALL spu_render    (void *state, sint16 *buf, uint32 samples);
void   EMU_CALL spu_render_ext(void *state, sint16 *buf, sint16 *ext, uint32 samples);
uint16 EMU_CALL spu_lh(void *state, uint32 a);

/* Read a voice register (SPUREG_VOICE_*) for the mdx2wav visualizer. */
uint32 EMU_CALL spu_getreg_voice(void *state, uint32 voice, uint32 n);
/* Read a main SPU register (SPUREG_*) for the mdx2wav visualizer. */
uint32 EMU_CALL spu_getreg_main(void *state, uint32 n);
uint32 EMU_CALL spu_getreg_flag(void *state, uint32 n);
/* Pointer to the 512 KB SPU RAM (16-bit words). */
uint16* EMU_CALL spu_get_ram(void *state);

/* Per-voice output sink (see spucore.h). */
void   EMU_CALL spu_set_wave_sink(void *state, SPUCORE_WAVE_SINK sink, void *user);
uint32 EMU_CALL spu_dbg_voice_addr(void *state, uint32 voice);

/* Per-voice captured output (ring buffer of 2048 samples). */
int16_t const* EMU_CALL spu_dbg_waveform(void *state, uint32 voice);
uint32 EMU_CALL spu_dbg_wave_pos(void *state, uint32 voice);
void   EMU_CALL spu_sh(void *state, uint32 a, uint16 d);

void   EMU_CALL spu_dma(void *state, uint32 core, void *mem, uint32 mem_ofs, uint32 mem_mask, uint32 bytes, int iswrite);

uint32 EMU_CALL spu_cycles_until_interrupt(void *state, uint32 samples);

/*
** Enable/disable main or reverb
*/
void EMU_CALL spu_enable_main(void *state, uint8 enable);
void EMU_CALL spu_enable_reverb(void *state, uint8 enable);

#ifdef __cplusplus
}
#endif

#endif
