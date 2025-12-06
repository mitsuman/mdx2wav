#ifndef VISUALIZER_H
#define VISUALIZER_H

#include "ym2151_state.h"

// 前方宣言
struct SDL_Window;
struct SDL_Renderer;
struct _TTF_Font;

// Layout constants
static const int KEYBOARD_START_X = 240;
static const int WHITE_KEY_WIDTH = 10;
static const int OCTAVE_COUNT = 58;  // 表示する白鍵の数 (C0-C8 = 57-58 keys)
static const int WAVEFORM_OFFSET = 5;  // 鍵盤と波形の間隔
static const int CHANNEL_WAVEFORM_X = KEYBOARD_START_X + OCTAVE_COUNT * WHITE_KEY_WIDTH + WAVEFORM_OFFSET;  // 825
static const int CHANNEL_WAVEFORM_WIDTH = 200;

// Utility functions (forward declaration using void* to avoid SDL_ttf.h dependency)
void renderText(SDL_Renderer* renderer, void* font, const char* text, 
               int x, int y, int r, int g, int b);

class Visualizer {
public:
    Visualizer();
    ~Visualizer();
    
    // 初期化
    bool init(const char* title, int width, int height);
    
    // 状態オブジェクトを設定
    void setState(YM2151State* state);
    
    // OPMチップポインタを設定（Timer制御用）
    void setOPMPointer(void* opm_ptr) { opm_ptr_ = opm_ptr; }
    
    // OPMVisualizerラッパーを設定（キーボード演奏用）
    void setOPMWrapper(void* opm_wrapper) { opm_wrapper_ = opm_wrapper; }
    
    // PCMデータを渡す（波形表示用）
    void updateWaveform(const short* samples, int count);
    
    // イベント処理 & 描画
    void update();
    
    // 終了
    void shutdown();
    
    // 実行中かどうか
    bool isRunning() const { return running_; }
    
    // スクリーンショット保存（BMPフォーマット）
    bool saveScreenshot(const char* filename);
    
    // スクリーンショット専用モード（保存後に即終了）
    void setScreenshotMode(const char* filename);
    
    // 曲名とファイル名の設定
    void setSongTitle(const char* title);
    void setFilename(const char* filename);
    
    // ファイルリストの設定
    void setFileList(const char** files, int count, int current_index);
    
    // ファイル切り替え要求があるか確認
    bool hasFileChangeRequest() const { return file_change_requested_; }
    int getRequestedFileIndex() const { return requested_file_index_; }
    void clearFileChangeRequest() { file_change_requested_ = false; }
    
    // 再生時間をリセット
    void resetElapsedTime() { elapsed_time_ = 0.0; }
    
    // MXDRVG一時停止状態を取得
    bool isMXDRVGPaused() const { return mxdrvg_paused_; }
    
    // YM2151ミュート状態を取得
    bool isYM2151Muted() const { return ym2151_muted_; }

private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    YM2151State* state_;
    void* opm_ptr_;  // OPMチップへのポインタ（Timer制御用）
    void* opm_wrapper_;  // OPMVisualizerラッパー（キーボード演奏用）
    bool running_;
    bool initialized_;
    
    // フレームカウンター
    unsigned int frame_count_;
    bool screenshot_taken_;
    
    // スクリーンショットモード
    bool screenshot_mode_;
    const char* screenshot_filename_;
    
    // フォント（前方宣言用にvoid*を使用）
    void* font_small_;
    void* font_medium_;
    void* font_large_;
    void* font_japanese_;  // 日本語フォント
    
    // ビットマップフォント
    void* bitmap_font_texture_;  // SDL_Texture*
    int bitmap_char_width_;
    int bitmap_char_height_;
    int bitmap_chars_per_row_;
    
    // 曲名とファイル名
    char song_title_[256];
    char filename_[256];
    
    // 再生時間管理
    double elapsed_time_;  // 秒単位の経過時間
    
    // ファイルリスト管理
    const char** file_list_;
    int file_list_count_;
    int current_file_index_;
    bool file_change_requested_;
    int requested_file_index_;
    
    // MXDRVG一時停止フラグ
    bool mxdrvg_paused_;
    
    // YM2151レジスタ書き込みブロックフラグ
    bool ym2151_muted_;
    
    // キーボード演奏用の選択チャンネル
    int selected_channel_;
    
    // キーボード演奏用のオクターブオフセット
    int octave_offset_;
    
    // ポリフォニックモード
    bool polyphonic_mode_;
    
    // チャンネルごとのキーオン状態管理
    struct ChannelKeyState {
        bool active;           // チャンネルが使用中か
        int midi_note;         // 現在のMIDIノート番号
        unsigned int key_on_time;  // キーオン時のフレーム番号
    };
    ChannelKeyState channel_keys_[8];
    
    // ADPCMチャンネルのピーク管理
    struct ADPCMPeakInfo {
        uint8_t peak_volume;       // ピーク音量
        uint32_t keyon_time;       // キーオン時のフレーム数
        uint32_t last_length;      // 最後のLEN値
        uintptr_t start_address;   // 再生開始アドレス（起点）
        uintptr_t end_address;     // 再生終了アドレス（起点+length）
    };
    ADPCMPeakInfo adpcm_peaks_[8];
    
    // 描画メソッド
    void renderTitle();
    void renderTimerInfo();
    void renderChannelInfo();
    void renderADPCMChannels(int& y);
    void renderKeyboard();
    void renderWaveform();
    void renderAlgorithmDiagram(int x, int y, int algorithm, const YM2151State::Channel& channel);
    void renderLFOWaveform(int x, int y, int waveform, int r, int g, int b);
    void draw7Segment(int x, int y, int digit, int r, int g, int b, int seg_width, int seg_height);
    void renderBitmapText(const char* text, int x, int y, int r, int g, int b);
    void renderChannelWaveform(int x, int y, int width, int height, const int16_t* waveform_data, int sample_count, int color_r, int color_g, int color_b);
    void getColorFromAddress(uintptr_t address, int& r, int& g, int& b);
    
    // ヘルパー
    void getChannelColor(int ch, int& r, int& g, int& b);
    void getEGPhaseColor(int phase, int& r, int& g, int& b);
    const char* getNoteName(int note);
    
    // キーボード演奏
    void triggerNote(int midi_note, bool key_on);
    void enterPolyphonicMode();
    int findChannelForNote(int midi_note, bool key_on);
    void copyChannelRegisters(int src_ch, int dst_ch);
};

#endif // VISUALIZER_H
