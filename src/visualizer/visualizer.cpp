#include "visualizer.h"
#include "video_encoder.h"
#include "../../gamdx/fmgen/opm.h"
#include "../../gamdx/mxdrvg/opm_visualizer.h"

#include <SDL2/SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sys/time.h>
#include <iconv.h>

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#endif

// 波形表示用のバッファサイズ
static const int WAVEFORM_BUFFER_SIZE = 2048;

// 内部実装用の構造体
struct VisualizerImpl {
    std::vector<float> waveform_left;
    std::vector<float> waveform_right;
    int waveform_pos;
    
    VisualizerImpl() : waveform_pos(0) {
        waveform_left.resize(WAVEFORM_BUFFER_SIZE, 0.0f);
        waveform_right.resize(WAVEFORM_BUFFER_SIZE, 0.0f);
    }
};

static VisualizerImpl* impl = nullptr;

Visualizer::Visualizer()
    : window_(nullptr)
    , renderer_(nullptr)
    , state_(nullptr)
    , opm_ptr_(nullptr)
    , opm_wrapper_(nullptr)
    , running_(false)
    , initialized_(false)
    , video_mode_(false)
    , video_encoder_(nullptr)
    , offscreen_surface_(nullptr)
    , video_width_(0)
    , video_height_(0)
    , video_fps_(0)
    , frame_count_(0)
    , screenshot_taken_(false)
    , screenshot_mode_(false)
    , screenshot_filename_(nullptr)
    , font_small_(nullptr)
    , font_medium_(nullptr)
    , font_large_(nullptr)
    , font_japanese_(nullptr)
    , bitmap_font_texture_(nullptr)
    , bitmap_char_width_(6)
    , bitmap_char_height_(6)
    , bitmap_chars_per_row_(16)
    , elapsed_time_(0.0)
    , file_list_(nullptr)
    , file_list_count_(0)
    , current_file_index_(0)
    , file_change_requested_(false)
    , requested_file_index_(0)
    , restart_requested_(false)
    , mxdrvg_paused_(false)
    , ym2151_muted_(false)
    , selected_channel_(0)
    , octave_offset_(-1)
    , polyphonic_mode_(false)
    , midi_client_(0)
    , midi_port_(0)
    , ym2151_display_channels_(8)
    , adpcm_display_channels_(8)
    , ym2151_waveform_scale_(1.0f)
    , adpcm_waveform_scale_(1.0f)
    , spectrum_debug_file_(nullptr)
    , spectrum_debug_frame_count_(0) {
    song_title_[0] = '\0';
    filename_[0] = '\0';
    
    // スペクトラムアナライザの初期化
    for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
        spectrum_analyzers_[i] = nullptr;
    }
    
    // チャンネルキー状態の初期化
    for (int i = 0; i < 8; i++) {
        channel_keys_[i].active = false;
        channel_keys_[i].midi_note = -1;
        channel_keys_[i].key_on_time = 0;
    }
    
    // 前フレーム波形データの初期化
    for (int i = 0; i < MAX_CHANNELS; i++) {
        memset(prev_waveform_[i], 0, sizeof(prev_waveform_[i]));
        prev_waveform_offset_[i] = 0;
        has_prev_waveform_[i] = false;
    }
}

Visualizer::~Visualizer() {
    shutdown();
}

bool Visualizer::initCommon() {
    if (TTF_Init() == -1) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }

    const char* font_paths[] = {
        "data/6x6-pixel-font.otf",
        "../data/6x6-pixel-font.otf",
        "data/8-bit-6x6-nostalgia.otf",
        "../data/8-bit-6x6-nostalgia.otf",
        "/System/Library/Fonts/Supplemental/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
        nullptr
    };
    const char* font_path = nullptr;
    for (int i = 0; font_paths[i] != nullptr; i++) {
        FILE* f = fopen(font_paths[i], "r");
        if (f) {
            fclose(f);
            font_path = font_paths[i];
            fprintf(stderr, "Using font: %s\n", font_path);
            break;
        }
    }
    
    if (font_path) {
        font_small_ = TTF_OpenFont(font_path, 14);
        font_medium_ = TTF_OpenFont(font_path, 18);
        font_large_ = TTF_OpenFont(font_path, 24);
        
        if (!font_small_ || !font_medium_ || !font_large_) {
            fprintf(stderr, "Warning: Failed to load font: %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "Warning: No suitable font found\n");
    }
    
    // ビットマップフォントの読み込み (96x96, 16x16 grid, ASCII 0-255)
    const char* bitmap_font_paths[] = {
        "data/5x5_bitmap_font_96.png",
        "../data/5x5_bitmap_font_96.png",
        nullptr
    };
    
    for (int i = 0; bitmap_font_paths[i] != nullptr; i++) {
        SDL_Surface* surface = IMG_Load(bitmap_font_paths[i]);
        if (surface) {
            bitmap_font_texture_ = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_FreeSurface(surface);
            
            if (bitmap_font_texture_) {
                fprintf(stderr, "Loaded bitmap font: %s\n", bitmap_font_paths[i]);
                break;
            }
        }
    }
    
    if (!bitmap_font_texture_) {
        fprintf(stderr, "Warning: Failed to load bitmap font\n");
    }
    
    // 日本語フォントの読み込み（モノスペースフォントを優先）
    const char* japanese_font_paths[] = {
        "data/KH-Dot-Kagurazaka-16.ttf",
        "../data/KH-Dot-Kagurazaka-16.ttf",
        "data/KH-Dot-Akihabara-16.ttf",
        "../data/KH-Dot-Akihabara-16.ttf",
        "/System/Library/Fonts/Osaka.ttf",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
        "/System/Library/Fonts/Supplemental/Monaco.dfont",
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
        nullptr
    };
    
    for (int i = 0; japanese_font_paths[i] != nullptr; i++) {
        FILE* f = fopen(japanese_font_paths[i], "r");
        if (f) {
            fclose(f);
            font_japanese_ = TTF_OpenFont(japanese_font_paths[i], 20);
            if (font_japanese_) {
                fprintf(stderr, "Using Japanese font: %s\n", japanese_font_paths[i]);
                break;
            }
        }
    }
    
    if (!font_japanese_) {
        fprintf(stderr, "Warning: Failed to load Japanese font\n");
    }
    
    // タイトルの初期値
    snprintf(song_title_, sizeof(song_title_), "YM2151 Visualizer");
    
    return true;
}

// 共通のクリーンアップ処理
void Visualizer::cleanupCommon() {
    if (bitmap_font_texture_) {
        SDL_DestroyTexture((SDL_Texture*)bitmap_font_texture_);
        bitmap_font_texture_ = nullptr;
    }
    if (font_small_) {
        TTF_CloseFont((TTF_Font*)font_small_);
        font_small_ = nullptr;
    }
    if (font_medium_) {
        TTF_CloseFont((TTF_Font*)font_medium_);
        font_medium_ = nullptr;
    }
    if (font_large_) {
        TTF_CloseFont((TTF_Font*)font_large_);
        font_large_ = nullptr;
    }
    if (font_japanese_) {
        TTF_CloseFont((TTF_Font*)font_japanese_);
        font_japanese_ = nullptr;
    }
    TTF_Quit();
}

bool Visualizer::init(const char* title, int width, int height) {
    if (initialized_) {
        return true;
    }
    
    // SDL初期化
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    
    // ウィンドウ作成
    window_ = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN
    );
    
    if (!window_) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    
    // レンダラー作成
    renderer_ = SDL_CreateRenderer(
        window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    
    if (!renderer_) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }
    
    impl = new VisualizerImpl();
    
    // スペクトラムアナライザを初期化（チャンネルごと）
    for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
        spectrum_analyzers_[i] = new SpectrumAnalyzer();
    }
    
    // 共通の初期化処理を呼び出し
    if (!initCommon()) {
        for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
            delete spectrum_analyzers_[i];
            spectrum_analyzers_[i] = nullptr;
        }
        delete impl;
        impl = nullptr;
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }
    
    // MIDI入力を初期化（リアルタイムモードのみ）
    if (initMIDI()) {
        fprintf(stderr, "MIDI input initialized\n");
    }
    
    initialized_ = true;
    running_ = true;
    
    return true;
}

bool Visualizer::initVideoMode(const char* video_filename, int width, int height, int fps, int sample_rate) {
    if (initialized_) {
        return true;
    }
    
    video_mode_ = true;
    video_width_ = width;
    video_height_ = height;
    video_fps_ = fps;
    
    // SDL初期化（オフスクリーンレンダリング用）
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    
    // オフスクリーンサーフェスを作成（動画フレームバッファ）
    // 32ビットRGBA形式で作成（SDL_CreateSoftwareRendererはRGBA形式が必要）
    offscreen_surface_ = SDL_CreateRGBSurface(0, width, height, 32, 
                                              0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!offscreen_surface_) {
        fprintf(stderr, "Failed to create offscreen surface: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    
    // オフスクリーンサーフェス用のレンダラーを作成
    renderer_ = SDL_CreateSoftwareRenderer(offscreen_surface_);
    if (!renderer_) {
        fprintf(stderr, "Failed to create software renderer: %s\n", SDL_GetError());
        SDL_FreeSurface(offscreen_surface_);
        SDL_Quit();
        return false;
    }
    
    impl = new VisualizerImpl();
    
    // スペクトラムアナライザを初期化（チャンネルごと）
    for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
        spectrum_analyzers_[i] = new SpectrumAnalyzer();
    }
    
    // 共通の初期化処理を呼び出し
    if (!initCommon()) {
        for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
            delete spectrum_analyzers_[i];
            spectrum_analyzers_[i] = nullptr;
        }
        delete impl;
        impl = nullptr;
        SDL_DestroyRenderer(renderer_);
        SDL_FreeSurface(offscreen_surface_);
        SDL_Quit();
        return false;
    }
    
    // VideoEncoderを初期化
    video_encoder_ = new VideoEncoder();
    if (!video_encoder_->init(video_filename, width, height, fps, sample_rate, 2)) {
        fprintf(stderr, "Failed to initialize video encoder: %s\n", video_encoder_->getError());
        delete video_encoder_;
        video_encoder_ = nullptr;
        cleanupCommon();
        delete impl;
        impl = nullptr;
        SDL_DestroyRenderer(renderer_);
        SDL_FreeSurface(offscreen_surface_);
        SDL_Quit();
        return false;
    }
    
    initialized_ = true;
    running_ = true;
    
    fprintf(stderr, "Video mode initialized: %s (%dx%d @ %dfps)\n", 
            video_filename, width, height, fps);
    
    return true;
}

void Visualizer::setState(YM2151State* state) {
    state_ = state;
}

void Visualizer::setSpectrumDebug(const char* filename) {
    if (spectrum_debug_file_) {
        fclose(spectrum_debug_file_);
    }
    spectrum_debug_file_ = fopen(filename, "w");
    spectrum_debug_frame_count_ = 0;
    if (spectrum_debug_file_) {
        fprintf(stderr, "Spectrum debug output enabled: %s\n", filename);
    }
}

void Visualizer::updateWaveform(const short* samples, int count) {
    if (!impl || !samples) return;
    
    // 動画モードの場合は音声エンコーダーに送る
    if (video_mode_ && video_encoder_) {
        video_encoder_->addAudioSamples(samples, count);
    }
    
    // チャンネルごとにスペクトラムアナライザにデータを渡す
    if (opm_ptr_) {
        FM::OPM* opm = (FM::OPM*)opm_ptr_;
        
        // YM2151の各チャンネル（0-7）の波形を取得
        for (int ch = 0; ch < 8; ch++) {
            if (spectrum_analyzers_[ch]) {
                int16_t channel_waveform_mono[1024];
                opm->dbgGetChannelWaveform(ch, channel_waveform_mono, 1024);
                
                // モノラルをステレオに変換（processAudio はステレオを期待）
                int16_t channel_waveform_stereo[2048];
                for (int i = 0; i < 1024; i++) {
                    channel_waveform_stereo[i * 2] = channel_waveform_mono[i];      // L
                    channel_waveform_stereo[i * 2 + 1] = channel_waveform_mono[i];  // R
                }
                
                spectrum_analyzers_[ch]->processAudio(channel_waveform_stereo, 1024);
            }
        }
    }
    
    // ADPCM用（各チャンネルの波形を個別に解析）
    if (state_) {
        YM2151State::ADPCMChannel adpcm_channels[8];
        state_->getAllADPCMChannels(adpcm_channels);
        const int waveform_samples = sizeof(adpcm_channels[0].waveform) / sizeof(int16_t);
        int16_t channel_waveform_stereo[waveform_samples * 2];
        for (int ch = 0; ch < 8; ch++) {
            SpectrumAnalyzer* analyzer = spectrum_analyzers_[8 + ch];
            if (!analyzer) {
                continue;
            }
            for (int i = 0; i < waveform_samples; i++) {
                int16_t sample = adpcm_channels[ch].waveform[i];
                channel_waveform_stereo[i * 2] = sample;
                channel_waveform_stereo[i * 2 + 1] = sample;
            }
            analyzer->processAudio(channel_waveform_stereo, waveform_samples);
        }
    }
    
    // サンプルをダウンサンプリングして波形バッファに格納
    int stride = std::max(1, count / (WAVEFORM_BUFFER_SIZE / 4));
    
    for (int i = 0; i < count && impl->waveform_pos < WAVEFORM_BUFFER_SIZE; i += stride) {
        impl->waveform_left[impl->waveform_pos] = samples[i * 2] / 32768.0f;
        impl->waveform_right[impl->waveform_pos] = samples[i * 2 + 1] / 32768.0f;
        impl->waveform_pos++;
    }
    
    // バッファが一杯になったらリセット
    if (impl->waveform_pos >= WAVEFORM_BUFFER_SIZE) {
        impl->waveform_pos = 0;
    }
}

void Visualizer::update() {
    if (!initialized_ || !running_) {
        return;
    }
    
    // イベント処理
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            running_ = false;
            return;
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                running_ = false;
                return;
            } else if (event.key.keysym.sym == SDLK_UP && file_list_count_ > 0) {
                // 前のファイルへ
                int new_index = current_file_index_ - 1;
                if (new_index < 0) new_index = file_list_count_ - 1;
                requested_file_index_ = new_index;
                file_change_requested_ = true;
                return;
            } else if (event.key.keysym.sym == SDLK_DOWN && file_list_count_ > 0) {
                // 次のファイルへ
                int new_index = (current_file_index_ + 1) % file_list_count_;
                requested_file_index_ = new_index;
                file_change_requested_ = true;
                return;
            } else if (event.key.keysym.sym == SDLK_x) {
                // MXDRVGの一時停止/再開（レジスタ書き込みのみ停止、PCM生成は継続）
                mxdrvg_paused_ = !mxdrvg_paused_;
                fprintf(stderr, "MXDRVG %s\n", mxdrvg_paused_ ? "PAUSED" : "RESUMED");
            } else if (event.key.keysym.sym == SDLK_c) {
                // 曲の頭から再生
                restart_requested_ = true;
                fprintf(stderr, "Restarting song from beginning\n");
                return;
            } else if (event.key.keysym.sym == SDLK_b && state_ && opm_ptr_) {
                // Timer Bをトグル
                state_->toggleTimerB(opm_ptr_);
                YM2151State::TimerState timer;
                state_->getTimerState(timer);
                fprintf(stderr, "Timer B %s\n", timer.timer_b_enable ? "ON" : "OFF");
            } else if (event.key.keysym.sym == SDLK_m) {
                // YM2151レジスタアクセスをブロック/解除
                ym2151_muted_ = !ym2151_muted_;
                fprintf(stderr, "YM2151 %s\n", ym2151_muted_ ? "MUTED" : "UNMUTED");
                
                // ミュート時は全チャンネルにキーオフを送信
                if (ym2151_muted_ && opm_wrapper_) {
                    OPMVisualizer* wrapper = (OPMVisualizer*)opm_wrapper_;
                    for (int ch = 0; ch < 8; ch++) {
                        wrapper->SetRegDirect(0x08, ch & 0x07);  // キーオフ (オペレータマスク = 0)
                        // ポリフォニックモードの場合、チャンネル状態もリセット
                        if (polyphonic_mode_) {
                            channel_keys_[ch].active = false;
                        }
                    }
                }
            } else if (event.key.keysym.sym >= SDLK_F1 && event.key.keysym.sym <= SDLK_F8) {
                // F1-F8: プリセット保存/読み込み
                int preset_num = event.key.keysym.sym - SDLK_F1 + 1;
                if (ym2151_muted_) {
                    // ミュート時は読み込み
                    if (loadChannelPreset(preset_num)) {
                        fprintf(stderr, "Loaded preset %d to all channels\n", preset_num);
                    } else {
                        fprintf(stderr, "Failed to load preset %d\n", preset_num);
                    }
                } else {
                    // 非ミュート時は保存
                    if (saveChannelPreset(preset_num)) {
                        fprintf(stderr, "Saved channel %d to preset %d\n", selected_channel_, preset_num);
                    } else {
                        fprintf(stderr, "Failed to save preset %d\n", preset_num);
                    }
                }
            } else if (event.key.keysym.sym == SDLK_F9) {
                // ポリフォニックモード切り替え
                polyphonic_mode_ = !polyphonic_mode_;
                if (polyphonic_mode_) {
                    enterPolyphonicMode();
                }
                fprintf(stderr, "Polyphonic mode %s\n", polyphonic_mode_ ? "ON" : "OFF");
            } else if (event.key.keysym.sym >= SDLK_0 && event.key.keysym.sym <= SDLK_7) {
                // チャンネル選択 (0-7)
                selected_channel_ = event.key.keysym.sym - SDLK_0;
                fprintf(stderr, "Selected channel: %d\n", selected_channel_);
            } else if (event.key.keysym.sym == SDLK_COMMA) {
                // オクターブダウン
                if (octave_offset_ > -5) octave_offset_--;
                fprintf(stderr, "Octave offset: %d\n", octave_offset_);
            } else if (event.key.keysym.sym == SDLK_PERIOD) {
                // オクターブアップ
                if (octave_offset_ < 3) octave_offset_++;
                fprintf(stderr, "Octave offset: %d\n", octave_offset_);
            } else if (event.key.keysym.sym == SDLK_a) {
                triggerNote(60, true); // C4
            } else if (event.key.keysym.sym == SDLK_w) {
                triggerNote(61, true); // C#4
            } else if (event.key.keysym.sym == SDLK_s) {
                triggerNote(62, true); // D4
            } else if (event.key.keysym.sym == SDLK_e) {
                triggerNote(63, true); // D#4
            } else if (event.key.keysym.sym == SDLK_d) {
                triggerNote(64, true); // E4
            } else if (event.key.keysym.sym == SDLK_f) {
                triggerNote(65, true); // F4
            } else if (event.key.keysym.sym == SDLK_t) {
                triggerNote(66, true); // F#4
            } else if (event.key.keysym.sym == SDLK_g) {
                triggerNote(67, true); // G4
            } else if (event.key.keysym.sym == SDLK_y) {
                triggerNote(68, true); // G#4
            } else if (event.key.keysym.sym == SDLK_h) {
                triggerNote(69, true); // A4
            } else if (event.key.keysym.sym == SDLK_u) {
                triggerNote(70, true); // A#4
            } else if (event.key.keysym.sym == SDLK_j) {
                triggerNote(71, true); // B4
            } else if (event.key.keysym.sym == SDLK_k) {
                triggerNote(72, true); // C5
            } else if (event.key.keysym.sym == SDLK_o) {
                triggerNote(73, true); // C#5
            } else if (event.key.keysym.sym == SDLK_l) {
                triggerNote(74, true); // D5
            } else if (event.key.keysym.sym == SDLK_p) {
                triggerNote(75, true); // D#5
            } else if (event.key.keysym.sym == SDLK_SEMICOLON) {
                triggerNote(76, true); // E5
            } else if (event.key.keysym.sym == SDLK_QUOTE) {
                triggerNote(77, true); // F5
            } else if (event.key.keysym.sym == SDLK_BACKSLASH) {
                triggerNote(79, true); // G5
            }
        } else if (event.type == SDL_KEYUP) {
            // キーリリース時はキーオフ
            if (event.key.keysym.sym == SDLK_a) {
                triggerNote(60, false);
            } else if (event.key.keysym.sym == SDLK_w) {
                triggerNote(61, false);
            } else if (event.key.keysym.sym == SDLK_s) {
                triggerNote(62, false);
            } else if (event.key.keysym.sym == SDLK_e) {
                triggerNote(63, false);
            } else if (event.key.keysym.sym == SDLK_d) {
                triggerNote(64, false);
            } else if (event.key.keysym.sym == SDLK_f) {
                triggerNote(65, false);
            } else if (event.key.keysym.sym == SDLK_t) {
                triggerNote(66, false);
            } else if (event.key.keysym.sym == SDLK_g) {
                triggerNote(67, false);
            } else if (event.key.keysym.sym == SDLK_y) {
                triggerNote(68, false);
            } else if (event.key.keysym.sym == SDLK_h) {
                triggerNote(69, false);
            } else if (event.key.keysym.sym == SDLK_u) {
                triggerNote(70, false);
            } else if (event.key.keysym.sym == SDLK_j) {
                triggerNote(71, false);
            } else if (event.key.keysym.sym == SDLK_k) {
                triggerNote(72, false);
            } else if (event.key.keysym.sym == SDLK_o) {
                triggerNote(73, false);
            } else if (event.key.keysym.sym == SDLK_l) {
                triggerNote(74, false);
            } else if (event.key.keysym.sym == SDLK_p) {
                triggerNote(75, false);
            } else if (event.key.keysym.sym == SDLK_SEMICOLON) {
                triggerNote(76, false);
            } else if (event.key.keysym.sym == SDLK_QUOTE) {
                triggerNote(77, false);
            } else if (event.key.keysym.sym == SDLK_BACKSLASH) {
                triggerNote(79, false);
            }
        }
    }
    
    // 背景クリア（濃い青紫）
    SDL_SetRenderDrawColor(renderer_, 10, 10, 40, 255);
    SDL_RenderClear(renderer_);
    
    // 再生時間を更新（60FPS想定）
    elapsed_time_ += 1.0 / 60.0;
    
    // 各パーツを描画
    renderTitle();
    renderTimerInfo();
    renderChannelInfo();
    renderKeyboard();
    renderSpectrum();
    //renderWaveform();
    
    // 画面更新
    SDL_RenderPresent(renderer_);
    
    // レジスタ書き込みカウンタをリセット（次のフレーム用）
    state_->resetRegisterWriteCounts();
    
    // フレームカウンター
    frame_count_++;
    
    // スクリーンショットモード（コマンドラインオプションで指定された場合のみ）
    if (screenshot_mode_ && !screenshot_taken_ && frame_count_ >= 180) {
        if (saveScreenshot(screenshot_filename_)) {
            fprintf(stderr, "Screenshot saved to: %s\n", screenshot_filename_);
        } else {
            fprintf(stderr, "Failed to save screenshot\n");
        }
        screenshot_taken_ = true;
        running_ = false; // 即終了
        return;
    }
}

bool Visualizer::renderVideoFrame() {
    if (!initialized_ || !video_mode_ || !running_) {
        return false;
    }
    
    if (!state_ || !video_encoder_) {
        return false;
    }
    
    // 背景クリア（濃い青紫）
    SDL_SetRenderDrawColor(renderer_, 10, 10, 40, 255);
    SDL_RenderClear(renderer_);
    
    // 再生時間を更新（動画モード時はフレーム数から計算）
    elapsed_time_ = frame_count_ / (double)video_fps_;
    
    // 各種描画（通常モードと同じ）
    renderTitle();
    renderTimerInfo();
    renderChannelInfo();
    renderKeyboard();
    renderSpectrum();
    
    // レンダラーの内容をサーフェスに反映
    SDL_RenderPresent(renderer_);
    
    // RGBピクセルデータを取得
    unsigned char* pixels = (unsigned char*)offscreen_surface_->pixels;
    
    // 動画エンコーダーにフレームを追加
    if (!video_encoder_->addVideoFrame(pixels)) {
        fprintf(stderr, "Failed to add video frame: %s\n", video_encoder_->getError());
        running_ = false;
        return false;
    }
    
    // レジスタ書き込みカウンタをリセット
    state_->resetRegisterWriteCounts();
    
    // フレームカウンター
    frame_count_++;
    
    return true;
}

void Visualizer::shutdown() {
    if (!initialized_) {
        return;
    }
    
    running_ = false;
    
    // 動画エンコーダーをファイナライズ
    if (video_encoder_) {
        video_encoder_->finalize();
        delete video_encoder_;
        video_encoder_ = nullptr;
    }
    
    // オフスクリーンサーフェスを解放
    if (offscreen_surface_) {
        SDL_FreeSurface(offscreen_surface_);
        offscreen_surface_ = nullptr;
    }
    
    // MIDI入力をクリーンアップ
    shutdownMIDI();
    
    // スペクトラムアナライザを削除（チャンネルごと）
    for (int i = 0; i < NUM_SPECTRUM_ANALYZERS; i++) {
        if (spectrum_analyzers_[i]) {
            delete spectrum_analyzers_[i];
            spectrum_analyzers_[i] = nullptr;
        }
    }
    
    // スペクトラムデバッグファイルを閉じる
    if (spectrum_debug_file_) {
        fclose(spectrum_debug_file_);
        spectrum_debug_file_ = nullptr;
    }
    
    if (impl) {
        delete impl;
        impl = nullptr;
    }
    
    // フォントとTTFをクリーンアップ
    cleanupCommon();
    
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    initialized_ = false;
}

bool Visualizer::saveScreenshot(const char* filename) {
    if (!initialized_ || !renderer_) {
        return false;
    }
    
    // レンダラーのサイズを取得
    int width, height;
    SDL_GetRendererOutputSize(renderer_, &width, &height);
    
    // サーフェス作成
    SDL_Surface* surface = SDL_CreateRGBSurface(0, width, height, 32,
                                                 0x00FF0000,
                                                 0x0000FF00,
                                                 0x000000FF,
                                                 0xFF000000);
    if (!surface) {
        fprintf(stderr, "Failed to create surface: %s\n", SDL_GetError());
        return false;
    }
    
    // レンダラーの内容を読み取り
    if (SDL_RenderReadPixels(renderer_, nullptr, surface->format->format,
                             surface->pixels, surface->pitch) != 0) {
        fprintf(stderr, "Failed to read pixels: %s\n", SDL_GetError());
        SDL_FreeSurface(surface);
        return false;
    }
    
    // BMP保存
    if (SDL_SaveBMP(surface, filename) != 0) {
        fprintf(stderr, "Failed to save BMP: %s\n", SDL_GetError());
        SDL_FreeSurface(surface);
        return false;
    }
    
    SDL_FreeSurface(surface);
    return true;
}

void Visualizer::setScreenshotMode(const char* filename) {
    screenshot_mode_ = true;
    screenshot_filename_ = filename;
    frame_count_ = 179; // 次のフレームで保存
}

void Visualizer::setSongTitle(const char* title) {
    if (!title) return;
    
    // Shift_JISかUTF-8に変換
    iconv_t cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd == (iconv_t)-1) {
        // 変換失敗時はそのままコピー
        snprintf(song_title_, sizeof(song_title_), "%s", title);
        return;
    }
    
    char temp_buf[512];
    char* inbuf = (char*)title;
    char* outbuf = temp_buf;
    size_t inbytesleft = strlen(title);
    size_t outbytesleft = sizeof(temp_buf) - 1;
    
    size_t result = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    *outbuf = '\0';  // null終端
    
    iconv_close(cd);
    
    if (result == (size_t)-1) {
        // 変換失敗時は元の文字列を使用
        snprintf(song_title_, sizeof(song_title_), "%s", title);
    } else {
        snprintf(song_title_, sizeof(song_title_), "%s", temp_buf);
    }
    
    // 末尾の空白、\r、\nを削除
    int len = strlen(song_title_);
    while (len > 0 && (song_title_[len - 1] == ' ' || 
                       song_title_[len - 1] == '\r' || 
                       song_title_[len - 1] == '\n' ||
                       song_title_[len - 1] == '\t')) {
        song_title_[len - 1] = '\0';
        len--;
    }
}

void Visualizer::setFilename(const char* filename) {
    if (filename) {
        // ファイル名のみを抽出（パスを除く）
        const char* basename = strrchr(filename, '/');
        if (basename) {
            basename++;
        } else {
            basename = filename;
        }
        snprintf(filename_, sizeof(filename_), "%s", basename);
    }
}

void Visualizer::setFileList(const char** files, int count, int current_index) {
    file_list_ = files;
    file_list_count_ = count;
    current_file_index_ = current_index;
    file_change_requested_ = false;
}

// テキスト描画ヘルパー
// ビットマップフォントでテキストを描画（96x96 PNG、16x16グリッド、ASCII 0-255、6x6/文字）
void Visualizer::renderBitmapText(const char* text, int x, int y, int r, int g, int b) {
    if (!bitmap_font_texture_ || !text) return;
    
    // 色を設定（テクスチャ全体に色を適用）
    SDL_SetTextureColorMod((SDL_Texture*)bitmap_font_texture_, r, g, b);
    
    int cur_x = x;
    for (int i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        
        // ASCII 0-255すべてに対応
        int char_index = c;
        int src_x = (char_index % bitmap_chars_per_row_) * bitmap_char_width_;
        int src_y = (char_index / bitmap_chars_per_row_) * bitmap_char_height_;
        
        SDL_Rect src = {src_x, src_y, bitmap_char_width_, bitmap_char_height_};
        SDL_Rect dst = {cur_x, y, bitmap_char_width_, bitmap_char_height_};
        
        SDL_RenderCopy(renderer_, (SDL_Texture*)bitmap_font_texture_, &src, &dst);
        
        cur_x += bitmap_char_width_ + 1;  // 1ピクセルの間隔
    }
    
    // 色設定をリセット
    SDL_SetTextureColorMod((SDL_Texture*)bitmap_font_texture_, 255, 255, 255);
}

// フィードバック表示を描画するヘルパー関数
static void drawFeedbackIndicator(SDL_Renderer* renderer, SDL_Rect rect, int feedback) {
    if (feedback == 0) return;
    
    // FB強度に応じて黒→赤で色を変化（0-7 → 黒から明るい赤）
    int red = feedback * 32;  // 0, 32, 64, 96, 128, 160, 192, 224
    if (red > 255) red = 255;
    SDL_SetRenderDrawColor(renderer, red, 0, 0, 255);
    
    // ボックスの下に2pxの線を描画
    SDL_RenderDrawLine(renderer, rect.x, rect.y + rect.h, rect.x + rect.w - 1, rect.y + rect.h);
    SDL_RenderDrawLine(renderer, rect.x, rect.y + rect.h + 1, rect.x + rect.w - 1, rect.y + rect.h + 1);
}

// ノイズ表示を描画するヘルパー関数
static void drawNoiseIndicator(SDL_Renderer* renderer, SDL_Rect rect, bool noise_enable, int noise_freq) {
    if (!noise_enable) return;
    
    // ノイズ周波数に応じて青→紫で色を変化（0-31 → 青から紫）
    // 低周波(0) = 青 RGB(100,150,255), 高周波(31) = 紫 RGB(200,100,255)
    float t = noise_freq / 31.0f;  // 0.0 ~ 1.0
    int red = (int)(100 + t * 100);   // 100 -> 200
    int green = (int)(150 - t * 50);  // 150 -> 100
    int blue = 255;                    // 255 (固定)
    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
    
    // ボックスの下に2pxの線を描画
    SDL_RenderDrawLine(renderer, rect.x, rect.y + rect.h, rect.x + rect.w - 1, rect.y + rect.h);
    SDL_RenderDrawLine(renderer, rect.x, rect.y + rect.h + 1, rect.x + rect.w - 1, rect.y + rect.h + 1);
}

// アルゴリズムに基づいてオペレータの出力までの距離を計算
// 戻り値: 0=最終出力に直結, 1-3=段階的に遠い
static int getOperatorOutputDistance(int algorithm, int op) {
    // op: 0=M1, 1=C1, 2=M2, 3=C2
    switch (algorithm) {
        case 0: // M1→C1→M2→C2
            if (op == 3) return 0;      // C2: 直結
            if (op == 2) return 1;      // M2: 1段階
            if (op == 1) return 2;      // C1: 2段階
            if (op == 0) return 3;      // M1: 3段階
            break;
        case 1: // M1→M2→C2, C1 parallel
            if (op == 3 || op == 1) return 0;  // C2, C1: 直結
            if (op == 2) return 1;      // M2: 1段階
            if (op == 0) return 2;      // M1: 2段階
            break;
        case 2: // C1→M2→C2, M1 parallel
            if (op == 3 || op == 0) return 0;  // C2, M1: 直結
            if (op == 2) return 1;      // M2: 1段階
            if (op == 1) return 2;      // C1: 2段階
            break;
        case 3: // M1→C1→C2, M2 parallel
            if (op == 3 || op == 2) return 0;  // C2, M2: 直結
            if (op == 1) return 1;      // C1: 1段階
            if (op == 0) return 2;      // M1: 2段階
            break;
        case 4: // M1→C1, M2→C2
            if (op == 1 || op == 3) return 0;  // C1, C2: 直結
            if (op == 0 || op == 2) return 1;  // M1, M2: 1段階
            break;
        case 5: // M1→C1, M2, C2 (3系統)
            if (op == 1 || op == 2 || op == 3) return 0;  // C1, M2, C2: 直結
            if (op == 0) return 1;      // M1: 1段階
            break;
        case 6: // M1→C1, M2, C2
            if (op == 1 || op == 2 || op == 3) return 0;  // C1, M2, C2: 直結
            if (op == 0) return 1;      // M1: 1段階
            break;
        case 7: // M1, C1, M2, C2 (完全並列)
            return 0;  // 全て直結
    }
    return 0;
}

// アドレス値から色を生成（HSV色空間で色相を変えて同じ明るさの色を生成）
void Visualizer::getColorFromAddress(uintptr_t address, int& r, int& g, int& b) {
    // アドレスをハッシュ化して0-31の範囲に
    uint32_t hash = (uint32_t)(address ^ (address >> 32));
    hash = hash * 2654435761u;  // Knuth's multiplicative hash
    int color_index = hash % 32;
    
    // HSV色空間で色相を変える（彩度と明度は固定）
    float hue = (color_index * 360.0f / 32.0f);  // 0-360度
    float saturation = 0.70f;  // 彩度70%
    float value = 0.85f;        // 明度85%
    
    // HSV to RGB変換
    float c = value * saturation;
    float x = c * (1.0f - fabs(fmod(hue / 60.0f, 2.0f) - 1.0f));
    float m = value - c;
    
    float r1, g1, b1;
    if (hue < 60) {
        r1 = c; g1 = x; b1 = 0;
    } else if (hue < 120) {
        r1 = x; g1 = c; b1 = 0;
    } else if (hue < 180) {
        r1 = 0; g1 = c; b1 = x;
    } else if (hue < 240) {
        r1 = 0; g1 = x; b1 = c;
    } else if (hue < 300) {
        r1 = x; g1 = 0; b1 = c;
    } else {
        r1 = c; g1 = 0; b1 = x;
    }
    
    r = (int)((r1 + m) * 255.0f);
    g = (int)((g1 + m) * 255.0f);
    b = (int)((b1 + m) * 255.0f);
}

// アルゴリズムに基づいてオペレータの接続先を取得
// 戻り値: 接続先オペレータ番号 (0-3), または -1 (OUTに直結)
static int getOperatorTarget(int algorithm, int op) {
    // op: 0=M1, 1=C1, 2=M2, 3=C2
    switch (algorithm) {
        case 0: // M1→C1→M2→C2→OUT
            if (op == 0) return 1;  // M1→C1
            if (op == 1) return 2;  // C1→M2
            if (op == 2) return 3;  // M2→C2
            if (op == 3) return -1; // C2→OUT
            break;
        case 1: // M1→M2→C2→OUT, C1→OUT
            if (op == 0) return 2;  // M1→M2
            if (op == 1) return -1; // C1→OUT
            if (op == 2) return 3;  // M2→C2
            if (op == 3) return -1; // C2→OUT
            break;
        case 2: // C1→M2→C2→OUT, M1→OUT
            if (op == 0) return -1; // M1→OUT
            if (op == 1) return 2;  // C1→M2
            if (op == 2) return 3;  // M2→C2
            if (op == 3) return -1; // C2→OUT
            break;
        case 3: // M1→C1→C2→OUT, M2→OUT
            if (op == 0) return 1;  // M1→C1
            if (op == 1) return 3;  // C1→C2
            if (op == 2) return -1; // M2→OUT
            if (op == 3) return -1; // C2→OUT
            break;
        case 4: // M1→C1→OUT, M2→C2→OUT
            if (op == 0) return 1;  // M1→C1
            if (op == 1) return -1; // C1→OUT
            if (op == 2) return 3;  // M2→C2
            if (op == 3) return -1; // C2→OUT
            break;
        case 5: // M1→C1→OUT, M2→OUT, C2→OUT
            if (op == 0) return 1;  // M1→C1
            if (op == 1) return -1; // C1→OUT
            if (op == 2) return -1; // M2→OUT
            if (op == 3) return -1; // C2→OUT
            break;
        case 6: // M1→C1→OUT, M2→OUT, C2→OUT (same as 5)
            if (op == 0) return 1;  // M1→C1
            if (op == 1) return -1; // C1→OUT
            if (op == 2) return -1; // M2→OUT
            if (op == 3) return -1; // C2→OUT
            break;
        case 7: // M1→OUT, C1→OUT, M2→OUT, C2→OUT (完全並列)
            return -1;
    }
    return -1;
}

// 7セグメントディスプレイの各セグメント定義
// セグメント配置:
void Visualizer::renderLFOWaveform(int x, int y, int waveform, int r, int g, int b) {
    // LFO波形を小さく図示（16x8ピクセル）
    const int w = 16;
    const int h = 8;
    
    SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
    
    switch (waveform) {
        case 0: // Saw wave (ノコギリ波)
            for (int i = 0; i < w; i++) {
                int py = y + h - (i * h / w);
                SDL_RenderDrawPoint(renderer_, x + i, py);
            }
            break;
            
        case 1: // Square wave (矩形波)
            for (int i = 0; i < w / 2; i++) {
                SDL_RenderDrawPoint(renderer_, x + i, y);
            }
            for (int i = w / 2; i < w; i++) {
                SDL_RenderDrawPoint(renderer_, x + i, y + h);
            }
            SDL_RenderDrawLine(renderer_, x + w / 2, y, x + w / 2, y + h);
            break;
            
        case 2: // Triangle wave (三角波)
            for (int i = 0; i < w / 2; i++) {
                int py = y + h - (i * 2 * h / w);
                SDL_RenderDrawPoint(renderer_, x + i, py);
            }
            for (int i = w / 2; i < w; i++) {
                int py = y + ((i - w / 2) * 2 * h / w);
                SDL_RenderDrawPoint(renderer_, x + i, py);
            }
            break;
            
        case 3: // Random noise (ランダムノイズ)
            for (int i = 0; i < w; i += 2) {
                int py = y + (rand() % (h + 1));
                SDL_RenderDrawPoint(renderer_, x + i, py);
                if (i + 1 < w) {
                    SDL_RenderDrawPoint(renderer_, x + i + 1, py);
                }
            }
            break;
    }
}

void Visualizer::renderAlgorithmDiagram(int x, int y, int algorithm, const YM2151State::Channel& channel) {
    // アルゴリズムの接続図を描画（YM2151データシートの公式表記に準拠）
    // オペレータ表記: M1(OP1), C1(OP2), M2(OP3), C2(OP4)
    const int box_w = 10;
    const int box_h = 6;
    const int spacing_h = 12;
    const int spacing_v = 8;
    const int arrow_len = 4;
    
    // Algorithm 0の全幅（4つのボックス + 5つの矢印）= OUTの位置
    const int max_width = box_w * 4 + arrow_len * 5;
    
    // 各オペレータの色をEGフェーズから取得
    int m1_r, m1_g, m1_b;
    int c1_r, c1_g, c1_b;
    int m2_r, m2_g, m2_b;
    int c2_r, c2_g, c2_b;
    getEGPhaseColor(channel.operators[0].eg_phase, m1_r, m1_g, m1_b); // M1
    getEGPhaseColor(channel.operators[1].eg_phase, c1_r, c1_g, c1_b); // C1
    getEGPhaseColor(channel.operators[2].eg_phase, m2_r, m2_g, m2_b); // M2
    getEGPhaseColor(channel.operators[3].eg_phase, c2_r, c2_g, c2_b); // C2
    
    // 接続線の色
    int line_r = 100, line_g = 120, line_b = 200;
    
    // アルゴリズムごとの描画（データシートの表記順）
    switch (algorithm) {
        case 0: { // CON=0: M1→C1→M2→C2→OUT (完全直列)
            int cx = x;
            // M1
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1 = {cx, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1, channel.feedback);
            cx += box_w + arrow_len;
            // 矢印
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y + box_h/2, cx, y + box_h/2);
            // C1
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1 = {cx, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1);
            cx += box_w + arrow_len;
            // 矢印
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y + box_h/2, cx, y + box_h/2);
            // M2
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2 = {cx, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2, channel.noise_enable, channel.noise_freq);
            cx += box_w + arrow_len;
            // 矢印
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y + box_h/2, cx, y + box_h/2);
            // C2
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2 = {cx, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2);
            cx += box_w + arrow_len;
            // OUT への矢印
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y + box_h/2, cx, y + box_h/2);
            break;
        }
            
        case 1: { // CON=1: M1→M2→C2→OUT, C1→OUT (並列)
            // 上段: M1→M2→C2 (3ボックス+4矢印)
            int chain_width = box_w * 3 + arrow_len * 4;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_1 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_1);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_1, channel.feedback);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_1 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_1);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_1, channel.noise_enable, channel.noise_freq);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_1 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_1);
            // 下段: C1 (並列)
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_1 = {x + (max_width - chain_width), y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_1);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_1);
            // OUT への合流
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, x + (max_width - chain_width) + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 2: { // CON=2: C1→M2→C2→OUT, M1→OUT (並列)
            // 上段: C1→M2→C2 (3ボックス+4矢印)
            int chain_width = box_w * 3 + arrow_len * 4;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_2 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_2);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_2 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_2);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_2, channel.noise_enable, channel.noise_freq);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_2 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_2);
            // 下段: M1 (並列)
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_2 = {x + (max_width - chain_width), y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_2);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_2);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_2, channel.feedback);
            // OUT への合流
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, x + (max_width - chain_width) + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 3: { // CON=3: M1→C1→C2→OUT, M2→OUT (並列)
            // 上段: M1→C1→C2 (3ボックス+4矢印)
            int chain_width = box_w * 3 + arrow_len * 4;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_3 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_3);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_3);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_3, channel.feedback);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_3 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_3);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_3);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_3 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_3);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_3);
            // 下段: M2 (並列)
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_3 = {x + (max_width - chain_width) + box_w + arrow_len, y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_3);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_3);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_3, channel.noise_enable, channel.noise_freq);
            // OUT への合流
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, x + (max_width - chain_width) + box_w + arrow_len + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 4: { // CON=4: M1→C1→OUT, M2→C2→OUT (2系統並列)
            // 上段・下段: 各2ボックス+3矢印
            int chain_width = box_w * 2 + arrow_len * 3;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_4 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_4);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_4);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_4, channel.feedback);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_4 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_4);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_4);
            // 下段: M2→C2
            cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_4 = {cx, y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_4);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_4);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_4, channel.noise_enable, channel.noise_freq);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y + spacing_v + box_h/2, cx, y + spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_4 = {cx, y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_4);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_4);
            // OUT への合流
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, cx + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 5: { // CON=5: M1→C1→OUT, M2→OUT, C2→OUT (3系統並列)
            // 上段: M1→C1 (2ボックス+3矢印)
            int chain_width = box_w * 2 + arrow_len * 3;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_5 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_5);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_5);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_5, channel.feedback);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_5 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_5);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_5);
            // 中段: M2 (1ボックス+2矢印)
            int m2_x = x + (max_width - (box_w + arrow_len * 2));
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_5 = {m2_x, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_5);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_5);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_5, channel.noise_enable, channel.noise_freq);
            // 下段: C2 (1ボックス+2矢印)
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_5 = {m2_x, y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_5);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_5);
            // OUT への合流
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, m2_x + box_w, y + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, m2_x + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 6: { // CON=6: M1→C1→OUT, M2→OUT, C2→OUT
            // 上段: M1→C1 (2ボックス+3矢印)
            int chain_width = box_w * 2 + arrow_len * 3;
            int cx = x + (max_width - chain_width);
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_6 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_6);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_6);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_6, channel.feedback);
            cx += box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx - arrow_len, y - spacing_v + box_h/2, cx, y - spacing_v + box_h/2);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_6 = {cx, y - spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_6);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_6);
            // 中下段: M2, C2 (1ボックス+2矢印)
            int m2_x = x + (max_width - (box_w + arrow_len * 2));
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_6 = {m2_x, y, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_6);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_6);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_6, channel.noise_enable, channel.noise_freq);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_6 = {m2_x, y + spacing_v, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_6);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_6);
            // OUT への合流を示す線
            int out_x = cx + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, cx + box_w, y - spacing_v + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, m2_x + box_w, y + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, m2_x + box_w, y + spacing_v + box_h/2, out_x, y);
            break;
        }
            
        case 7: { // CON=7: M1→OUT, C1→OUT, M2→OUT, C2→OUT (完全並列)
            // 4つのオペレータを縦に配置 (1ボックス+2矢印)
            int op_x = x + (max_width - (box_w + arrow_len * 2));
            SDL_SetRenderDrawColor(renderer_, m1_r, m1_g, m1_b, 255);
            SDL_Rect m1_7 = {op_x, y - (int)(spacing_v*1.5), box_w, box_h};
            SDL_RenderFillRect(renderer_, &m1_7);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m1_7);
            // フィードバック表示
            drawFeedbackIndicator(renderer_, m1_7, channel.feedback);
            SDL_SetRenderDrawColor(renderer_, c1_r, c1_g, c1_b, 255);
            SDL_Rect c1_7 = {op_x, y - spacing_v/2, box_w, box_h};
            SDL_RenderFillRect(renderer_, &c1_7);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c1_7);
            SDL_SetRenderDrawColor(renderer_, m2_r, m2_g, m2_b, 255);
            SDL_Rect m2_7 = {op_x, y + spacing_v/2, box_w, box_h};
            SDL_RenderFillRect(renderer_, &m2_7);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &m2_7);
            // ノイズ表示 (M2 = op 2)
            drawNoiseIndicator(renderer_, m2_7, channel.noise_enable, channel.noise_freq);
            SDL_SetRenderDrawColor(renderer_, c2_r, c2_g, c2_b, 255);
            SDL_Rect c2_7 = {op_x, y + (int)(spacing_v*1.5), box_w, box_h};
            SDL_RenderFillRect(renderer_, &c2_7);
            SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
            SDL_RenderDrawRect(renderer_, &c2_7);
            // OUTへの合流
            int out_x = op_x + box_w + arrow_len;
            SDL_SetRenderDrawColor(renderer_, line_r, line_g, line_b, 255);
            SDL_RenderDrawLine(renderer_, op_x + box_w, y - (int)(spacing_v*1.5) + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, op_x + box_w, y - spacing_v/2 + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, op_x + box_w, y + spacing_v/2 + box_h/2, out_x, y);
            SDL_RenderDrawLine(renderer_, op_x + box_w, y + (int)(spacing_v*1.5) + box_h/2, out_x, y);
            break;
        }
    }
}

void Visualizer::renderTitle() {
    if (!font_japanese_) return;
    
    TTF_Font* font_jp = (TTF_Font*)font_japanese_;
    TTF_Font* font_sm = (TTF_Font*)font_small_;
    
    // タイトル背景
    SDL_Rect bg = {0, 0, 1400, 40};
    SDL_SetRenderDrawColor(renderer_, 20, 20, 60, 255);
    SDL_RenderFillRect(renderer_, &bg);
    
    // 曲名を描画（大きく）
    SDL_Color title_color = {200, 220, 255, 255};
    SDL_Surface* title_surface = TTF_RenderUTF8_Blended(font_jp, song_title_, title_color);
    if (title_surface) {
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, title_surface);
        if (texture) {
            SDL_Rect dst = {10, 5, title_surface->w, title_surface->h};
            SDL_RenderCopy(renderer_, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(title_surface);
    }
    
    // ファイル名を描画（小さく、右上）
    if (font_sm && filename_[0] != '\0') {
        SDL_Color filename_color = {150, 160, 180, 255};
        SDL_Surface* filename_surface = TTF_RenderUTF8_Blended(font_sm, filename_, filename_color);
        if (filename_surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, filename_surface);
            if (texture) {
                SDL_Rect dst = {1400 - filename_surface->w - 10, 22, filename_surface->w, filename_surface->h};
                SDL_RenderCopy(renderer_, texture, nullptr, &dst);
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(filename_surface);
        }
    }
    
    // 再生時間を描画（ファイル名の左側）
    if (font_sm) {
        int minutes = (int)(elapsed_time_ / 60.0);
        int seconds = (int)(elapsed_time_) % 60;
        int millis = (int)((elapsed_time_ - (int)elapsed_time_) * 100);
        
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%02d:%02d.%02d", minutes, seconds, millis);
        
        SDL_Color time_color = {180, 200, 220, 255};
        SDL_Surface* time_surface = TTF_RenderUTF8_Blended(font_sm, time_str, time_color);
        if (time_surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, time_surface);
            if (texture) {
                // ファイル名と同じY座標(22)、ファイル名の左にスペースを空けて配置
                int filename_width = 0;
                if (filename_[0] != '\0') {
                    SDL_Surface* fn_surface = TTF_RenderUTF8_Blended(font_sm, filename_, time_color);
                    if (fn_surface) {
                        filename_width = fn_surface->w;
                        SDL_FreeSurface(fn_surface);
                    }
                }
                SDL_Rect dst = {1400 - filename_width - time_surface->w - 30, 22, time_surface->w, time_surface->h};
                SDL_RenderCopy(renderer_, texture, nullptr, &dst);
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(time_surface);
        }
    }
    
    // 区切り線
    SDL_SetRenderDrawColor(renderer_, 80, 90, 150, 255);
    SDL_RenderDrawLine(renderer_, 0, 40, 1400, 40);
}

void Visualizer::renderTimerInfo() {
    if (!state_) return;
    
    YM2151State::TimerState timer;
    state_->getTimerState(timer);
    
    int y = 44;
    int x = 10;
    
    // システム情報を表示（TIMER-Aと同じ行）
    renderBitmapText("SHARP X68000 / MXDRV 2.06+17 Rel.X5-S / MXDRVg V1.50a / mdx2wav 20251225 by mtm", x, y, 100, 140, 200);
    
    // TIMER情報をシステム情報の右側に配置
    int timer_x = x + 640;  // システム情報の右側から開始
    
    // Timer A情報
    char timer_a_text[64];
    snprintf(timer_a_text, sizeof(timer_a_text), "TIMER-A:%04d %s", 
             timer.timer_a, timer.timer_a_enable ? "ON" : "OFF");
    renderBitmapText(timer_a_text, timer_x, y, 
                    timer.timer_a_enable ? 100 : 60,
                    timer.timer_a_enable ? 200 : 100,
                    timer.timer_a_enable ? 100 : 60);
    
    // Timer B情報
    char timer_b_text[64];
    snprintf(timer_b_text, sizeof(timer_b_text), "TIMER-B:%03d %s", 
             timer.timer_b, timer.timer_b_enable ? "ON" : "OFF");
    renderBitmapText(timer_b_text, timer_x + 150, y,
                    timer.timer_b_enable ? 100 : 60,
                    timer.timer_b_enable ? 200 : 100,
                    timer.timer_b_enable ? 100 : 60);
    
    // Mute indicator
    if (ym2151_muted_) {
        renderBitmapText("MUTE", timer_x + 300, y, 255, 80, 80);
    }
    
    // Polyphonic mode indicator
    if (polyphonic_mode_) {
        renderBitmapText("POLY", timer_x + 360, y, 80, 200, 255);
    }
    
#ifdef __APPLE__
    // MIDI input indicator
    if (midi_client_ != 0) {
        renderBitmapText("MIDI-IN", timer_x + 420, y, 100, 255, 100);
    }
#endif
    
    // 区切り線
    SDL_SetRenderDrawColor(renderer_, 60, 70, 100, 255);
    SDL_RenderDrawLine(renderer_, 0, y + 10, 1400, y + 10);
}

void Visualizer::renderChannelInfo() {
    if (!state_) return;
    
    YM2151State::Channel channels[8];
    state_->getAllChannels(channels);
    
    TTF_Font* font_med = (TTF_Font*)font_medium_;
    TTF_Font* font_sm = (TTF_Font*)font_small_;
    
    int y = YM2151_START_Y;
    int line_height = YM2151_LINE_HEIGHT;
    
    // FMチャンネル (0-7) - 表示チャンネル数分だけ表示
    for (int ch = 0; ch < ym2151_display_channels_; ch++) {
        auto& c = channels[ch];
        
        int r, g, b;
        getChannelColor(c.algorithm, r, g, b);  // アルゴリズム番号で色を決定
        
        // チャンネル番号の背景
        SDL_Rect bg_rect = {5, y, 60, line_height - 10};
        SDL_SetRenderDrawColor(renderer_, r/3, g/3, b/3, 255);
        //SDL_RenderFillRect(renderer_, &bg_rect);
        
        // 7セグメント表示でチャンネル番号を表示
        int seg_x = 20;
        int seg_y = y + 10;
        int seg_w = 8;
        int seg_h = 7;
        
        // 出力に繋がっているオペレータ（キャリア）の出力をチェック
        bool has_output = false;
        for (int op = 0; op < 4; op++) {
            // アルゴリズムに基づいてキャリアかどうか判定
            bool is_carrier = false;
            switch (c.algorithm) {
                case 0: case 1: case 2: // ALG 0-2: C2のみ
                    is_carrier = (op == 3);
                    break;
                case 3: case 4: // ALG 3-4: C1とC2
                    is_carrier = (op == 1 || op == 3);
                    break;
                case 5: case 6: // ALG 5-6: C1, M2, C2
                    is_carrier = (op == 1 || op == 2 || op == 3);
                    break;
                case 7: // ALG 7: 全て
                    is_carrier = true;
                    break;
            }
            
            if (is_carrier) {
                // eg_outは0-8184で、大きいほど音が小さい
                // 出力があると判定する閾値（8000以下なら音が出ている）
                if (c.operators[op].eg_out < 8000) {
                    has_output = true;
                    break;
                }
            }
        }
        
        // オレンジ色（出力がある時は明るく、ない時は暗く）
        int seg_r = has_output ? CHANNEL_LABEL_COLOR_ACTIVE_R : CHANNEL_LABEL_COLOR_INACTIVE_R;
        int seg_g = has_output ? CHANNEL_LABEL_COLOR_ACTIVE_G : CHANNEL_LABEL_COLOR_INACTIVE_G;
        int seg_b = has_output ? CHANNEL_LABEL_COLOR_ACTIVE_B : CHANNEL_LABEL_COLOR_INACTIVE_B;
        
        // チャンネル番号を7セグメント表示
        draw7Segment(seg_x, seg_y, ch, seg_r, seg_g, seg_b, seg_w, seg_h);
        
        // "YAMAHA"ラベルを表示（チャンネル番号の下、YM2151の上）
        if (bitmap_font_texture_) {
            renderBitmapText("YAMAHA", 10, seg_y + 20, seg_r, seg_g, seg_b);
        } else if (font_sm) {
            renderText(renderer_, font_sm, "YAMAHA", 10, seg_y + 20, seg_r, seg_g, seg_b);
        }
        
        // "YM2151"ラベルを表示（YAMAHAの下）
        if (bitmap_font_texture_) {
            renderBitmapText("YM2151", 10, seg_y + 28, seg_r, seg_g, seg_b);
        } else if (font_sm) {
            renderText(renderer_, font_sm, "YM2151", 10, seg_y + 28, seg_r, seg_g, seg_b);
        }
        
        // レジスタ書き込み量を線で表示（YM2151ラベルの下）
        // 最大50バイトを想定して線の長さを計算
        int max_reg_bar = 50;
        int reg_bar_width = std::min((int)c.register_writes, max_reg_bar);
        if (reg_bar_width > 0) {
            SDL_SetRenderDrawColor(renderer_, seg_r / 2, seg_g / 2, seg_b / 2 + 60, 255);
            SDL_Rect reg_bar = {10, seg_y + 38, reg_bar_width, 2};
            SDL_RenderFillRect(renderer_, &reg_bar);
        }
        
        // ピークホールドを表示（縦線）
        int peak_bar_width = std::min((int)c.register_writes_peak, max_reg_bar);
        if (peak_bar_width > 0) {
            SDL_SetRenderDrawColor(renderer_, seg_r, seg_g, seg_b / 2 + 100, 255);
            SDL_RenderDrawLine(renderer_, 10 + peak_bar_width, seg_y + 37, 10 + peak_bar_width, seg_y + 40);
        }
        
        // キーオン状態で明るくする
        if (c.key_on) {
            SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
            SDL_Rect key_rect = {70, y, 10, line_height - 10};
            SDL_RenderFillRect(renderer_, &key_rect);
        }
        
        // LFOの位相をエミュレータから取得（AMSによる音量変調用）
        YM2151State::LFOState lfo;
        state_->getLFOState(lfo);
        
        // LFO位相 0-511を0-2πに変換
        float lfo_phase = (lfo.lfo_count / 512.0f) * 2.0f * 3.14159265f;
        
        // LFO波形に応じた値を計算 (-1.0 ~ +1.0)
        float lfo_value = 0.0f;
        switch (lfo.lfo_waveform & 0x03) {
            case 0: // Saw wave
                lfo_value = (lfo.lfo_count / 256.0f) - 1.0f;
                break;
            case 1: // Square wave
                lfo_value = (lfo.lfo_count < 256) ? 1.0f : -1.0f;
                break;
            case 2: // Triangle wave
                if (lfo.lfo_count < 256) {
                    lfo_value = (lfo.lfo_count / 128.0f) - 1.0f;
                } else {
                    lfo_value = 3.0f - (lfo.lfo_count / 128.0f);
                }
                break;
            case 3: // Noise (random-like, use phase as seed)
                lfo_value = sinf(lfo_phase * 17.0f);
                break;
        }
        
        // AMS (0-3) による音量変調の深さ
        float ams_depth = c.ams / 3.0f;  // 0.0 ~ 1.0
        float ams_modulation = lfo_value * ams_depth * 0.3f;  // -0.3 ~ +0.3 (AMS最大時)
        
        // オペレータのEG状態を横向きバーで表示（4つ縦並び）
        int eg_x = 85;
        int eg_y = y + 0;
        int bar_width = 110;  // 横バーの最大幅
        int bar_height = 4;   // 横バーの高さ
        int bar_spacing = 5;  // バー間の間隔
        
        for (int op = 0; op < 4; op++) {
            auto& oper = c.operators[op];
            int op_y = eg_y + op * bar_spacing;
            
            // EG出力を横バーで表示
            // eg_outは0-8184で、大きいほど音が小さい（反転必要）
            float base_level = (8184 - oper.eg_out) / 8184.0f;  // 0.0 ~ 1.0
            
            // AMSによる揺らぎを追加
            //float modulated_level = base_level * (1.0f + ams_modulation);
            float modulated_level = base_level * (1.0f);
            if (modulated_level < 0.0f) modulated_level = 0.0f;
            if (modulated_level > 1.0f) modulated_level = 1.0f;
            
            int level_w = (int)(modulated_level * bar_width);
            
            // ピーク表示
            float peak_level = (8184 - oper.eg_out_peak) / 8184.0f;
            int peak_w = (int)(peak_level * bar_width);
            if (peak_w > 0) {
                // EGフェーズに応じた色
                int eg_r, eg_g, eg_b;
                switch (oper.eg_phase) {
                    case 1: eg_r = 100; eg_g = 255; eg_b = 100; break; // attack 緑
                    case 2: eg_r = 255; eg_g = 200; eg_b = 100; break; // decay オレンジ
                    case 3: eg_r = 100; eg_g = 180; eg_b = 255; break; // sustain 青
                    case 4: eg_r = 200; eg_g = 100; eg_b = 200; break; // release 紫
                    default: eg_r = 60; eg_g = 60; eg_b = 80; break;   // off 暗い
                }
                
                // ピークライン（細い線）
                SDL_SetRenderDrawColor(renderer_, eg_r, eg_g, eg_b, 255);
                SDL_Rect peak_line = {eg_x + peak_w - 1, op_y, 1, bar_height};
                SDL_RenderFillRect(renderer_, &peak_line);
            }
            
            // 現在のレベルバー
            if (level_w > 0) {
                int eg_r, eg_g, eg_b;
                switch (oper.eg_phase) {
                    case 1: eg_r = 100; eg_g = 255; eg_b = 100; break;
                    case 2: eg_r = 255; eg_g = 200; eg_b = 100; break;
                    case 3: eg_r = 100; eg_g = 180; eg_b = 255; break;
                    case 4: eg_r = 200; eg_g = 100; eg_b = 200; break;
                    default: eg_r = 60; eg_g = 60; eg_b = 80; break;
                }
                SDL_SetRenderDrawColor(renderer_, eg_r, eg_g, eg_b, 180);
                SDL_Rect eg_bar = {eg_x, op_y, level_w, bar_height};
                SDL_RenderFillRect(renderer_, &eg_bar);
            }
            
            // バーの枠
            SDL_Rect eg_frame = {eg_x, op_y, bar_width, bar_height};
            SDL_SetRenderDrawColor(renderer_, 60, 70, 120, 255);
            SDL_RenderDrawRect(renderer_, &eg_frame);
        }
        
        // アルゴリズム表示（接続図と被らないように上に配置）
        char alg_text[16];
        snprintf(alg_text, sizeof(alg_text), "ALG:%d", c.algorithm);
        if (bitmap_font_texture_) {
            renderBitmapText(alg_text, 85, y + 20, 100, 140, 220);
        } else {
            renderText(renderer_, font_sm, alg_text, 85, y + 20, 100, 140, 220);
        }
        
        // パン表示 (L/R)
        char pan_text[16];
        const char* pan_str = "  ";
        if ((c.left_right & 0x80) && (c.left_right & 0x40)) {
            pan_str = "LR";
        } else if (c.left_right & 0x80) {
            pan_str = "L ";
        } else if (c.left_right & 0x40) {
            pan_str = " R";
        } else {
            pan_str = "--";
        }
        snprintf(pan_text, sizeof(pan_text), "PAN:%s", pan_str);
        renderBitmapText(pan_text, 145, y + 20, 100, 140, 220);
        
        // PMS/AMS表示（PANの下）
        // PMS (0-7): 周波数変調の深さ
        char pms_text[32];
        snprintf(pms_text, sizeof(pms_text), "PMD:%02X", lfo.pmd);
        bool pms_active = (c.pms > 0);
        int pms_r = pms_active ? 120 : 40;
        int pms_g = pms_active ? 180 : 60;
        int pms_b = pms_active ? 255 : 80;
        renderBitmapText(pms_text, 145, y + 44, pms_r, pms_g, pms_b);
        
        // AMS (0-3): 音量変調の深さ
        char ams_text[32];
        snprintf(ams_text, sizeof(ams_text), "AMD:%02X", lfo.amd);
        bool ams_active = (c.ams > 0);
        int ams_r = ams_active ? 120 : 40;
        int ams_g = ams_active ? 180 : 60;
        int ams_b = ams_active ? 255 : 80;
        renderBitmapText(ams_text, 145, y + 56, ams_r, ams_g, ams_b);
        
        // LFRQ表示（AMS/PMSのいずれもアクティブでないなら暗く表示）
        bool lfo_active = pms_active || ams_active;
        int lfrq_r = lfo_active ? 100 : 40;
        int lfrq_g = lfo_active ? 150 : 60;
        int lfrq_b = lfo_active ? 200 : 80;
        char lfrq_text[32];
        snprintf(lfrq_text, sizeof(lfrq_text), "LFR:%02X", lfo.lfo_freq);
        renderBitmapText(lfrq_text, 145, y + 32, lfrq_r, lfrq_g, lfrq_b);
        
        // LFO波形を図示（LFRQの右隣）
        if (lfo_active)
            renderLFOWaveform(195, y + 32, lfo.lfo_waveform, lfrq_r, lfrq_g, lfrq_b);
        
        // アルゴリズム接続図を描画
        renderAlgorithmDiagram(85, y + 45, c.algorithm, c);
        
        y += line_height;
    }
    
    // ADPCMチャンネル (8-15) の表示
    if (adpcm_display_channels_ > 0) {
        renderADPCMChannels(y);
    }
}

void Visualizer::renderKeyboard() {
    if (!state_) return;
    
    YM2151State::Channel channels[8];
    state_->getAllChannels(channels);
    
    TTF_Font* font_sm = (TTF_Font*)font_small_;
    
    // 各チャンネルごとに鍵盤を縦に並べる
    // 8オクターブ（C0-C7）全域を表示 - YM2151のKCレジスタで設定可能な全範囲
    int start_x = KEYBOARD_START_X;
    int start_y = YM2151_START_Y;
    int line_height = YM2151_LINE_HEIGHT;
    int white_key_width = WHITE_KEY_WIDTH;
    int white_key_height = line_height - 15;
    int black_key_width = 6;
    int black_key_height = (white_key_height * 2) / 3;
    
    // 波形表示の設定
    int waveform_x = CHANNEL_WAVEFORM_X;
    int waveform_width = CHANNEL_WAVEFORM_WIDTH;
    int waveform_height = line_height - 20;
    
    // FMチャンネルのみ鍵盤表示 (ADPCMは音程情報がないため表示しない)
    // 表示チャンネル数分だけ表示
    for (int ch = 0; ch < ym2151_display_channels_; ch++) {
        auto& channel = channels[ch];
        
        // EGフェーズに基づく色を決定（全オペレータの中で最も進んでいるフェーズを使用）
        int dominant_phase = 0;  // デフォルトはoff
        {
            // アクティブなオペレータの中で最も進んでいるフェーズを探す
            for (int op = 0; op < 4; op++) {
                //if (channel.operator_mask & (1 << op))
                {
                    int phase = channel.operators[op].eg_phase;
                    // attack(1) > decay(2) > sustain(3) > release(4) の優先順位
                    if (phase == 1) {  // attack
                        dominant_phase = 1;
                        break;  // attackが最優先
                    } else if (phase == 2 && dominant_phase != 1) {  // decay
                        dominant_phase = 2;
                    } else if (phase == 3 && dominant_phase < 2) {  // sustain
                        dominant_phase = 3;
                    } else if (phase == 4 && dominant_phase < 2) {  // release
                        dominant_phase = 4;
                    }
                }
            }
        }
        
        // EGフェーズに応じた色を設定
        int r, g, b;
        switch (dominant_phase) {
            case 1: // attack
                r = 100; g = 255; b = 100; // 緑
                break;
            case 2: // decay
                r = 255; g = 200; b = 100; // オレンジ
                break;
            case 3: // sustain
                r = 100; g = 180; b = 255; // 青
                break;
            case 4: // release
                r = 200; g = 100; b = 200; // 紫
                break;
            default: // off or next
                r = 60; g = 60; b = 80; // 暗い
                break;
        }
        
        int base_x = start_x;
        int base_y = start_y + ch * line_height;
        
        // Key Fractionインジケーター（鍵盤の左横）
        {
            int indicator_x = base_x - 10;  // 鍵盤の左10px
            int indicator_center_y = base_y + white_key_height;
            int indicator_range = white_key_height - 2;  // 上下の可動範囲
            
            // KF (0-63) を -1.0 ~ +1.0 に変換
            // KF=32が中央（0）、KF=0が下（-1）、KF=63が上（+1）
            float kf_normalized = (channel.key_fraction) / 63.0f;  // 0 ~ +1.0
            
            // Y座標を計算（上が正、下が負）
            int indicator_y = indicator_center_y - (int)(kf_normalized * indicator_range);
            
            // 中央線（参照用、グレー）
            SDL_SetRenderDrawColor(renderer_, 80, 80, 100, 128);
            SDL_RenderDrawLine(renderer_, indicator_x, indicator_center_y, indicator_x + 3, indicator_center_y);

            // 緑の矩形（4x1ピクセル）
            SDL_Rect kf_rect = {indicator_x, indicator_y, 4, 1};
            SDL_SetRenderDrawColor(renderer_, 100, 255, 100, 255);  // 緑
            SDL_RenderFillRect(renderer_, &kf_rect);
            
        }
        
        // 白鍵を描画 (C0-C7) - 8オクターブ = 57個の白鍵
        const int white_notes[] = {0, 2, 4, 5, 7, 9, 11};  // C, D, E, F, G, A, B
        int white_key_index = 0;
        
        for (int octave = 0; octave <= 8; octave++) {
            int notes_to_draw = (octave == 8) ? 1 : 7;  // C8のみ描画
            
            for (int i = 0; i < notes_to_draw; i++) {
                int note = white_notes[i];
                int midi_note = octave * 12 + note;
                int x = base_x + white_key_index * white_key_width;
                
                SDL_Rect key_rect = {x, base_y, white_key_width - 1, white_key_height-2};
                
                bool pressed = (channel.note == midi_note);
                
                if (pressed) {
                    SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
                } else {
                    SDL_SetRenderDrawColor(renderer_, 200/2, 210/2, 255/2, 255);
                }
                
                SDL_RenderFillRect(renderer_, &key_rect);
                SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
                SDL_RenderDrawRect(renderer_, &key_rect);
                
                // C音にオクターブ番号を表示
                if (note == 0) {
                    char octave_text[4];
                    snprintf(octave_text, sizeof(octave_text), "%d", octave);
                    renderBitmapText(octave_text, x + 2, base_y + white_key_height , 80, 100, 160);
                }
                
                white_key_index++;
            }
        }
        
        // 黒鍵を描画 (C0-C6まで) - 7オクターブ = 35個の黒鍵
        const int black_notes[] = {1, 3, 6, 8, 10};  // C#, D#, F#, G#, A#
        const int black_positions[] = {0, 1, 3, 4, 5};  // 白鍵のどの位置に配置するか
        white_key_index = 0;
        
        for (int octave = 0; octave <= 7; octave++) {
            for (int i = 0; i < 5; i++) {
                int note = black_notes[i];
                int midi_note = octave * 12 + note;
                int x = base_x + (white_key_index + black_positions[i]) * white_key_width + white_key_width - black_key_width / 2;
                
                SDL_Rect key_rect = {x, base_y, black_key_width, black_key_height};
                
                bool pressed = (channel.key_on && channel.note == midi_note);
                
                if (pressed) {
                    SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
                } else {
                    SDL_SetRenderDrawColor(renderer_, 30, 30, 50, 255);
                }
                
                SDL_RenderFillRect(renderer_, &key_rect);
                SDL_SetRenderDrawColor(renderer_, 60, 60, 120, 255);
                SDL_RenderDrawRect(renderer_, &key_rect);
            }
            white_key_index += 7;
        }
        
        // 各オペレータのMUL/Detune適用後の周波数を横線で表示（接続関係を示す）
        //if (channel.key_on) // keyonじゃなくても表示する。offの後release時も音が出ているため
        {
            // KF (0-63) による周波数オフセットを計算
            // KFは1半音を64分割する (64 KF = 1半音 = 100セント)
            float kf_cents = (channel.key_fraction / 64.0f) * 100.0f;  // 0-100セント
            float kf_semitones = kf_cents / 100.0f;  // 0-1半音
            
            // ベース周波数（KC+KF）
            float base_freq_note = channel.note + kf_semitones;
            
            // 各オペレータのY座標（下からOP0,1,2,3の順で積む）
            const int line_spacing = 2;  // オペレータ間の縦間隔
            
            // オペレータごとに接続先への横線を描画
            for (int op = 0; op < 4; op++) {
                if (!(channel.operator_mask & (1 << op))) continue;
                
                auto& oper = channel.operators[op];
                
                // このオペレータのY座標
                int op_y = base_y - 2 - ((3-op) * line_spacing);
                
                // MUL (0-15) による周波数倍率
                float mul_ratio = (oper.multiple == 0) ? 0.5f : (float)oper.multiple;
                
                // DT1 (0-7) によるデチューン（セント単位）
                float dt1_cents = 0.0f;
                if (oper.detune1 < 4) {
                    dt1_cents = -(4 - oper.detune1) * 12.5f;
                } else if (oper.detune1 > 4) {
                    dt1_cents = (oper.detune1 - 4) * 12.5f;
                }
                
                // DT2 (0-3) による粗調整（セント単位）
                float dt2_cents = oper.detune2 * 100.0f;
                float total_detune_semitones = (dt1_cents + dt2_cents) / 100.0f;
                
                // このオペレータの最終周波数
                float mul_semitones = log2f(mul_ratio) * 12.0f;
                float op_note = base_freq_note + mul_semitones + total_detune_semitones;
                float op_position = op_note / 12.0f * 7.0f;
                int op_x = base_x + (int)(op_position * white_key_width);
                
                // 接続先を取得
                int target = getOperatorTarget(channel.algorithm, op);
                int target_x;
                
                if (target == -1) {
                    // OUTに直結 → KC+KFが基準
                    float out_position = base_freq_note / 12.0f * 7.0f;
                    target_x = base_x + (int)(out_position * white_key_width);
                } else {
                    // 他のオペレータに接続 → そのオペレータの周波数
                    auto& target_oper = channel.operators[target];
                    float target_mul_ratio = (target_oper.multiple == 0) ? 0.5f : (float)target_oper.multiple;
                    
                    float target_dt1_cents = 0.0f;
                    if (target_oper.detune1 < 4) {
                        target_dt1_cents = -(4 - target_oper.detune1) * 12.5f;
                    } else if (target_oper.detune1 > 4) {
                        target_dt1_cents = (target_oper.detune1 - 4) * 12.5f;
                    }
                    
                    float target_dt2_cents = target_oper.detune2 * 100.0f;
                    float target_total_detune = (target_dt1_cents + target_dt2_cents) / 100.0f;
                    
                    float target_mul_semitones = log2f(target_mul_ratio) * 12.0f;
                    float target_note = base_freq_note + target_mul_semitones + target_total_detune;
                    float target_position = target_note / 12.0f * 7.0f;
                    target_x = base_x + (int)(target_position * white_key_width);
                }
                
                // 範囲チェック
                if ((op_x >= base_x && op_x < base_x + 57 * white_key_width) ||
                    (target_x >= base_x && target_x < base_x + 57 * white_key_width)) {
                    
                    // 最終出力までの距離に応じて色を変更（EGフェーズと同じ配色）
                    int distance = getOperatorOutputDistance(channel.algorithm, op);
                    uint8_t r, g, b;
                    switch (distance) {
                        case 0:  // 直結 → 緑 (Attack相当)
                            r = 100; g = 255; b = 100;
                            break;
                        case 1:  // 1ノード離れる → オレンジ (Decay相当)
                            r = 255; g = 180; b = 100;
                            break;
                        case 2:  // 2ノード離れる → 青 (Sustain相当)
                            r = 100; g = 150; b = 255;
                            break;
                        case 3:  // 3ノード離れる → 紫 (Release相当)
                            r = 200; g = 100; b = 255;
                            break;
                        default:
                            r = 100; g = 150; b = 255;  // デフォルトは青
                    }
                    
                    // 接続先から現在のオペレータへ横線を描画
                    SDL_SetRenderDrawColor(renderer_, r, g, b, 200);
                    SDL_RenderDrawLine(renderer_, target_x, op_y, op_x, op_y);
                }
            }
        }
        
        // チャンネルの波形を鍵盤の右側に表示
        int16_t waveform_data[YM2151State::CHANNEL_WAVEFORM_SIZE];
        state_->getChannelWaveform(ch, waveform_data, YM2151State::CHANNEL_WAVEFORM_SIZE);
        
        // アルゴリズムベースの色を取得
        int wave_r, wave_g, wave_b;
        getChannelColor(channel.algorithm, wave_r, wave_g, wave_b);
        
        // 汎用波形描画関数を使用（YM2151用のスケールを適用）
        renderChannelWaveform(waveform_x, base_y + 2, waveform_width, waveform_height,
                            waveform_data, YM2151State::CHANNEL_WAVEFORM_SIZE,
                            wave_r, wave_g, wave_b, ch, ym2151_waveform_scale_);
    }
}

void Visualizer::renderWaveform() {
    if (!impl) return;
    
    TTF_Font* font_med = (TTF_Font*)font_medium_;
    
    int waveform_x = 240;  // 鍵盤と同じ開始位置
    int waveform_y = 420;
    int waveform_width = 820;  // 8オクターブの鍵盤幅 (57白鍵 × 10px) に合わせる
    int waveform_height = 150;
    
    // タイトル
    if (bitmap_font_texture_) {
        renderBitmapText("Waveform", waveform_x + 5, waveform_y - 25, 120, 150, 255);
    } else {
        renderText(renderer_, font_med, "Waveform", waveform_x + 5, waveform_y - 25, 120, 150, 255);
    }
    
    // 波形エリアの枠
    SDL_Rect frame = {waveform_x, waveform_y, waveform_width, waveform_height};
    SDL_SetRenderDrawColor(renderer_, 80, 90, 150, 255);
    SDL_RenderDrawRect(renderer_, &frame);
    
    // 中心線
    SDL_SetRenderDrawColor(renderer_, 60, 70, 120, 255);
    SDL_RenderDrawLine(renderer_, 
                      waveform_x, waveform_y + waveform_height/2,
                      waveform_x + waveform_width, waveform_y + waveform_height/2);
    
    // L/Rラベル
    if (bitmap_font_texture_) {
        renderBitmapText("L", waveform_x + waveform_width + 10, waveform_y + 30, 100, 200, 255);
    } else {
        renderText(renderer_, font_med, "L", waveform_x + waveform_width + 10, waveform_y + 30, 100, 200, 255);
    }
    
    // 波形を描画（左チャンネル）
    SDL_SetRenderDrawColor(renderer_, 100, 180, 255, 255);
    for (size_t i = 1; i < impl->waveform_left.size(); i++) {
        int x1 = waveform_x + ((i-1) * waveform_width) / impl->waveform_left.size();
        int y1 = waveform_y + waveform_height/2 - (int)(impl->waveform_left[i-1] * waveform_height/2);
        int x2 = waveform_x + (i * waveform_width) / impl->waveform_left.size();
        int y2 = waveform_y + waveform_height/2 - (int)(impl->waveform_left[i] * waveform_height/2);
        
        SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
    }
}

void Visualizer::triggerNote(int midi_note, bool key_on, int velocity) {
    if (!opm_wrapper_) return;
    
    // OPMVisualizerラッパーを使用してレジスタを書き込む
    // これにより状態更新と実チップへの書き込みが両方行われる
    OPMVisualizer* wrapper = (OPMVisualizer*)opm_wrapper_;
    
    // オクターブオフセットを適用
    midi_note += octave_offset_ * 12;
    
    // X68000のYM2151の音程はMIDIより2半音高いため補正
    midi_note -= 2;

    //さらにkc=0がC#4相当なので1半音下げる
    midi_note -= 1;
    
    // 範囲チェック (0-127)
    if (midi_note < 0 || midi_note > 127) return;
    
    // ポリフォニックモードの場合、チャンネルを自動割り当て
    int target_channel = selected_channel_;
    if (polyphonic_mode_) {
        target_channel = findChannelForNote(midi_note, key_on);
        if (target_channel < 0) return; // チャンネルが見つからない
    }

    // MIDIノート番号をYM2151のKC (Key Code)に変換
    // MIDI note 60 = C4, YM2151では octave=5, note=0 が C4相当
    // 計算: octave = (midi_note / 12) - 1 + 1 = midi_note / 12
    //       semitone = midi_note % 12
    
    int octave = (midi_note / 12);
    int note_in_octave = midi_note % 12;
    // 3,7,11,15は欠番なので変換テーブルを使用
    int note_to_kc_table[12] = {
        0,1,2,4,
        5,6,8,9,
        10,12,13,14
    };
    //int note_in_octave = (semitone * 16) / 12;
    
    // KC = (octave << 4) | note_in_octave
    uint8_t kc = ((octave & 0x07) << 4) | note_to_kc_table[note_in_octave];
    
    // ベロシティに基づいてTLを調整
    if (key_on) {
        for (int op = 0; op < 4; op++) {
            // プリセットTL値をベースに使用
            uint8_t base_tl = channel_keys_[target_channel].preset_tl[op];
            
            // new_TL = (velocity - 96) / 96 * base_TL
            // velocity 96で原音、127で減衰なし、0で最大減衰
            int tl_offset = ((127 - velocity) * base_tl) / 96;
            int new_tl = base_tl + tl_offset;
            if (new_tl > 127) new_tl = 127;
            if (new_tl < 0) new_tl = 0;
            
            // 0x60-0x7F: TL
            wrapper->SetRegDirect(0x60 + (op << 3) + target_channel, new_tl);
        }
    }
    
    // 選択されたチャンネルにKC (Key Code)を設定
    // レジスタ 0x28-0x2F: KC (Key Code)
    // SetRegDirect を使用してミュートをバイパス
    wrapper->SetRegDirect(0x28 + target_channel, kc);
    
    // Key On/Off を設定
    // レジスタ 0x08: Key On/Off
    // bit 0-2: チャンネル番号 (0-7)
    // bit 3-6: オペレータマスク (M1=bit3, C1=bit4, M2=bit5, C2=bit6)
    //          全オペレータをオンにする場合は 0x78 (0b01111000)
    uint8_t key_code = target_channel & 0x07;
    if (key_on) {
        key_code |= 0x78;  // 全オペレータをオン
        // ポリフォニックモードの場合、チャンネル状態を更新
        if (polyphonic_mode_) {
            channel_keys_[target_channel].active = true;
            channel_keys_[target_channel].midi_note = midi_note;
            channel_keys_[target_channel].key_on_time = frame_count_;
        }
    } else {
        // キーオフ時、ポリフォニックモードではチャンネルを非アクティブに
        if (polyphonic_mode_) {
            channel_keys_[target_channel].active = false;
        }
    }
    
    wrapper->SetRegDirect(0x08, key_code);
}

void Visualizer::enterPolyphonicMode() {
    if (!opm_wrapper_ || !state_) return;
    
    fprintf(stderr, "Entering polyphonic mode: copying ch%d settings to all channels\n", selected_channel_);
    
    // 選択チャンネルのTL値を取得
    YM2151State::Channel selected_ch_info;
    state_->getChannelInfo(selected_channel_, selected_ch_info);
    
    // 全チャンネルに現在のチャンネルのレジスタをコピー
    for (int ch = 0; ch < 8; ch++) {
        if (ch != selected_channel_) {
            copyChannelRegisters(selected_channel_, ch);
        }
        // チャンネル状態をリセット
        channel_keys_[ch].active = false;
        channel_keys_[ch].midi_note = -1;
        channel_keys_[ch].key_on_time = 0;
        
        // プリセットTL値を保存（velocityのベース値）
        for (int op = 0; op < 4; op++) {
            channel_keys_[ch].preset_tl[op] = selected_ch_info.operators[op].total_level;
        }
    }
}

int Visualizer::findChannelForNote(int midi_note, bool key_on) {
    if (key_on) {
        // キーオン時: 同じノートを探す
        for (int ch = 0; ch < 8; ch++) {
            if (channel_keys_[ch].midi_note == midi_note) {
                return ch; // 同じノートを再トリガー
            }
        }
        
        // 未使用チャンネルを探す
        if(0)for (int ch = 0; ch < 8; ch++) {
            if (!channel_keys_[ch].active) {
                return ch;
            }
        }
        
        // すべて使用中の場合、最も古いチャンネルを使う
        int oldest_ch = 0;
        unsigned int oldest_time = channel_keys_[0].key_on_time;
        for (int ch = 1; ch < 8; ch++) {
            if (channel_keys_[ch].key_on_time < oldest_time) {
                oldest_time = channel_keys_[ch].key_on_time;
                oldest_ch = ch;
            }
        }
        return oldest_ch;
    } else {
        // キーオフ時: 同じノートのチャンネルを探す
        for (int ch = 0; ch < 8; ch++) {
            if (channel_keys_[ch].active && channel_keys_[ch].midi_note == midi_note) {
                return ch;
            }
        }
        return -1; // 見つからない
    }
}

void Visualizer::copyChannelRegisters(int src_ch, int dst_ch) {
    if (!opm_wrapper_ || !state_) return;
    
    OPMVisualizer* wrapper = (OPMVisualizer*)opm_wrapper_;
    
    // ソースチャンネルの情報を取得
    YM2151State::Channel src_info;
    state_->getChannelInfo(src_ch, src_info);
    
    // チャンネル固有のレジスタをコピー
    // 0x20-0x27: RL/FB/CON (Algorithm with Pan)
    wrapper->SetRegDirect(0x20 + dst_ch, src_info.left_right | (src_info.feedback << 3) | src_info.algorithm);
    
    // 0x38-0x3F: PMS/AMS
    wrapper->SetRegDirect(0x38 + dst_ch, src_info.pms << 4 | src_info.ams);
    
    // オペレータパラメータをコピー (4オペレータ分)
    for (int op = 0; op < 4; op++) {
        const auto& src_op = src_info.operators[op];
        
        // 0x40-0x5F: DT1/MUL
        wrapper->SetRegDirect(0x40 + (op << 3) + dst_ch, src_op.detune1 << 4 | src_op.multiple);
        
        // 0x60-0x7F: TL
        wrapper->SetRegDirect(0x60 + (op << 3) + dst_ch, src_op.total_level);
        
        // 0x80-0x9F: KS/AR
        wrapper->SetRegDirect(0x80 + (op << 3) + dst_ch, src_op.key_scale << 6 | src_op.attack_rate);
        
        // 0xA0-0xBF: D1R (decay rate)
        wrapper->SetRegDirect(0xA0 + (op << 3) + dst_ch, src_op.decay_rate);
        
        // 0xC0-0xDF: DT2/D2R (sustain rate)
        wrapper->SetRegDirect(0xC0 + (op << 3) + dst_ch, src_op.detune2 << 6 | src_op.sustain_rate);
        
        // 0xE0-0xFF: D1L/RR
        wrapper->SetRegDirect(0xE0 + (op << 3) + dst_ch, src_op.sustain_level << 4 | src_op.release_rate);
    }
}

#ifdef __APPLE__
// MIDI入力コールバック（C関数）
static void MIDIReadCallback(const MIDIPacketList* packetList, void* readProcRefCon, void* srcConnRefCon) {
    Visualizer* self = (Visualizer*)readProcRefCon;
    const MIDIPacket* packet = &packetList->packet[0];
    
    for (UInt32 i = 0; i < packetList->numPackets; i++) {
        Byte* data = (Byte*)packet->data;
        UInt16 length = packet->length;
        
        if (length >= 3) {
            Byte status = data[0] & 0xF0;
            Byte note = data[1] & 0x7F;
            Byte velocity = data[2] & 0x7F;
            
            if (status == 0x90 && velocity > 0) {
                // Note On
                self->triggerNote(note, true, velocity);
            } else if (status == 0x80 || (status == 0x90 && velocity == 0)) {
                // Note Off
                self->triggerNote(note, false, 0);
            }
        }
        
        packet = MIDIPacketNext(packet);
    }
}

void Visualizer::midiInputCallback(void* message, void* refCon) {
    // Unused - kept for compatibility
}

bool Visualizer::initMIDI() {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    
    OSStatus status = MIDIClientCreate(CFSTR("mdx2wav"), NULL, NULL, &client);
    if (status != noErr) {
        fprintf(stderr, "Failed to create MIDI client: %d\n", (int)status);
        return false;
    }
    
    status = MIDIInputPortCreate(client, CFSTR("Input"), MIDIReadCallback, this, &port);
    if (status != noErr) {
        fprintf(stderr, "Failed to create MIDI input port: %d\n", (int)status);
        MIDIClientDispose(client);
        return false;
    }
    
    // 全ての利用可能なMIDIソースに接続
    ItemCount sourceCount = MIDIGetNumberOfSources();
    if (sourceCount == 0) {
        fprintf(stderr, "No MIDI sources found\n");
        MIDIPortDispose(port);
        MIDIClientDispose(client);
        return false;
    }
    
    fprintf(stderr, "Found %d MIDI source(s):\n", (int)sourceCount);
    for (ItemCount i = 0; i < sourceCount; i++) {
        MIDIEndpointRef source = MIDIGetSource(i);
        CFStringRef name = NULL;
        MIDIObjectGetStringProperty(source, kMIDIPropertyName, &name);
        
        if (name) {
            char nameBuf[256];
            CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
            fprintf(stderr, "  [%d] %s\n", (int)i, nameBuf);
            CFRelease(name);
        }
        
        status = MIDIPortConnectSource(port, source, NULL);
        if (status != noErr) {
            fprintf(stderr, "Failed to connect to MIDI source %d: %d\n", (int)i, (int)status);
        } else {
            fprintf(stderr, "Connected to MIDI source %d\n", (int)i);
        }
    }
    
    midi_client_ = client;
    midi_port_ = port;
    
    return true;
}

void Visualizer::shutdownMIDI() {
    if (midi_port_) {
        MIDIPortDispose(midi_port_);
        midi_port_ = 0;
    }
    if (midi_client_) {
        MIDIClientDispose(midi_client_);
        midi_client_ = 0;
    }
}
#else
// Non-macOS platforms
bool Visualizer::initMIDI() {
    return false;
}

void Visualizer::shutdownMIDI() {
}

void Visualizer::midiInputCallback(void* message, void* refCon) {
}
#endif

// プリセット保存
bool Visualizer::saveChannelPreset(int preset_num) {
    if (!state_ || preset_num < 1 || preset_num > 8) {
        return false;
    }
    
    // ファイル名を生成
    char filename[256];
    snprintf(filename, sizeof(filename), "ym2151_preset_%d.bin", preset_num);
    
    // チャンネル情報を取得
    YM2151State::Channel ch_info;
    state_->getChannelInfo(selected_channel_, ch_info);
    
    // ファイルに保存（バイナリ形式）
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        return false;
    }
    
    // ヘッダー（識別用）
    const char header[] = "YM2151PR";
    fwrite(header, 1, 8, fp);
    
    // バージョン
    uint32_t version = 1;
    fwrite(&version, sizeof(uint32_t), 1, fp);
    
    // チャンネル設定
    fwrite(&ch_info.left_right, sizeof(uint8_t), 1, fp);
    fwrite(&ch_info.feedback, sizeof(uint8_t), 1, fp);
    fwrite(&ch_info.algorithm, sizeof(uint8_t), 1, fp);
    fwrite(&ch_info.pms, sizeof(uint8_t), 1, fp);
    fwrite(&ch_info.ams, sizeof(uint8_t), 1, fp);
    
    // 4オペレータ分のデータ
    for (int op = 0; op < 4; op++) {
        const auto& op_info = ch_info.operators[op];
        fwrite(&op_info.detune1, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.multiple, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.total_level, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.key_scale, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.attack_rate, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.decay_rate, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.sustain_rate, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.sustain_level, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.release_rate, sizeof(uint8_t), 1, fp);
        fwrite(&op_info.detune2, sizeof(uint8_t), 1, fp);
    }
    
    fclose(fp);
    return true;
}

// プリセット読み込み
bool Visualizer::loadChannelPreset(int preset_num) {
    if (!opm_wrapper_ || preset_num < 1 || preset_num > 8) {
        return false;
    }
    
    // ファイル名を生成
    char filename[256];
    snprintf(filename, sizeof(filename), "ym2151_preset_%d.bin", preset_num);
    
    // ファイルから読み込み
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        return false;
    }
    
    // ヘッダー確認
    char header[8];
    if (fread(header, 1, 8, fp) != 8 || memcmp(header, "YM2151PR", 8) != 0) {
        fclose(fp);
        return false;
    }
    
    // バージョン確認
    uint32_t version;
    if (fread(&version, sizeof(uint32_t), 1, fp) != 1 || version != 1) {
        fclose(fp);
        return false;
    }
    
    // チャンネル設定を読み込み
    uint8_t left_right, feedback, algorithm, pms, ams;
    fread(&left_right, sizeof(uint8_t), 1, fp);
    fread(&feedback, sizeof(uint8_t), 1, fp);
    fread(&algorithm, sizeof(uint8_t), 1, fp);
    fread(&pms, sizeof(uint8_t), 1, fp);
    fread(&ams, sizeof(uint8_t), 1, fp);
    
    // オペレータデータを読み込み
    struct OpData {
        uint8_t detune1, multiple, total_level, key_scale;
        uint8_t attack_rate, decay_rate, sustain_rate;
        uint8_t sustain_level, release_rate, detune2;
    } op_data[4];
    
    for (int op = 0; op < 4; op++) {
        fread(&op_data[op].detune1, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].multiple, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].total_level, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].key_scale, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].attack_rate, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].decay_rate, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].sustain_rate, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].sustain_level, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].release_rate, sizeof(uint8_t), 1, fp);
        fread(&op_data[op].detune2, sizeof(uint8_t), 1, fp);
    }
    
    fclose(fp);
    
    // 全チャンネルにレジスタを設定
    OPMVisualizer* wrapper = (OPMVisualizer*)opm_wrapper_;
    
    for (int ch = 0; ch < 8; ch++) {
        // 0x20-0x27: RL/FB/CON
        wrapper->SetRegDirect(0x20 + ch, left_right | (feedback << 3) | algorithm);
        
        // 0x38-0x3F: PMS/AMS
        wrapper->SetRegDirect(0x38 + ch, (pms << 4) | ams);
        
        // オペレータパラメータ
        for (int op = 0; op < 4; op++) {
            const auto& op_d = op_data[op];
            
            // 0x40-0x5F: DT1/MUL
            wrapper->SetRegDirect(0x40 + (op << 3) + ch, (op_d.detune1 << 4) | op_d.multiple);
            
            // 0x60-0x7F: TL
            wrapper->SetRegDirect(0x60 + (op << 3) + ch, op_d.total_level);
            
            // プリセットTL値を保存（velocityのベース値）
            channel_keys_[ch].preset_tl[op] = op_d.total_level;
            
            // 0x80-0x9F: KS/AR
            wrapper->SetRegDirect(0x80 + (op << 3) + ch, (op_d.key_scale << 6) | op_d.attack_rate);
            
            // 0xA0-0xBF: D1R
            wrapper->SetRegDirect(0xA0 + (op << 3) + ch, op_d.decay_rate);
            
            // 0xC0-0xDF: DT2/D2R
            wrapper->SetRegDirect(0xC0 + (op << 3) + ch, (op_d.detune2 << 6) | op_d.sustain_rate);
            
            // 0xE0-0xFF: D1L/RR
            wrapper->SetRegDirect(0xE0 + (op << 3) + ch, (op_d.sustain_level << 4) | op_d.release_rate);
        }
    }
    
    return true;
}

void Visualizer::renderSpectrumSection(int x, int y, int width, int height,
                                       SpectrumAnalyzer* analyzer,
                                       int channel_index,
                                       bool write_debug) {
    if (!analyzer) {
        return;
    }

    SDL_Rect bg = {x, y, width, height};
    SDL_SetRenderDrawColor(renderer_, 18, 20, 28, 255);
    SDL_RenderFillRect(renderer_, &bg);

    bool output_debug = write_debug && spectrum_debug_file_ && channel_index >= 0;
    if (output_debug) {
        fprintf(spectrum_debug_file_, "CH%d: ", channel_index);
        for (int bar = 0; bar < 10; bar++) {
            float magnitude = analyzer->getMagnitude(bar);
            fprintf(spectrum_debug_file_, "[%d]=%.4f ", bar, magnitude);
        }
        fprintf(spectrum_debug_file_, "\n");
    }

    const int bin_count = analyzer->getBinCount();
    if (bin_count <= 0 || width <= 0 || height <= 4) {
        SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        SDL_RenderDrawRect(renderer_, &bg);
        return;
    }

    const int vertical_margin = 2;
    const int drawable_height = height - vertical_margin * 2;
    const int baseline = y + height - vertical_margin;

    // 目安となる水平グリッド
    SDL_SetRenderDrawColor(renderer_, 50, 52, 65, 255);
    for (int i = 1; i <= 4; ++i) {
        int grid_y = y + vertical_margin + (drawable_height * i) / 5;
        SDL_RenderDrawLine(renderer_, x, grid_y, x + width - 1, grid_y);
    }

    auto sampleMagnitude = [&](float normalized) {
        float bin_pos = normalized * (bin_count - 1);
        int base_bin = static_cast<int>(bin_pos);
        float frac = bin_pos - base_bin;
        float mag0 = analyzer->getMagnitude(base_bin);
        float mag1 = analyzer->getMagnitude(std::min(base_bin + 1, bin_count - 1));
        return mag0 + (mag1 - mag0) * frac;
    };

    for (int column = 0; column < width; ++column) {
        float normalized = (width > 1) ? static_cast<float>(column) / static_cast<float>(width - 1) : 0.0f;
        float magnitude = sampleMagnitude(normalized);
        int column_height = static_cast<int>(magnitude * drawable_height);
        if (column_height <= 0) {
            continue;
        }

        column_height = std::min(column_height, drawable_height);
        int column_top = std::max(baseline - column_height, y + vertical_margin);

        float intensity = std::min(1.0f, magnitude);
        Uint8 r = static_cast<Uint8>(std::min(255.0f, 80.0f + intensity * 360.0f));
        Uint8 g = static_cast<Uint8>(std::min(255.0f, 140.0f + intensity * 220.0f));
        Uint8 b = static_cast<Uint8>(std::min(255.0f, 200.0f + intensity * 140.0f));
        SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
        SDL_RenderDrawLine(renderer_, x + column, baseline, x + column, column_top);
    }

    SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
    SDL_RenderDrawRect(renderer_, &bg);
}

// スペクトラムアナライザの描画
void Visualizer::renderSpectrum() {
    const int SPECTRUM_X = 1100;  // 元のウィンドウ右端から開始
    const int SPECTRUM_WIDTH = 300;
    const int SPECTRUM_START_Y = YM2151_START_Y;
    
    // デバッグ出力（最初の10フレームのみ）
    bool do_debug = spectrum_debug_file_ && spectrum_debug_frame_count_ < 180;  // 3秒分（60fps想定）
    if (do_debug) {
        fprintf(spectrum_debug_file_, "=== Frame %d ===\n", spectrum_debug_frame_count_);
    }
    
    // YM2151チャンネルごとにスペクトラムを描画（各チャンネル波形と同じ位置/高さ）
    for (int ch = 0; ch < ym2151_display_channels_; ch++) {
        int base_y = SPECTRUM_START_Y + ch * YM2151_LINE_HEIGHT;
        int spectrum_y = base_y + 2;                  // waveformと同じオフセット
        int spectrum_height = YM2151_LINE_HEIGHT - 20; // waveformと同じ高さ
        
        renderSpectrumSection(SPECTRUM_X, spectrum_y, SPECTRUM_WIDTH, spectrum_height,
                              spectrum_analyzers_[ch], ch, do_debug);
    }  // for each YM2151 channel
    
    // ADPCMチャンネル用のスペクトラム
    if (adpcm_display_channels_ > 0) {
        int adpcm_start_y = SPECTRUM_START_Y + ym2151_display_channels_ * YM2151_LINE_HEIGHT;
        for (int ch = 0; ch < adpcm_display_channels_ && ch < 8; ch++) {
            int base_y = adpcm_start_y + ch * ADPCM_LINE_HEIGHT;
            int spectrum_y = base_y + 8;                    // ADPCM波形と同位置
            int spectrum_height = ADPCM_LINE_HEIGHT - 8;    // waveformと同じ高さ
            renderSpectrumSection(SPECTRUM_X, spectrum_y, SPECTRUM_WIDTH, spectrum_height,
                                  spectrum_analyzers_[8 + ch], 8 + ch, do_debug);
        }
    }

    // フレームカウンタを増やす
    if (do_debug) {
        spectrum_debug_frame_count_++;
    }
}
