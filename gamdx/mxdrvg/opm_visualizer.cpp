#include "opm_visualizer.h"
#include "../../src/visualizer/ym2151_state.h"
#include "../../src/visualizer/visualizer.h"

OPMVisualizer::OPMVisualizer(OPM_Delegate* inner, YM2151State* state)
    : inner_(inner), state_(state), visualizer_(nullptr) {
}

OPMVisualizer::~OPMVisualizer() {
    // innerは外部で管理されるのでここでは削除しない
}

void OPMVisualizer::SetReg(uint addr, uint data) {
    // 状態を更新
    if (state_) {
        state_->updateRegister((uint8_t)addr, (uint8_t)data);
    }
    
    // YM2151ミュート時は実際のチップへの書き込みをブロック
    if (visualizer_ && visualizer_->isYM2151Muted()) {
        return;
    }
    
    // 実際のエミュレータに転送
    if (inner_) {
        inner_->SetReg(addr, data);
    }
}

void OPMVisualizer::SetRegDirect(uint addr, uint data) {
    // 状態を更新
    if (state_) {
        state_->updateRegister((uint8_t)addr, (uint8_t)data);
    }
    
    // ミュートチェックをスキップして実際のエミュレータに転送
    if (inner_) {
        inner_->SetReg(addr, data);
    }
}

void OPMVisualizer::Reset() {
    if (state_) {
        state_->reset();
    }
    if (inner_) {
        inner_->Reset();
    }
}

bool OPMVisualizer::Count(int32 us) {
    if (inner_) {
        return inner_->Count(us);
    }
    return false;
}

int32 OPMVisualizer::GetNextEvent() {
    if (inner_) {
        return inner_->GetNextEvent();
    }
    return 0x100000;
}

bool OPMVisualizer::Init(uint c, uint r, bool filter) {
    if (inner_) {
        return inner_->Init(c, r, filter);
    }
    return false;
}

uint OPMVisualizer::ReadStatus() {
    if (inner_) {
        return inner_->ReadStatus();
    }
    return 0;
}

void OPMVisualizer::Mix(short* buffer, int nsamples) {
    if (inner_) {
        inner_->Mix(buffer, nsamples);
    }
}

void OPMVisualizer::SetVolume(int db) {
    if (inner_) {
        inner_->SetVolume(db);
    }
}

void OPMVisualizer::SetIrqCallback(CALLBACK *callback) {
    if (inner_) {
        inner_->SetIrqCallback(callback);
    }
}
