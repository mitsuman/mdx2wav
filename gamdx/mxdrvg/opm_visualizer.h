#ifndef OPM_VISUALIZER_H
#define OPM_VISUALIZER_H

#include "opm_delegate.h"

// 前方宣言
class YM2151State;
class Visualizer;

// 既存のOPM_Delegateをラップして、SetRegをフックするクラス
class OPMVisualizer : public OPM_Delegate {
public:
    OPMVisualizer(OPM_Delegate* inner, YM2151State* state);
    
    // Visualizerを設定（ミュート判定用）
    void setVisualizer(Visualizer* vis) { visualizer_ = vis; }
    virtual ~OPMVisualizer();
    
    // SetRegをオーバーライドしてレジスタ書き込みをフック
    virtual void SetReg(uint addr, uint data) override;
    
    // ミュートをバイパスして直接レジスタ書き込み（キーボード演奏用）
    void SetRegDirect(uint addr, uint data);
    
    // その他のメソッドは全てinnerに委譲
    virtual void Reset() override;
    virtual bool Count(int32 us) override;
    virtual int32 GetNextEvent() override;
    virtual bool Init(uint c, uint r, bool filter = false) override;
    virtual uint ReadStatus() override;
    virtual void Mix(short* buffer, int nsamples) override;
    virtual void SetVolume(int db) override;
    virtual void SetIrqCallback(CALLBACK *callback) override;

private:
    OPM_Delegate* inner_;  // 実際のエミュレータ (fmgen or mame)
    YM2151State* state_;   // 状態管理オブジェクト
    Visualizer* visualizer_;  // ビジュアライザ（ミュート判定用）
};

#endif // OPM_VISUALIZER_H
