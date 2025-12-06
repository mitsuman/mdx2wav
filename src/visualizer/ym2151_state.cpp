#include "ym2151_state.h"
#include "../../gamdx/fmgen/opm.h"
#include <cstring>
#include <algorithm>
#include <sys/time.h>

// 波形バッファサイズの定義
const int YM2151State::CHANNEL_WAVEFORM_SIZE;

// 現在時刻をミリ秒で取得
static uint64_t getTimeMillis() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// YM2151のアルゴリズムごとのキャリアオペレータ判定テーブル
// アルゴリズム0-7, オペレータ0-3(M1,C1,M2,C2)
static const bool carrier_table[8][4] = {
    // ALG  M1     C1     M2     C2
    {false, false, false, true },  // 0: M1->M2->C1->C2
    {false, false, false, true },  // 1: M1+M2->C1->C2
    {false, false, false, true },  // 2: M1+(M2->C1)->C2
    {false, true,  false, true },  // 3: (M1->M2)+C1->C2
    {false, true,  false, true },  // 4: (M1->C1)+(M2->C2)
    {false, true,  true,  true },  // 5: M1->(C1+C2+M2)
    {false, true,  true,  true },  // 6: (M1->C1)+C2+M2
    {true,  true,  true,  true },  // 7: C1+C2+M2+M1
};

YM2151State::YM2151State() 
    : adpcm_buffer_(nullptr)
    , adpcm_buffer_size_(0) {
    pthread_mutex_init(&mutex_, nullptr);
    reset();
}

YM2151State::~YM2151State() {
    pthread_mutex_destroy(&mutex_);
}

void YM2151State::reset() {
    pthread_mutex_lock(&mutex_);
    memset(&channels_, 0, sizeof(channels_));
    memset(&adpcm_channels_, 0, sizeof(adpcm_channels_));
    memset(registers_, 0, sizeof(registers_));
    memset(channel_waveforms_, 0, sizeof(channel_waveforms_));
    memset(channel_waveform_pos_, 0, sizeof(channel_waveform_pos_));
    pthread_mutex_unlock(&mutex_);
}

bool YM2151State::isCarrier(int algorithm, int op) {
    if (algorithm < 0 || algorithm > 7 || op < 0 || op > 3) {
        return false;
    }
    return carrier_table[algorithm][op];
}

void YM2151State::updateNote(int ch) {
    if (ch < 0 || ch >= 8) return;
    
    uint8_t kc = channels_[ch].key_code;
    uint8_t kf = channels_[ch].key_fraction;
    
    // YM2151のKC/KFからMIDIノート番号への変換
    // KC: 上位3bit=オクターブ(0-7), 下位4bit=ノート(0-15)
    int octave = (kc >> 4) & 0x07;

    // C#から始まるのと欠番があるのに注意
    int kc_table[] = {
        1, 2, 3, 4, // C# ... 3->欠番
        4, 5, 6, 7, // 7->欠番
        7, 8, 9,10, // 11->欠番
       10,11,12,12}; // 15->欠番
    int note_in_octave = kc_table[kc & 0x0F];
    
    // KFによる微調整（省略：より正確にするならKFも考慮）
    
    // MIDIノート番号 (C4 = 60)
    // YM2151のオクターブ4をMIDIのオクターブ4(60)に合わせる
    int midi_note = (octave - 1) * 12 + note_in_octave + 12;

    // X68は3.58MHzで動かすべきところを4.00MHzで動かしているので実際より高い音が出ている
    // Cを440Hzにするために2セミトーン下げる補正を入れる
    midi_note += 2;
    
    // 範囲チェック
    channels_[ch].note = std::max(0, std::min(127, midi_note));
}

void YM2151State::updateVolume(int ch) {
    if (ch < 0 || ch >= 8) return;
    
    int algorithm = channels_[ch].algorithm;
    
    // キャリアオペレータのTLの平均を音量とする
    int total = 0;
    int count = 0;
    
    for (int op = 0; op < 4; op++) {
        if (isCarrier(algorithm, op)) {
            // TLは値が小さいほど音量が大きい (0=最大, 127=最小)
            int tl = channels_[ch].operators[op].total_level;
            total += (127 - tl);  // 反転して0-127に
            count++;
        }
    }
    
    if (count > 0) {
        channels_[ch].velocity = total / count;
    } else {
        channels_[ch].velocity = 0;
    }
}

void YM2151State::updateRegister(uint8_t addr, uint8_t data) {
    pthread_mutex_lock(&mutex_);
    
    registers_[addr] = data;
    
    // チャンネル固有のレジスタの場合、そのチャンネルの書き込みカウンタを増やす
    int target_ch = -1;
    if (addr == 0x08) {
        // KEY ON/OFF
        target_ch = data & 0x07;
    } else if (addr >= 0x20 && addr <= 0x3F) {
        // RL/FB/CON, KC, KF, PMS/AMS
        target_ch = addr & 0x07;
    } else if (addr >= 0x40 && addr <= 0xFF) {
        // オペレータ関連レジスタ
        target_ch = addr & 0x07;
    }
    
    if (target_ch >= 0 && target_ch < 8) {
        channels_[target_ch].register_writes++;
    }
    
    // レジスタアドレスで分岐して状態を更新
    if (addr == 0x0F) {
        // NOISE ENABLE/FREQ (bit7=NE, bit4-0=NFRQ)
        // only for channel 7 (slot32)
        channels_[7].noise_enable = (data >> 7) & 0x01;
        channels_[7].noise_freq = data & 0x1F;
        
    } else if (addr == 0x08) {
        // KEY ON/OFF (bit0-2=ch, bit3-6=slots)
        int ch = data & 0x07;
        int slots = (data >> 3) & 0x0F;
        bool new_key_on = (slots != 0);
        bool was_key_on = channels_[ch].key_on;
        
        channels_[ch].key_on = new_key_on;
        channels_[ch].operator_mask = slots;
        
        // キーオン時にピークを記録
        if (new_key_on && !was_key_on) {
            float vel = channels_[ch].velocity / 127.0f;
            channels_[ch].peak_volume = vel;
            channels_[ch].display_volume = vel;
            channels_[ch].last_update_time = getTimeMillis();
        }
        // キーオフ時は時刻だけ更新
        else if (!new_key_on && was_key_on) {
            channels_[ch].last_update_time = getTimeMillis();
        }
        
    } else if (addr >= 0x20 && addr <= 0x27) {
        // RL/FB/CON (Connection/Algorithm)
        int ch = addr - 0x20;
        channels_[ch].left_right = data & 0xC0;
        channels_[ch].feedback = (data >> 3) & 0x07;
        channels_[ch].algorithm = data & 0x07;
        updateVolume(ch);
        
    } else if (addr >= 0x28 && addr <= 0x2F) {
        // KC (Key Code)
        int ch = addr - 0x28;
        channels_[ch].key_code = data & 0x7F;
        updateNote(ch);
        
    } else if (addr >= 0x30 && addr <= 0x37) {
        // KF (Key Fraction)
        int ch = addr - 0x30;
        channels_[ch].key_fraction = (data >> 2) & 0x3F;
        updateNote(ch);
        
    } else if (addr >= 0x38 && addr <= 0x3F) {
        // PMS/AMS
        int ch = addr - 0x38;
        channels_[ch].pms = (data >> 4) & 0x07;
        channels_[ch].ams = data & 0x03;
        
    } else if (addr >= 0x40 && addr <= 0x5F) {
        // DT1/MUL (Detune1/Multiple)
        int slot = (addr - 0x40) >> 3;
        int ch = (addr - 0x40) & 0x07;
        int op = slot;  // slot 0-3 = op M1,C1,M2,C2
        if (op < 4) {
            channels_[ch].operators[op].detune1 = (data >> 4) & 0x07;
            channels_[ch].operators[op].multiple = data & 0x0F;
        }
        
    } else if (addr >= 0x60 && addr <= 0x7F) {
        // TL (Total Level)
        int slot = (addr - 0x60) >> 3;
        int ch = (addr - 0x60) & 0x07;
        int op = slot;
        if (op < 4) {
            channels_[ch].operators[op].total_level = data & 0x7F;
            updateVolume(ch);
        }
        
    } else if (addr >= 0x80 && addr <= 0x9F) {
        // KS/AR (Key Scale/Attack Rate)
        int slot = (addr - 0x80) >> 3;
        int ch = (addr - 0x80) & 0x07;
        int op = slot;
        if (op < 4) {
            channels_[ch].operators[op].key_scale = (data >> 6) & 0x03;
            channels_[ch].operators[op].attack_rate = data & 0x1F;
        }
        
    } else if (addr >= 0xA0 && addr <= 0xBF) {
        // AMS-EN/D1R (Decay Rate 1)
        int slot = (addr - 0xA0) >> 3;
        int ch = (addr - 0xA0) & 0x07;
        int op = slot;
        if (op < 4) {
            channels_[ch].operators[op].decay_rate = data & 0x1F;
        }
        
    } else if (addr >= 0xC0 && addr <= 0xDF) {
        // DT2/D2R (Detune2/Decay Rate 2)
        int slot = (addr - 0xC0) >> 3;
        int ch = (addr - 0xC0) & 0x07;
        int op = slot;
        if (op < 4) {
            channels_[ch].operators[op].detune2 = (data >> 6) & 0x03;
            channels_[ch].operators[op].sustain_rate = data & 0x1F;
        }
        
    } else if (addr >= 0xE0 && addr <= 0xFF) {
        // D1L/RR (Sustain Level/Release Rate)
        int slot = (addr - 0xE0) >> 3;
        int ch = (addr - 0xE0) & 0x07;
        int op = slot;
        if (op < 4) {
            channels_[ch].operators[op].sustain_level = (data >> 4) & 0x0F;
            channels_[ch].operators[op].release_rate = data & 0x0F;
        }
    }
    
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getChannelInfo(int ch, Channel& out) {
    if (ch < 0 || ch >= 8) return;
    
    pthread_mutex_lock(&mutex_);
    out = channels_[ch];
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getAllChannels(Channel out[8]) {
    pthread_mutex_lock(&mutex_);
    memcpy(out, channels_, sizeof(channels_));
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getLFOState(LFOState& out) {
    pthread_mutex_lock(&mutex_);
    out = lfo_state_;
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getTimerState(TimerState& out) {
    pthread_mutex_lock(&mutex_);
    out = timer_state_;
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::toggleTimerB(void* opm_ptr) {
    if (!opm_ptr) return;
    
    pthread_mutex_lock(&mutex_);
    // レジスタ0x14のBit1を反転
    registers_[0x14] ^= 0x02;
    uint8_t new_value = registers_[0x14];
    // 状態を更新
    timer_state_.timer_b_enable = (new_value & 0x02) != 0;
    pthread_mutex_unlock(&mutex_);
    
    // YM2151チップに実際に書き込む
    FM::OPM* opm = static_cast<FM::OPM*>(opm_ptr);
    opm->SetReg(0x14, new_value);
}

void YM2151State::getADPCMChannel(int ch, ADPCMChannel& out) {
    if (ch < 0 || ch >= 8) return;
    
    pthread_mutex_lock(&mutex_);
    out = adpcm_channels_[ch];
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getAllADPCMChannels(ADPCMChannel out[8]) {
    pthread_mutex_lock(&mutex_);
    memcpy(out, adpcm_channels_, sizeof(adpcm_channels_));
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::updateADPCMChannel(int ch, bool key_on, uint8_t volume, uint32_t rate, uintptr_t address, uint32_t length, uint8_t mode, bool is_initial_keyon) {
    if (ch < 0 || ch >= 8) return;
    
    pthread_mutex_lock(&mutex_);
    adpcm_channels_[ch].key_on = key_on;
    adpcm_channels_[ch].volume = volume;
    adpcm_channels_[ch].rate = rate;
    
    // is_initial_keyonがtrueの場合は初回アドレスを更新（Out()呼び出し時）
    if (is_initial_keyon) {
        adpcm_channels_[ch].initial_address = address;
    }
    // 現在の再生アドレスは常に更新
    adpcm_channels_[ch].address = address;
    
    adpcm_channels_[ch].length = length;
    adpcm_channels_[ch].mode = mode;
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::setADPCMBufferInfo(void* buffer, uint32_t size) {
    pthread_mutex_lock(&mutex_);
    adpcm_buffer_ = buffer;
    adpcm_buffer_size_ = size;
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::updateADPCMWaveform(int ch, const int16_t* waveform, int size) {
    if (ch < 0 || ch >= 8 || !waveform || size <= 0) return;
    
    pthread_mutex_lock(&mutex_);
    int copy_size = std::min(size, 200);
    memcpy(adpcm_channels_[ch].waveform, waveform, copy_size * sizeof(int16_t));
    // 残りをゼロクリア
    if (copy_size < 200) {
        memset(adpcm_channels_[ch].waveform + copy_size, 0, (200 - copy_size) * sizeof(int16_t));
    }
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::getChannelWaveform(int ch, int16_t* out, int size) {
    if (ch < 0 || ch >= 8 || !out || size <= 0) return;
    
    pthread_mutex_lock(&mutex_);
    
    int copy_size = std::min(size, CHANNEL_WAVEFORM_SIZE);
    int pos = channel_waveform_pos_[ch];
    
    // 最新のデータから古いデータへ、リングバッファとして読み出す
    for (int i = 0; i < copy_size; i++) {
        int idx = (pos - copy_size + i + CHANNEL_WAVEFORM_SIZE) % CHANNEL_WAVEFORM_SIZE;
        out[i] = channel_waveforms_[ch][idx];
    }
    
    pthread_mutex_unlock(&mutex_);
}

void YM2151State::resetRegisterWriteCounts() {
    pthread_mutex_lock(&mutex_);
    for (int i = 0; i < 8; i++) {
        // ピークを更新
        if (channels_[i].register_writes > channels_[i].register_writes_peak) {
            channels_[i].register_writes_peak = channels_[i].register_writes;
            channels_[i].peak_hold_frames = 60; // 60フレーム（約1秒）保持
        } else if (channels_[i].peak_hold_frames > 0) {
            channels_[i].peak_hold_frames--;
            if (channels_[i].peak_hold_frames == 0) {
                channels_[i].register_writes_peak = 0;
            }
        }
        
        channels_[i].register_writes = 0;
    }
    pthread_mutex_unlock(&mutex_);
}

#ifdef ENABLE_VISUALIZER
// fmgenのインクルードは実装ファイルのみ
#include "../../gamdx/fmgen/opm.h"

void YM2151State::updateFromFmgen(void* opm_ptr) {
    if (!opm_ptr) return;
    
    FM::OPM* opm = static_cast<FM::OPM*>(opm_ptr);
    
    pthread_mutex_lock(&mutex_);
    
    // LFO状態を更新
    lfo_state_.lfo_count = opm->GetLFOCount();
    lfo_state_.lfo_freq = opm->GetLFOFreq();
    lfo_state_.lfo_waveform = opm->GetLFOWaveform();
    lfo_state_.amd = opm->GetAMD();
    lfo_state_.pmd = opm->GetPMD();
    
    // Timer状態を更新（レジスタから読み取り）
    timer_state_.timer_a = ((registers_[0x10] & 0x03) << 8) | registers_[0x11];
    timer_state_.timer_b = registers_[0x12];
    timer_state_.timer_a_enable = (registers_[0x14] & 0x01) != 0;
    timer_state_.timer_b_enable = (registers_[0x14] & 0x02) != 0;
    timer_state_.status = registers_[0x14];  // Control register
    timer_state_.timer_a_count = 0;  // カウンタは外部から取得不可
    timer_state_.timer_b_count = 0;
    
    // OPMから各チャンネルの波形を取得（MixSubで記録済み）
    for (int ch = 0; ch < 8; ch++) {
        opm->dbgGetChannelWaveform(ch, channel_waveforms_[ch], CHANNEL_WAVEFORM_SIZE);
        // 位置はOPM側で管理されているので、最新データを取得するだけ
    }
    
    for (int ch = 0; ch < 8; ch++) {
        FM::Channel4* fmch = opm->dbgGetCh(ch);
        if (!fmch) continue;
        
        // 各オペレータのEG情報を取得
        for (int op = 0; op < 4; op++) {
            auto& src_op = fmch->op[op];
            auto& dst_op = channels_[ch].operators[op];
            
            bool current_key_on = src_op.dbgIsKeyOn();
            dst_op.eg_level = src_op.dbgGetEGLevel();
            dst_op.eg_phase = src_op.dbgGetEGPhase();
            dst_op.eg_out = src_op.dbgGetEGOut();
            
            // キーオン検出時にピークをリセット
            if (current_key_on && !dst_op.was_key_on) {
                dst_op.eg_out_peak = dst_op.eg_out;
            }
            // キーオン中はピークを更新（小さい値=大きい音）
            else if (current_key_on && dst_op.eg_out < dst_op.eg_out_peak) {
                dst_op.eg_out_peak = dst_op.eg_out;
            }
            
            dst_op.key_on = current_key_on;
            dst_op.was_key_on = current_key_on;
        }
    }
    
    pthread_mutex_unlock(&mutex_);
}
#else
void YM2151State::updateFromFmgen(void* opm_ptr) {
    // Visualizer無効時は何もしない
}
#endif
