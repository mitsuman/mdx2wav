#ifndef PCM8_VISUALIZER_H
#define PCM8_VISUALIZER_H

#include "../pcm8/x68pcm8.h"

// 前方宣言
class YM2151State;

// X68PCM8をラップしてOut()をフックするクラス (委譲パターン)
class PCM8Visualizer : public X68K::X68PCM8 {
public:
    PCM8Visualizer(YM2151State* state);
    ~PCM8Visualizer();
    
    // 全メソッドをオーバーライド
    bool Init(uint rate);
    bool SetRate(uint rate);
    void Reset();
    int Out(int ch, void *adrs, int mode, int len);
    void Abort();
    void SetChannelMask(uint mask);
    void SetVolume(int db);
    void Mix(X68K::Sample* buffer, int ndata);

private:
    YM2151State* state_;   // 状態管理オブジェクト
};

#endif // PCM8_VISUALIZER_H
