#ifndef YM2151_STATE_H
#define YM2151_STATE_H

#include <stdint.h>
#include <pthread.h>

// YM2151の8チャンネル分の状態を管理
class YM2151State
{
public:
    struct Operator
    {
        uint8_t total_level;   // TL (0-127)
        uint8_t key_scale;     // KS (0-3)
        uint8_t multiple;      // MUL (0-15)
        uint8_t detune1;       // DT1 (0-7)
        uint8_t detune2;       // DT2 (0-3)
        uint8_t attack_rate;   // AR (0-31)
        uint8_t decay_rate;    // D1R (0-31)
        uint8_t sustain_rate;  // D2R (0-31)
        uint8_t release_rate;  // RR (0-15)
        uint8_t sustain_level; // D1L (0-15)

        // エンベロープ状態（fmgenから取得）
        int eg_level; // EGレベル (0-4095, 大きいほど小さい音)
        int eg_phase; // EGフェーズ (0=next, 1=attack, 2=decay, 3=sustain, 4=release, 5=off)
        int eg_out;   // EG出力値
        bool key_on;  // キーオン状態

        // ビジュアライザー用のピーク表示
        int eg_out_peak; // EG出力のピーク値
        bool was_key_on; // 前回のキーオン状態（キーオン検出用）
    };

    struct LFOState
    {
        uint32_t lfo_count;   // LFOカウンタ (0-511)
        uint8_t lfo_freq;     // LFO周波数 (0-255)
        uint8_t lfo_waveform; // LFO波形 (0=saw, 1=square, 2=triangle, 3=noise)
        uint8_t amd;          // AM深度 (0-127)
        uint8_t pmd;          // PM深度 (0-127)
    };

    struct TimerState
    {
        uint16_t timer_a;      // Timer A period (0-1023)
        uint8_t timer_b;       // Timer B period (0-255)
        int32_t timer_a_count; // Timer A counter
        int32_t timer_b_count; // Timer B counter
        bool timer_a_enable;   // Timer A enabled
        bool timer_b_enable;   // Timer B enabled
        uint8_t status;        // Status register
    };

    struct ADPCMChannel
    {
        bool key_on;               // 再生中かどうか
        uint8_t volume;            // 音量 (0-15)
        uint32_t rate;             // サンプリングレート
        uintptr_t initial_address; // 初回キーオン時のアドレス（色の決定に使用）
        uintptr_t address;         // 現在の再生アドレス（ポインタサイズ）
        uint32_t length;           // データ長
        uint8_t mode;              // モード (ADPCMの種類など)
        int16_t waveform[256];     // 波形データ
    };

    struct Channel
    {
        bool key_on;           // キーオン状態
        uint8_t note;          // 音程 (MIDI note number 0-127)
        uint8_t key_code;      // KC (0-127)
        uint8_t key_fraction;  // KF (0-63)
        uint8_t velocity;      // 音量 (0-127, キャリアのTLから計算)
        uint8_t algorithm;     // アルゴリズム (0-7)
        uint8_t feedback;      // フィードバック (0-7)
        uint8_t operator_mask; // どのオペレータが有効か (bit0-3)
        uint8_t left_right;    // L/R出力 (bit7=L, bit6=R)
        uint8_t ams;           // AM Sense (0-3)
        uint8_t pms;           // PM Sense (0-7)
        uint8_t noise_enable;  // ノイズ有効 (0=off, 1=on)
        uint8_t noise_freq;    // ノイズ周波数 (0-31)
        Operator operators[4]; // 4つのオペレータ (M1, C1, M2, C2)

        // ビジュアライザー用のエンベロープ表示
        float display_volume;          // 表示用の音量 (0.0-1.0, 減衰していく)
        float peak_volume;             // ピーク音量 (0.0-1.0)
        uint64_t last_update_time;     // 最後の更新時刻（ミリ秒）
        uint32_t register_writes;      // レジスタ書き込み量 (バイト数)
        uint32_t register_writes_peak; // レジスタ書き込み量のピーク
        uint32_t peak_hold_frames;     // ピークホールド残りフレーム数
    };

    YM2151State();
    ~YM2151State();

    // レジスタ更新 (SetRegから呼ばれる)
    void updateRegister(uint8_t addr, uint8_t data);

    // チャンネル情報の取得（スレッドセーフ）
    void getChannelInfo(int ch, Channel &out);

    // 全チャンネル情報の取得（スレッドセーフ）
    void getAllChannels(Channel out[8]);

    // ADPCMチャンネル情報の取得（スレッドセーフ）
    void getADPCMChannel(int ch, ADPCMChannel &out);
    void getAllADPCMChannels(ADPCMChannel out[8]);

    // ADPCMチャンネル情報の更新
    void updateADPCMChannel(int ch, bool key_on, uint8_t volume, uint32_t rate, uintptr_t address, uint32_t length, uint8_t mode, bool is_initial_keyon = false);

    // ADPCM波形の更新
    void updateADPCMWaveform(int ch, const int16_t *waveform, int size);

    // ADPCM全体バッファ情報の設定
    void setADPCMBufferInfo(void *buffer, uint32_t size);

    // ADPCM全体バッファ情報の取得
    void *getADPCMBuffer() const { return adpcm_buffer_; }
    uint32_t getADPCMBufferSize() const { return adpcm_buffer_size_; }

    // LFO状態の取得（スレッドセーフ）
    void getLFOState(LFOState &out);

    // Timer状態の取得（スレッドセーフ）
    void getTimerState(TimerState &out);

    // Timer Bのトグル（opm_ptrはOPMチップへのポインタ）
    void toggleTimerB(void *opm_ptr);

    // チャンネル波形の取得（スレッドセーフ）
    static const int CHANNEL_WAVEFORM_SIZE = 800;
    void getChannelWaveform(int ch, int16_t *out, int size);

    // レジスタ書き込みカウンタのリセット（フレーム毎に呼ぶ）
    void resetRegisterWriteCounts();

    // リセット
    void reset();

    // fmgenから直接EG情報を更新（OPMポインタを受け取る）
    void updateFromFmgen(void *opm_ptr);

private:
    Channel channels_[8];
    ADPCMChannel adpcm_channels_[8];
    LFOState lfo_state_;
    TimerState timer_state_;
    uint8_t registers_[256]; // 全レジスタの生データ
    pthread_mutex_t mutex_;  // スレッドセーフのためのミューテックス

    // ADPCM全体バッファ情報
    void *adpcm_buffer_;
    uint32_t adpcm_buffer_size_;

    // チャンネル波形バッファ（各チャンネル800サンプル）
    int16_t channel_waveforms_[8][800];
    int channel_waveform_pos_[8];  // 未使用（互換性のため残す）

    // ノート番号を計算 (KC/KF -> MIDI note)
    void updateNote(int ch);

    // 音量を計算（キャリアオペレータのTLから）
    void updateVolume(int ch);

    // アルゴリズムに基づいてキャリアオペレータを判定
    bool isCarrier(int algorithm, int op);
};

#endif // YM2151_STATE_H
