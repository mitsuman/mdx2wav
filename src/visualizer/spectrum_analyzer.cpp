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
    
    // FFT結果を各周波数ビンごとに保存
    for (int bin = 0; bin < NUM_BINS; bin++) {
        float magnitude = sqrtf(fft_real_[bin] * fft_real_[bin] +
                               fft_imag_[bin] * fft_imag_[bin]);
        magnitudes_[bin] = magnitude;

        // スムージング
        smoothed_magnitudes_[bin] = smoothed_magnitudes_[bin] * SMOOTHING_FACTOR +
                                    magnitude * (1.0f - SMOOTHING_FACTOR);
    }
}

float SpectrumAnalyzer::getMagnitude(int bin_index) const {
    if (bin_index < 0 || bin_index >= NUM_BINS) {
        return 0.0f;
    }
    
    // スムージングされた振幅を取得
    float magnitude = smoothed_magnitudes_[bin_index];
    
    // 対数スケール（dB）に変換して視認性を確保
    const float kMinDb = -60.0f;       // 下限（人間が感じにくいレベル）
    const float kReference = 1.0f;     // 0dB基準
    const float epsilon = 1e-6f;

    float magnitude_db = 20.0f * log10f(std::max(magnitude / kReference, epsilon));
    float normalized = (magnitude_db - kMinDb) / -kMinDb;  // kMinDb～0dBを0～1へ
    
    // 0.0～1.0の範囲にクランプ
    return std::max(0.0f, std::min(1.0f, normalized));
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
