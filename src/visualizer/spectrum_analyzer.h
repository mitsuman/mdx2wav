#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <cmath>
#include <cstring>

class SpectrumAnalyzer {
public:
    static const int FFT_SIZE = 512;
    static const int NUM_BINS = FFT_SIZE / 2;  // FFTの有効ビン数
    
    SpectrumAnalyzer();
    ~SpectrumAnalyzer();
    
    // 音声サンプルを入力してFFTを計算
    void processAudio(const int16_t* samples, int count);
    
    // 周波数ビンの振幅を取得（0.0-1.0の範囲）
    float getMagnitude(int bin_index) const;

    // 利用可能なビン数を返す
    int getBinCount() const { return NUM_BINS; }
    
private:
    void performFFT();
    void hamming_window(float* data, int n);
    void fft(float* real, float* imag, int n);
    
    float input_buffer_[FFT_SIZE];
    float fft_real_[FFT_SIZE];
    float fft_imag_[FFT_SIZE];
    float magnitudes_[NUM_BINS];
    float smoothed_magnitudes_[NUM_BINS];
    int buffer_pos_;
    
    static const float SMOOTHING_FACTOR;
};

#endif // SPECTRUM_ANALYZER_H
