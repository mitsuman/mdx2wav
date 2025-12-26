#include "visualizer.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <cstdio>
#include <algorithm>
#include <cmath>

// 汎用波形描画関数
void Visualizer::renderChannelWaveform(int x, int y, int width, int height, 
                                        const int16_t* waveform_data, int sample_count,
                                        int color_r, int color_g, int color_b, int ch_id,
                                        float waveform_scale) {
    // 波形の背景（暗い枠）
    SDL_SetRenderDrawColor(renderer_, 30, 30, 40, 255);
    SDL_Rect wave_bg = {x, y, width, height};
    SDL_RenderFillRect(renderer_, &wave_bg);
    SDL_SetRenderDrawColor(renderer_, 60, 60, 80, 255);
    SDL_RenderDrawRect(renderer_, &wave_bg);
    
    // 中央線（ゼロライン）
    int center_y = y + height / 2;
    SDL_SetRenderDrawColor(renderer_, 80, 80, 100, 128);
    SDL_RenderDrawLine(renderer_, x, center_y, x + width, center_y);
    
    // 波形を描画
    if (waveform_data && sample_count > 0) {
        int start_offset = findStableWaveformOffset(waveform_data, sample_count, width, ch_id);
        
        SDL_SetRenderDrawColor(renderer_, color_r, color_g, color_b, 255);
        int samples_to_draw = std::min(width, sample_count - start_offset);
        float adaptive_scale = waveform_scale;

        if (ch_id >= 0 && ch_id < MAX_CHANNELS && samples_to_draw > 0) {
            const float TARGET_OCCUPANCY = 0.92f;
            const float MIN_AUTO_SCALE = 0.2f;
            const float MAX_AUTO_SCALE = 8.0f;
            const float SILENCE_THRESHOLD = 0.0025f;
            const float ATTACK = 0.45f;   // 瞬間的なピークには素早く反応
            const float RELEASE = 0.08f;  // 静かなパッセージではゆっくり追従

            float peak = 0.0f;
            for (int i = 0; i < samples_to_draw; i++) {
                float sample = std::abs(waveform_data[start_offset + i]) / 32768.0f;
                if (sample > peak) {
                    peak = sample;
                }
            }

            float desired_auto = waveform_dynamic_scale_[ch_id];
            if (peak > SILENCE_THRESHOLD) {
                desired_auto = TARGET_OCCUPANCY / peak;
                if (desired_auto < MIN_AUTO_SCALE) desired_auto = MIN_AUTO_SCALE;
                if (desired_auto > MAX_AUTO_SCALE) desired_auto = MAX_AUTO_SCALE;
            } else {
                // 無音付近ではスケールをゆっくり持ち上げるだけに留める
                desired_auto = std::min(MAX_AUTO_SCALE, desired_auto * 1.01f + 0.01f);
            }

            float current_auto = waveform_dynamic_scale_[ch_id];
            float lerp = (desired_auto < current_auto) ? ATTACK : RELEASE;
            current_auto += (desired_auto - current_auto) * lerp;
            waveform_dynamic_scale_[ch_id] = current_auto;
            adaptive_scale = waveform_scale * current_auto;
        }
        for (int i = 0; i < samples_to_draw - 1; i++) {
            int y1 = center_y - (waveform_data[start_offset + i] * height * adaptive_scale / 2 / 32768);
            int y2 = center_y - (waveform_data[start_offset + i + 1] * height * adaptive_scale / 2 / 32768);
            
            SDL_RenderDrawLine(renderer_, x + i, y1, x + i + 1, y2);
        }
        
        // 次フレーム用に現在の波形を保存
        if (ch_id >= 0 && ch_id < MAX_CHANNELS && samples_to_draw > 0) {
            int copy_size = std::min(samples_to_draw, 200);
            memcpy(prev_waveform_[ch_id], waveform_data + start_offset, copy_size * sizeof(int16_t));
            prev_waveform_offset_[ch_id] = start_offset;
            has_prev_waveform_[ch_id] = true;
        }
    }
}

// 位相安定化: 前フレームとの相関を最大化するオフセットを探す
int Visualizer::findStableWaveformOffset(const int16_t* waveform_data, int sample_count, int width, int ch) {
    if (!waveform_data || sample_count <= width || ch >= MAX_CHANNELS) {
        return 0;
    }
    
    // 探索範囲: バッファの後半から
    int search_start = sample_count - width - 200;
    if (search_start < 0) search_start = 0;
    int search_end = sample_count - width;
    if (search_end <= search_start) return 0;
    
    // 前フレームがない場合は、ゼロクロス検出
    if (!has_prev_waveform_[ch]) {
        for (int i = search_start; i < search_end - 1; i++) {
            if (waveform_data[i] <= 0 && waveform_data[i + 1] > 0) {
                return i + 1;
            }
        }
        return search_start;
    }
    
    // 前フレームとの相関を計算して最適なオフセットを見つける
    int best_offset = prev_waveform_offset_[ch];
    int64_t best_correlation = INT64_MIN;
    
    // 前回のオフセット周辺を重点的に探索（±300サンプル、低周波対応）
    int search_range = 300;
    int offset_start = std::max(search_start, prev_waveform_offset_[ch] - search_range);
    int offset_end = std::min(search_end, prev_waveform_offset_[ch] + search_range);
    
    for (int offset = offset_start; offset < offset_end; offset++) {
        // 相関を計算（フルwidth=256サンプルで比較、低周波対応）
        int compare_len = std::min(256, width);
        int64_t correlation = 0;
        
        for (int i = 0; i < compare_len; i++) {
            correlation += (int64_t)waveform_data[offset + i] * (int64_t)prev_waveform_[ch][i];
        }
        
        // より良い相関が見つかった場合
        if (correlation > best_correlation) {
            best_correlation = correlation;
            best_offset = offset;
        }
    }
    
    // 相関が非常に低い場合（大きく変化した）、ゼロクロスで再初期化
    if (best_correlation < 1000000) {
        for (int i = search_start; i < search_end - 1; i++) {
            if (waveform_data[i] <= 0 && waveform_data[i + 1] > 0) {
                return i + 1;
            }
        }
    }
    
    return best_offset;
}

// 7セグメント表示
//   a
//  f b
//   g
//  e c
//   d
void Visualizer::draw7Segment(int x, int y, int digit, int r, int g, int b, int seg_width, int seg_height) {
    // 各数字のセグメント点灯パターン (a,b,c,d,e,f,g)
    static const bool segments[10][7] = {
        {1,1,1,1,1,1,0}, // 0
        {0,1,1,0,0,0,0}, // 1
        {1,1,0,1,1,0,1}, // 2
        {1,1,1,1,0,0,1}, // 3
        {0,1,1,0,0,1,1}, // 4
        {1,0,1,1,0,1,1}, // 5
        {1,0,1,1,1,1,1}, // 6
        {1,1,1,0,0,0,0}, // 7
        {1,1,1,1,1,1,1}, // 8
        {1,1,1,1,0,1,1}  // 9
    };
    
    if (digit < 0 || digit > 9) return;
    
    int seg_thick = 2;  // セグメントの太さ
    int gap = 1;  // セグメント間の隙間
    
    // オフ時の暗い色
    int off_r = r / 8;
    int off_g = g / 8;
    int off_b = b / 8;
    
    // セグメントa (上横)
    SDL_SetRenderDrawColor(renderer_, 
        segments[digit][0] ? r : off_r,
        segments[digit][0] ? g : off_g,
        segments[digit][0] ? b : off_b, 255);
    SDL_Rect seg_a = {x + gap, y, seg_width - gap * 2, seg_thick};
    SDL_RenderFillRect(renderer_, &seg_a);
    
    // セグメントb (右上縦)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][1] ? r : off_r,
        segments[digit][1] ? g : off_g,
        segments[digit][1] ? b : off_b, 255);
    SDL_Rect seg_b = {x + seg_width - seg_thick, y + gap, seg_thick, seg_height - gap};
    SDL_RenderFillRect(renderer_, &seg_b);
    
    // セグメントc (右下縦)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][2] ? r : off_r,
        segments[digit][2] ? g : off_g,
        segments[digit][2] ? b : off_b, 255);
    SDL_Rect seg_c = {x + seg_width - seg_thick, y + seg_height + gap, seg_thick, seg_height - gap};
    SDL_RenderFillRect(renderer_, &seg_c);
    
    // セグメントd (下横)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][3] ? r : off_r,
        segments[digit][3] ? g : off_g,
        segments[digit][3] ? b : off_b, 255);
    SDL_Rect seg_d = {x + gap, y + seg_height * 2, seg_width - gap * 2, seg_thick};
    SDL_RenderFillRect(renderer_, &seg_d);
    
    // セグメントe (左下縦)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][4] ? r : off_r,
        segments[digit][4] ? g : off_g,
        segments[digit][4] ? b : off_b, 255);
    SDL_Rect seg_e = {x, y + seg_height + gap, seg_thick, seg_height - gap};
    SDL_RenderFillRect(renderer_, &seg_e);
    
    // セグメントf (左上縦)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][5] ? r : off_r,
        segments[digit][5] ? g : off_g,
        segments[digit][5] ? b : off_b, 255);
    SDL_Rect seg_f = {x, y + gap, seg_thick, seg_height - gap};
    SDL_RenderFillRect(renderer_, &seg_f);
    
    // セグメントg (中央横)
    SDL_SetRenderDrawColor(renderer_,
        segments[digit][6] ? r : off_r,
        segments[digit][6] ? g : off_g,
        segments[digit][6] ? b : off_b, 255);
    SDL_Rect seg_g = {x + gap, y + seg_height, seg_width - gap * 2, seg_thick};
    SDL_RenderFillRect(renderer_, &seg_g);
}

// チャンネル色を取得（アルゴリズムベース）
void Visualizer::getChannelColor(int ch, int& r, int& g, int& b) {
    // アルゴリズム番号で異なる色を割り当て（青紫系）
    static const int colors[8][3] = {
        {100, 150, 255},  // ALG0: 明るい青
        {150, 100, 255},  // ALG1: 青紫
        {100, 200, 255},  // ALG2: シアン青
        {200, 150, 255},  // ALG3: ライラック
        {120, 180, 255},  // ALG4: スカイブルー
        {180, 120, 255},  // ALG5: 薄紫
        {100, 220, 200},  // ALG6: ターコイズ
        {160, 160, 255},  // ALG7: ペリウィンクル
    };
    
    if (ch >= 0 && ch < 8) {
        r = colors[ch][0];
        g = colors[ch][1];
        b = colors[ch][2];
    } else {
        r = g = b = 128;
    }
}

// EGフェーズの色を取得
void Visualizer::getEGPhaseColor(int phase, int& r, int& g, int& b) {
    // EGフェーズごとの色
    switch (phase) {
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
}

// ノート名を取得
const char* Visualizer::getNoteName(int note) {
    static const char* note_names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return note_names[note % 12];
}

// TTFフォントでテキストを描画
void renderText(SDL_Renderer* renderer, void* font_ptr, const char* text, 
                int x, int y, int r, int g, int b) {
    TTF_Font* font = (TTF_Font*)font_ptr;
    if (!font || !text) return;
    
    // 太字スタイルを設定（デジタル/LED風）
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    
    // テキストを描画
    SDL_Color color = {(Uint8)r, (Uint8)g, (Uint8)b, 255};
    SDL_Surface* surface = TTF_RenderText_Blended(font, text, color);
    if (!surface) {
        TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
        return;
    }
    
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    
    SDL_FreeSurface(surface);
    
    // スタイルを戻す
    TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
}
