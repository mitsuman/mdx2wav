#include "spectrum_analyzer.h"
#include <algorithm>

const float SpectrumAnalyzer::SMOOTHING_FACTOR = 0.5f;  // 反応を早くする

SpectrumAnalyzer::SpectrumAnalyzer() : buffer_pos_(0) {
    memset(input_buffer_, 0, sizeof(input_buffer_));
    memset(fft_real_, 0, sizeof(fft_real_));
    memset(fft_imag_, 0, sizeof(fft_imag_));
    memset(magnitudes_, 0, sizeof(magnitudes_));
    memset(smoothed_magnitudes_, 0, sizeof(smoothed_magnitudes_));
}

SpectrumAnalyzer::~SpectrumAnalyzer() {
}

void SpectrumAnalyzer::processAudio(const int16_t* samples, int count) {
    // ステレオサンプルをモノラルに変換してバッファに追加
    // samplesは[L, R, L, R, ...]の形式なので、countはサンプルペア数
    for (int i = 0; i < count; i++) {
        // L/Rの平均を取る
        float mono = (samples[i * 2] + samples[i * 2 + 1]) / 2.0f / 32768.0f;
        input_buffer_[buffer_pos_] = mono;
        buffer_pos_ = (buffer_pos_ + 1) % FFT_SIZE;
    }
    
    // FFT実行
    performFFT();
}

void SpectrumAnalyzer::performFFT() {
    // 入力バッファをコピーしてウィンドウ関数を適用
    float windowed[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = (buffer_pos_ + i) % FFT_SIZE;
        windowed[i] = input_buffer_[idx];
    }
    hamming_window(windowed, FFT_SIZE);
    
    // FFTバッファにコピー
    for (int i = 0; i < FFT_SIZE; i++) {
        fft_real_[i] = windowed[i];
        fft_imag_[i] = 0.0f;
    }
    
    // FFT実行
    fft(fft_real_, fft_imag_, FFT_SIZE);
    
    // 周波数ビンを対数スケールでバーに変換
    int bins_per_bar = (FFT_SIZE / 2) / NUM_BARS;
    for (int bar = 0; bar < NUM_BARS; bar++) {
        float sum = 0.0f;
        int start_bin = bar * bins_per_bar;
        int end_bin = start_bin + bins_per_bar;
        
        for (int bin = start_bin; bin < end_bin && bin < FFT_SIZE / 2; bin++) {
            float magnitude = sqrtf(fft_real_[bin] * fft_real_[bin] + 
                                   fft_imag_[bin] * fft_imag_[bin]);
            sum += magnitude;
        }
        
        magnitudes_[bar] = sum / bins_per_bar;
        
        // スムージング
        smoothed_magnitudes_[bar] = smoothed_magnitudes_[bar] * SMOOTHING_FACTOR +
                                   magnitudes_[bar] * (1.0f - SMOOTHING_FACTOR);
    }
}

float SpectrumAnalyzer::getMagnitude(int bar_index) const {
    if (bar_index < 0 || bar_index >= NUM_BARS) {
        return 0.0f;
    }
    
    // スムージングされた振幅を取得
    float magnitude = smoothed_magnitudes_[bar_index];
    
    // FFTの結果をスケーリング（感度を適度に調整）
    magnitude = magnitude * 3.0f;  // 10.0から3.0に減らす
    
    // 対数スケールで視認性向上
    if (magnitude > 0.001f) {
        magnitude = std::pow(magnitude, 0.7f);  // 0.6から0.7に調整
    }
    
    // 0.0～1.0の範囲にクランプ
    return std::max(0.0f, std::min(1.0f, magnitude));
}

void SpectrumAnalyzer::hamming_window(float* data, int n) {
    for (int i = 0; i < n; i++) {
        float w = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (n - 1));
        data[i] *= w;
    }
}

void SpectrumAnalyzer::fft(float* real, float* imag, int n) {
    // Cooley-Tukey FFT algorithm
    // ビット反転
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
        int k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
    
    // FFT本体
    for (int len = 2; len <= n; len *= 2) {
        float angle = -2.0f * M_PI / len;
        float wlen_real = cosf(angle);
        float wlen_imag = sinf(angle);
        
        for (int i = 0; i < n; i += len) {
            float w_real = 1.0f;
            float w_imag = 0.0f;
            
            for (int j = 0; j < len / 2; j++) {
                float u_real = real[i + j];
                float u_imag = imag[i + j];
                float v_real = real[i + j + len / 2] * w_real - imag[i + j + len / 2] * w_imag;
                float v_imag = real[i + j + len / 2] * w_imag + imag[i + j + len / 2] * w_real;
                
                real[i + j] = u_real + v_real;
                imag[i + j] = u_imag + v_imag;
                real[i + j + len / 2] = u_real - v_real;
                imag[i + j + len / 2] = u_imag - v_imag;
                
                float w_real_temp = w_real * wlen_real - w_imag * wlen_imag;
                w_imag = w_real * wlen_imag + w_imag * wlen_real;
                w_real = w_real_temp;
            }
        }
    }
}
