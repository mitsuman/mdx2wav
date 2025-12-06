#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#include "../types.h"
#include "global.h"
#include "pcm8.h"

namespace X68K
{
	typedef MXDRVG_SAMPLETYPE Sample;
	typedef int32 ISample;

	class X68PCM8
	{
	public:
		X68PCM8();
		~X68PCM8() {}

		virtual bool Init(uint rate);
		virtual bool SetRate(uint rate);
		virtual void Reset();

		virtual int Out(int ch, void *adrs, int mode, int len);
		virtual void Abort();

	virtual void Mix(Sample *buffer, int nsamples);
	virtual void SetVolume(int db);
	virtual void SetChannelMask(uint mask);
	
	// Visualizer用: Pcm8チャンネルへのアクセス
	const Pcm8& GetPcm8Channel(int ch) const { return mPcm8[ch & (PCM8_NCH - 1)]; }
	
	// Visualizer用: チャンネルごとの波形バッファを取得
	const int16_t* GetChannelWaveform(int ch) const { return channel_waveform_[ch & (PCM8_NCH - 1)]; }
	int GetChannelWaveformPos(int ch) const { return channel_waveform_pos_[ch & (PCM8_NCH - 1)]; }

protected:
	Pcm8 mPcm8[PCM8_NCH];
		uint mMask;
		int mVolume;
		int mSampleRate;
		
		// Visualizer用: 各チャンネルの波形リングバッファ (256サンプル)
		int16_t channel_waveform_[PCM8_NCH][256];
		int channel_waveform_pos_[PCM8_NCH];

		sint32 OutInpAdpcm[2];
		sint32 OutInpAdpcm_prev[2];
		sint32 OutInpAdpcm_prev2[2];
		sint32 OutOutAdpcm[2];
		sint32 OutOutAdpcm_prev[2];
		sint32 OutOutAdpcm_prev2[2];  // 高音フィルター２用バッファ

		sint32 OutInpOutAdpcm[2];
		sint32 OutInpOutAdpcm_prev[2];
		sint32 OutInpOutAdpcm_prev2[2];
		sint32 OutOutInpAdpcm[2];
		sint32 OutOutInpAdpcm_prev[2];  // 高音フィルター３用バッファ

		inline void pcmset62500(Sample* buffer, int ndata);
		inline void pcmset22050(Sample* buffer, int ndata);

	};

	inline int Max(int x, int y) { return (x > y) ? x : y; }
	inline int Min(int x, int y) { return (x < y) ? x : y; }
	inline int Abs(int x) { return x >= 0 ? x : -x; }

	inline int Limit(int v, int max, int min) 
	{ 
		return v > max ? max : (v < min ? min : v); 
	}

	inline void StoreSample(Sample& dest, ISample data)
	{
		if (sizeof(Sample) == 2)
			dest = (Sample) Limit(dest + data, 0x7fff, -0x8000);
		else
			dest += data;
	}

}

// [EOF]
