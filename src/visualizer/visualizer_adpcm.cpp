#include "visualizer.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <cstdio>
#include <algorithm>

// ADPCMチャンネルの描画
void Visualizer::renderADPCMChannels(int& y) {
    if (!state_) return;
    
    TTF_Font* font_sm = (TTF_Font*)font_small_;
    
    YM2151State::ADPCMChannel adpcm_channels[8];
    state_->getAllADPCMChannels(adpcm_channels);
    
    int adpcm_line_height = 45;  // ADPCMは縦に詰める
    
    for (int ch = 8; ch < 16; ch++) {
        int adpcm_ch = ch - 8;
        YM2151State::ADPCMChannel& ac = adpcm_channels[adpcm_ch];
        
        // ピーク情報を更新
        auto& peak = adpcm_peaks_[adpcm_ch];
        if (ac.key_on) {
            // キーオン時：ピーク更新とタイマー開始
            if (peak.keyon_time == 0 || ac.initial_address != peak.start_address) {
                //printf("ADPCM Ch %d Key On: Init=0x%016lX Cur=0x%016lX Vol=%d\n", adpcm_ch, (unsigned long)ac.initial_address, (unsigned long)ac.address, ac.volume);
                // 新しいキーオン、または異なるアドレスでの再生開始
                peak.peak_volume = ac.volume;
                peak.keyon_time = frame_count_;
                peak.start_address = ac.initial_address;
                peak.end_address = ac.initial_address + ac.length;
                peak.last_length = ac.length;
            } else if (ac.volume > peak.peak_volume) {
                peak.peak_volume = ac.volume;
            }
        } else {
            // キーオフ後、徐々にピークを減衰
            if (peak.peak_volume > 0) {
                // 30フレーム（約0.5秒）でピークをゼロに
                uint32_t frames_since_keyon = frame_count_ - peak.keyon_time;
                if (frames_since_keyon > 30) {
                    peak.peak_volume = 0;
                    peak.keyon_time = 0;
                    peak.start_address = 0;
                    peak.end_address = 0;
                }
            }
        }
        
        // 7セグメント表示でチャンネル番号を表示
        int seg_x = 20;
        int seg_y = y + 10;
        int seg_w = 8;
        int seg_h = 7;
        
        // チャンネル情報は固定色（オレンジ）
        int seg_r, seg_g, seg_b;
        if (ac.key_on) {
            seg_r = 255;
            seg_g = 180;
            seg_b = 100;
        } else {
            seg_r = 120;
            seg_g = 80;
            seg_b = 40;
        }
        
        // チャンネル番号を7セグメント表示 (8-15 を 0-7 として表示)
        draw7Segment(seg_x, seg_y, adpcm_ch, seg_r, seg_g, seg_b, seg_w, seg_h);
        
        // "ADPCM"ラベルを表示（チャンネル番号の下）
        if (bitmap_font_texture_) {
            renderBitmapText("ADPCM", 13, seg_y + 18, seg_r, seg_g, seg_b);
        } else if (font_sm) {
            renderText(renderer_, font_sm, "ADPCM", 13, seg_y + 16, seg_r, seg_g, seg_b);
        }
        
        // 音量バーを縦方向に表示 (YM2151スタイル)
        int vol_x = 70;
        int vol_y = y + 5;
        int vol_width = 10;
        int vol_height = 35;  // ADPCM用の高さ
        
        // ピーク表示（ピークホールド）
        if(0)if (peak.peak_volume > 0) {
            float peak_ratio = peak.peak_volume / 15.0f;  // 0-15を0.0-1.0に
            int peak_h = (int)(peak_ratio * vol_height);
            
            // キーオンからの経過時間で色を変える（減衰表示）
            uint32_t frames_since_keyon = frame_count_ - peak.keyon_time;
            int decay_alpha = 255;
            if (frames_since_keyon > 15) {
                decay_alpha = 255 - ((frames_since_keyon - 15) * 255 / 15);
                if (decay_alpha < 0) decay_alpha = 0;
            }
            
            // ピークバーの色（アドレスから生成、少し明るめ）
            int peak_r, peak_g, peak_b;
            if (peak.start_address > 0) {
                getColorFromAddress(peak.start_address, peak_r, peak_g, peak_b);
                // ピークは少し明るくする
                peak_r = std::min(255, peak_r * 11 / 10);
                peak_g = std::min(255, peak_g * 11 / 10);
                peak_b = std::min(255, peak_b * 11 / 10);
            } else {
                peak_r = 255;
                peak_g = 200;
                peak_b = 100;
            }
            
            SDL_SetRenderDrawColor(renderer_, peak_r, peak_g, peak_b, decay_alpha);
            SDL_Rect peak_rect = {vol_x, vol_y + vol_height - peak_h, vol_width, peak_h};
            SDL_RenderFillRect(renderer_, &peak_rect);
        }
        
        // 現在の音量表示（アドレスベースの色）
        if (ac.key_on && ac.volume > 0) {
            float vol_ratio = 1.0f;//ac.volume / 15.0f;  // 0-15を0.0-1.0に
            int vol_h = (int)(vol_ratio * vol_height);
            
            // ボリュームバーはアドレスから色を生成
            int vol_r, vol_g, vol_b;
            if (peak.start_address > 0) {
                getColorFromAddress(peak.start_address, vol_r, vol_g, vol_b);
            } else {
                vol_r = 255;
                vol_g = 180;
                vol_b = 100;
            }
            
            SDL_SetRenderDrawColor(renderer_, vol_r, vol_g, vol_b, 255);
            SDL_Rect vol_rect = {vol_x, vol_y + vol_height - vol_h, vol_width, vol_h};
            SDL_RenderFillRect(renderer_, &vol_rect);
        }
        
        // 音量バーの枠
        #if 0
        SDL_SetRenderDrawColor(renderer_, 60, 60, 80, 255);
        SDL_Rect vol_frame = {vol_x, vol_y, vol_width, vol_height};
        SDL_RenderDrawRect(renderer_, &vol_frame);
        #endif
        
        // アドレス範囲とLEN（サンプル長）を横方向のバーで可視化
        int len_x = 85;
        int len_y = y + 8;
        int len_max_width = 400;  // 最大表示幅
        int len_height = 6;       // バーの高さ

        void* adpcm_buffer = state_->getADPCMBuffer();
        uint32_t adpcm_buffer_size = state_->getADPCMBufferSize();
        
        if (peak.start_address > 0 && peak.end_address > peak.start_address) {
            // 全体の長さ（起点から終点まで）
            uint32_t total_length = peak.end_address - peak.start_address;
            
            // 全体バーの背景（暗いグレー）
            SDL_SetRenderDrawColor(renderer_, 40, 40, 50, 255);
            SDL_Rect total_rect = {len_x, len_y, len_max_width, len_height};
            SDL_RenderFillRect(renderer_, &total_rect);
            
            // 枠線
            SDL_SetRenderDrawColor(renderer_, 80, 80, 90, 255);
            SDL_RenderDrawRect(renderer_, &total_rect);
            
            if (ac.key_on && ac.address >= peak.start_address) {
                // 現在の再生位置（相対位置）
                uint32_t current_offset = ac.address - peak.start_address;
                float position_ratio = 1.0f - (float)current_offset / (float)total_length;
                float vol = ac.volume / 16.0f;
                if (vol > 1.0f) vol = 1.0f;
                position_ratio *= vol;  // 音量に応じて位置を調整
                if (position_ratio > 1.0f) position_ratio = 1.0f;
                
                // 再生残り部分（明るいオレンジ）
                int played_width = (int)(position_ratio * len_max_width);
                if (played_width > 0) {
                    SDL_SetRenderDrawColor(renderer_, seg_r, seg_g, seg_b, 255);
                    SDL_Rect played_rect = {len_x, len_y, played_width, len_height};
                    SDL_RenderFillRect(renderer_, &played_rect);
                }
                
                // 現在位置のマーカー（白い縦線）
                if (1) {
                    int vol_pos = (int)(vol * len_max_width);
                    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
                    SDL_RenderDrawLine(renderer_, len_x + vol_pos, len_y , 
                                      len_x + vol_pos, len_y + len_height-1);
                }
            }
            
            // アドレス情報をテキスト表示（初回/現在の両方）
            char addr_text[128];
            if (ac.key_on) {
                float progress = 0.0f;
                if (ac.address >= peak.start_address && total_length > 0) {
                    progress = ((float)(ac.address - peak.start_address) / (float)total_length) * 100.0f;
                }
                snprintf(addr_text, sizeof(addr_text), "I:%08X C:%08X (%3.0f%%)", 
                        (unsigned long)ac.initial_address-(unsigned long)adpcm_buffer, (unsigned long)ac.address-(unsigned long)adpcm_buffer, progress);
            } else {
                snprintf(addr_text, sizeof(addr_text), "I:%08X C:%08X", 
                        (unsigned long)peak.start_address-(unsigned long)adpcm_buffer, (unsigned long)peak.start_address-(unsigned long)adpcm_buffer);
            }
            
            renderBitmapText(addr_text, len_x, len_y + len_height + 2, seg_r, seg_g, seg_b);
        } else if (ac.key_on && ac.length > 0) {
            // まだ起点が記録されていない場合は従来の表示
            char len_text[32];
            snprintf(len_text, sizeof(len_text), "LEN:%08X", ac.length);
            renderBitmapText(len_text, len_x, len_y, seg_r, seg_g, seg_b);
        }
        
        // 全体ADPCMバッファ内での位置を可視化        
        if (adpcm_buffer && adpcm_buffer_size > 0 && ac.key_on && ac.address > 0) {
            uintptr_t buffer_start = reinterpret_cast<uintptr_t>(adpcm_buffer);
            uintptr_t buffer_end = buffer_start + adpcm_buffer_size;
            
            // 現在のアドレスが全体バッファ内にあるか確認
            if (ac.address >= buffer_start && ac.address < buffer_end) {
                int global_bar_x = len_x;
                int global_bar_y = len_y + 16;
                int global_bar_width = len_max_width;
                int global_bar_height = 3;
                
                // 全体バッファの背景（非常に暗いグレー）
                SDL_SetRenderDrawColor(renderer_, 30, 30, 35, 255);
                SDL_Rect global_bg = {global_bar_x, global_bar_y, global_bar_width, global_bar_height};
                SDL_RenderFillRect(renderer_, &global_bg);
                
                {
                    uintptr_t offset_in_buffer = ac.initial_address - buffer_start;
                    float global_position = (float)offset_in_buffer / (float)adpcm_buffer_size;
                    if (global_position > 1.0f) global_position = 1.0f;
                    int marker_x = global_bar_x + (int)(global_position * global_bar_width);

                    // マーカー（明るい青）
                    SDL_SetRenderDrawColor(renderer_, 100, 180, 200, 255);
                    SDL_RenderDrawLine(renderer_, marker_x, global_bar_y, marker_x, global_bar_y + global_bar_height);
                    SDL_RenderDrawLine(renderer_, marker_x - 1, global_bar_y, marker_x - 1, global_bar_y + global_bar_height);
                    SDL_RenderDrawLine(renderer_, marker_x + 1, global_bar_y, marker_x + 1, global_bar_y + global_bar_height);
                }

                // 現在の再生位置（全体バッファ内での相対位置）
                uintptr_t offset_in_buffer = ac.address - buffer_start;
                float global_position = (float)offset_in_buffer / (float)adpcm_buffer_size;
                if (global_position > 1.0f) global_position = 1.0f;
                
                int marker_x = global_bar_x + (int)(global_position * global_bar_width);
                
                // マーカー（明るい青）
                SDL_SetRenderDrawColor(renderer_, 100, 180, 255, 255);
                SDL_RenderDrawLine(renderer_, marker_x, global_bar_y, marker_x, global_bar_y + global_bar_height);
                SDL_RenderDrawLine(renderer_, marker_x - 1, global_bar_y, marker_x - 1, global_bar_y + global_bar_height);
                SDL_RenderDrawLine(renderer_, marker_x + 1, global_bar_y, marker_x + 1, global_bar_y + global_bar_height);
                
                // 全体バッファでの位置情報を表示
                #if 0
                char global_info[64];
                snprintf(global_info, sizeof(global_info), "BUF:%3.1f%%", global_position * 100.0f);
                renderBitmapText(global_info, len_x, global_bar_y + global_bar_height + 1, 100, 180, 255);
                #endif
            }
        }
        
        // ADPCM詳細情報を表示
        char info[256];
        if (ac.key_on) {
            snprintf(info, sizeof(info), "VOL:%02d RATE:%5d MODE:%02X",
                     ac.volume, ac.rate, ac.mode);
        } else {
            snprintf(info, sizeof(info), "");
        }
        
        renderBitmapText(info, len_x, len_y + 26, seg_r/2, seg_g/2, seg_b/2);
        
        // ADPCM波形表示（YM2151と同じ位置）
        int waveform_x = CHANNEL_WAVEFORM_X;
        int waveform_y = len_y;
        int waveform_width = CHANNEL_WAVEFORM_WIDTH;
        int waveform_height = adpcm_line_height - 8;
        
        // 波形データを描画
        if (ac.key_on) {
            // アドレスから色を取得（一貫性のため）
            int color_r, color_g, color_b;
            getColorFromAddress((unsigned long)ac.initial_address, color_r, color_g, color_b);
            
            // 汎用波形描画関数を使用
            renderChannelWaveform(waveform_x, waveform_y, waveform_width, waveform_height,
                                ac.waveform, 200, color_r, color_g, color_b);
        } else {
            // キーオフ時は空の波形枠のみ表示
            renderChannelWaveform(waveform_x, waveform_y, waveform_width, waveform_height,
                                nullptr, 0, 60, 60, 80);
        }
        
        y += adpcm_line_height;
    }
}
