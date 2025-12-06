#define MXDRVG_EXPORT
#define MXDRVG_CALLBACK

volatile unsigned char OpmReg1B;  // OPM レジスタ $1B の内容

#include "mxdrvg_core.h"

// Visualizer support: Get/Set OPM_Delegate implementation
OPM_Delegate* MXDRVG_GetOPMDelegate(void) {
	return OPM;
}

void MXDRVG_SetOPMDelegate(OPM_Delegate* delegate) {
	if (delegate) {
		OPM = delegate;
	}
}

// Visualizer support: Get/Set PCM8 implementation
X68K::X68PCM8* MXDRVG_GetPCM8(void) {
	return PCM8;
}

void MXDRVG_SetPCM8(X68K::X68PCM8* pcm8) {
	if (pcm8) {
        printf("MXDRVG_SetPCM8: Setting new PCM8 instance %p\n", pcm8);
		PCM8 = pcm8;
	}
}

// Visualizer support: Get ADPCM buffer info
void* MXDRVG_GetADPCMBuffer(void) {
	return (void*)G.L001e38;
}

unsigned int MXDRVG_GetADPCMBufferSize(void) {
	return G.L002224;
}

