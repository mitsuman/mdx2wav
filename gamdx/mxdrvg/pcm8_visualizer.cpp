#include "pcm8_visualizer.h"
#include "../../src/visualizer/ym2151_state.h"

PCM8Visualizer::PCM8Visualizer(YM2151State* state)
    : state_(state) {
}

PCM8Visualizer::~PCM8Visualizer() {
}

bool PCM8Visualizer::Init(uint rate) {
    //printf("PCM8Visualizer::Init called with rate %u\n", rate);
    return X68K::X68PCM8::Init(rate);
}

bool PCM8Visualizer::SetRate(uint rate) {
    return X68K::X68PCM8::SetRate(rate);
}

void PCM8Visualizer::Reset() {
    X68K::X68PCM8::Reset();
}

int PCM8Visualizer::Out(int ch, void *adrs, int mode, int len) {
    // チャンネル番号を正規化
    int actual_ch = ch & 7;  // 0-7
    
    // 実際の再生処理
    int result = X68K::X68PCM8::Out(ch, adrs, mode, len);
    
    // 状態更新: Out()が呼ばれた = キーオン
    if (state_ && actual_ch < 8) {
        // Pcm8オブジェクトから詳細な情報を取得
        const X68K::Pcm8& pcm = GetPcm8Channel(actual_ch);
        
        uint8_t volume = pcm.GetVolume();
        uint32_t rate = pcm.GetAdpcmRate();
        uintptr_t address = reinterpret_cast<uintptr_t>(adrs);
        uint32_t length = len;
        uint8_t mode_val = mode;
        
        // Out()はキーオンの開始なのでis_initial_keyon=trueを渡す
        state_->updateADPCMChannel(actual_ch, true, volume, rate, address, length, mode_val, true);
    }
    
    return result;
}

void PCM8Visualizer::Abort() {
    X68K::X68PCM8::Abort();
}

void PCM8Visualizer::SetChannelMask(uint mask) {
    X68K::X68PCM8::SetChannelMask(mask);
}

void PCM8Visualizer::SetVolume(int db) {
    X68K::X68PCM8::SetVolume(db);
}

void PCM8Visualizer::Mix(X68K::Sample* buffer, int ndata) {
    // 実際の合成処理（親クラスの実装を呼ぶ - ここで各チャンネルの波形がリングバッファに記録される）
    X68K::X68PCM8::Mix(buffer, ndata);
    
    // 各チャンネルの状態を更新
    if (state_) {
        for (int ch = 0; ch < 8; ch++) {
            const X68K::Pcm8& pcm = GetPcm8Channel(ch);
            
            // GetRest()が0の場合は再生完了 = キーオフ
            bool is_playing = (pcm.GetRest() > 0);
            
            if (!is_playing) {
                state_->updateADPCMChannel(ch, false, pcm.GetVolume(), pcm.GetAdpcmRate(), 0, 0, pcm.GetMode(), false);
            } else {
                state_->updateADPCMChannel(ch, true, pcm.GetVolume(), pcm.GetAdpcmRate(), 
                                          reinterpret_cast<uintptr_t>(pcm.GetDmaMar()), 
                                          pcm.GetDmaMtc(), pcm.GetMode(), false);
            }
            
            // リングバッファから最新の256サンプルを取得
            const int16_t* ring_buffer = GetChannelWaveform(ch);
            int current_pos = GetChannelWaveformPos(ch);
            int16_t channel_waveform[256];
            
            // リングバッファの現在位置から逆算して最新256サンプルを取得
            // 現在位置が256なので、256サンプル前から取得（1周分）
            for (int i = 0; i < 256; i++) {
                // リングバッファの位置計算：現在位置から256サンプル戻る
                int ring_pos = (current_pos - 256 + i) & 255;
                channel_waveform[i] = ring_buffer[ring_pos];
            }
            
            // 波形データを更新
            state_->updateADPCMWaveform(ch, channel_waveform, 256);
        }
    }
}
