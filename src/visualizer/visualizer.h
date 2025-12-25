#ifndef VISUALIZER_H
#define VISUALIZER_H

#include "ym2151_state.h"
#include "spectrum_analyzer.h"
#include <cstdio>

// 前方宣言
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Surface;
struct _TTF_Font;
class VideoEncoder;

// Layout constants
static const int YM2151_START_Y = 73;  // YM2151チャンネル表示の開始Y座標（タイトル+Timer情報分のスペース）
static const int YM2151_LINE_HEIGHT = 70;  // 各チャンネルの高さ
static const int KEYBOARD_START_X = 240;
static const int WHITE_KEY_WIDTH = 10;
static const int OCTAVE_COUNT = 58;  // 表示する白鍵の数 (C0-C8 = 57-58 keys)
static const int WAVEFORM_OFFSET = 5;  // 鍵盤と波形の間隔
static const int CHANNEL_WAVEFORM_X = KEYBOARD_START_X + OCTAVE_COUNT * WHITE_KEY_WIDTH + WAVEFORM_OFFSET;  // 825
static const int CHANNEL_WAVEFORM_WIDTH = 256;

// Channel label colors (shared by YM2151 and ADPCM)
static const int CHANNEL_LABEL_COLOR_ACTIVE_R = 255;
static const int CHANNEL_LABEL_COLOR_ACTIVE_G = 140;
static const int CHANNEL_LABEL_COLOR_ACTIVE_B = 0;
static const int CHANNEL_LABEL_COLOR_INACTIVE_R = 120;
static const int CHANNEL_LABEL_COLOR_INACTIVE_G = 60;
static const int CHANNEL_LABEL_COLOR_INACTIVE_B = 0;

// Utility functions (forward declaration using void* to avoid SDL_ttf.h dependency)
void renderText(SDL_Renderer* renderer, void* font, const char* text, 
               int x, int y, int r, int g, int b);

class Visualizer {
public:
    Visualizer();
    ~Visualizer();
    
    // 初期化
    bool init(const char* title, int width, int height);
    
    // 動画録画モードで初期化
    bool initVideoMode(const char* video_filename, int width, int height, int fps, int sample_rate);
    
    // 動画録画モードかどうか
    bool isVideoMode() const { return video_mode_; }
    
    // 状態オブジェクトを設定
    void setState(YM2151State* state);
    
    // OPMチップポインタを設定（Timer制御用）
    void setOPMPointer(void* opm_ptr) { opm_ptr_ = opm_ptr; }
    
    // 動画モード: フレームをレンダリングして動画に追加
    bool renderVideoFrame();
    
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
    
    // 曲の再スタート要求
    bool hasRestartRequest() const { return restart_requested_; }
    void clearRestartRequest() { restart_requested_ = false; }
    
    // 再生時間をリセット
    void resetElapsedTime() { elapsed_time_ = 0.0; }
    
    // MXDRVG一時停止状態を取得
    bool isMXDRVGPaused() const { return mxdrvg_paused_; }
    
    // YM2151ミュート状態を取得
    bool isYM2151Muted() const { return ym2151_muted_; }
    
    // 表示チャンネル数の設定
    void setDisplayChannels(int ym2151_channels, int adpcm_channels) {
        ym2151_display_channels_ = (ym2151_channels > 0 && ym2151_channels <= 8) ? ym2151_channels : 8;
        adpcm_display_channels_ = (adpcm_channels >= 0 && adpcm_channels <= 8) ? adpcm_channels : 8;
    }
    
    // 波形表示のスケール設定
    void setWaveformScale(float ym2151_scale, float adpcm_scale) {
        ym2151_waveform_scale_ = (ym2151_scale > 0.0f && ym2151_scale <= 10.0f) ? ym2151_scale : 1.0f;
        adpcm_waveform_scale_ = (adpcm_scale > 0.0f && adpcm_scale <= 10.0f) ? adpcm_scale : 1.0f;
    }
    
    // スペクトラムデバッグモードの設定
    void setSpectrumDebug(const char* filename);
    
    // キーボード/MIDI演奏
    void triggerNote(int midi_note, bool key_on, int velocity = 127);

private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    YM2151State* state_;
    void* opm_ptr_;  // OPMチップへのポインタ（Timer制御用）
    void* opm_wrapper_;  // OPMVisualizerラッパー（キーボード演奏用）
    bool running_;
    bool initialized_;
    
    // 動画録画モード
    bool video_mode_;
    VideoEncoder* video_encoder_;
    SDL_Surface* offscreen_surface_;
    int video_width_;
    int video_height_;
    int video_fps_;
    
    // MIDI入力
    uint32_t midi_client_;  // MIDIClientRef (CoreMIDI)
    uint32_t midi_port_;    // MIDIPortRef (CoreMIDI)
    
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
    bool restart_requested_;
    
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
        uint8_t original_tl[4];    // 各オペレータのオリジナルTL値（未使用）
        uint8_t preset_tl[4];      // プリセット/音色から読み込んだベースTL値
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
    
    // 表示チャンネル数
    int ym2151_display_channels_;
    int adpcm_display_channels_;
    float ym2151_waveform_scale_;  // YM2151波形表示の振幅スケール (1.0 = デフォルト)
    float adpcm_waveform_scale_;   // ADPCM波形表示の振幅スケール (1.0 = デフォルト)
    
    // 描画メソッド
    void renderTitle();
    void renderTimerInfo();
    void renderChannelInfo();
    void renderADPCMChannels(int& y);
    void renderKeyboard();
    void renderWaveform();
    void renderSpectrum();  // スペクトラムアナライザ描画
    void renderAlgorithmDiagram(int x, int y, int algorithm, const YM2151State::Channel& channel);
    void renderLFOWaveform(int x, int y, int waveform, int r, int g, int b);
    void draw7Segment(int x, int y, int digit, int r, int g, int b, int seg_width, int seg_height);
    void renderBitmapText(const char* text, int x, int y, int r, int g, int b);
    void renderChannelWaveform(int x, int y, int width, int height, const int16_t* waveform_data, int sample_count, int color_r, int color_g, int color_b, int ch_id, float waveform_scale = 1.0f);
    int findStableWaveformOffset(const int16_t* waveform_data, int sample_count, int width, int ch);
    void getColorFromAddress(uintptr_t address, int& r, int& g, int& b);
    
    // 前フレームの波形データ（位相安定化用）
    static const int MAX_CHANNELS = 16;  // YM2151(8) + ADPCM(8)
    int16_t prev_waveform_[MAX_CHANNELS][200];
    int prev_waveform_offset_[MAX_CHANNELS];
    bool has_prev_waveform_[MAX_CHANNELS];
    
    // スペクトラムアナライザ（チャンネルごと）
    static const int NUM_SPECTRUM_ANALYZERS = 9;  // YM2151(8) + ADPCM(1)
    SpectrumAnalyzer* spectrum_analyzers_[NUM_SPECTRUM_ANALYZERS];
    
    // スペクトラムデバッグ
    FILE* spectrum_debug_file_;
    int spectrum_debug_frame_count_;
    
    // 初期化/クリーンアップヘルパー
    bool initCommon();       // TTF、フォント読み込みなど共通の初期化
    void cleanupCommon();    // フォント、TTFのクリーンアップ
    
    // ヘルパー
    void getChannelColor(int ch, int& r, int& g, int& b);
    void getEGPhaseColor(int phase, int& r, int& g, int& b);
    const char* getNoteName(int note);
    
    // キーボード演奏（内部用）
    void enterPolyphonicMode();
    int findChannelForNote(int midi_note, bool key_on);
    void copyChannelRegisters(int src_ch, int dst_ch);
    
    // プリセット保存/読み込み
    bool saveChannelPreset(int preset_num);
    bool loadChannelPreset(int preset_num);
    
    // MIDI入力
    bool initMIDI();
    void shutdownMIDI();
    static void midiInputCallback(void* message, void* refCon);
};

#endif // VISUALIZER_H
