#include "opm_delegate.h"
#include "../fmgen/opm.h"

class OPMFmgen :public OPM_Delegate {
public:
  OPMFmgen() {}

protected:
  virtual ~OPMFmgen() {}

  virtual void Reset() {
    OPM.Reset();
  }

  virtual bool Count(int32 us) {
    OPM.Count(us);
  }

  virtual int32 GetNextEvent() {
    return OPM.GetNextEvent();
  }

  virtual bool Init(uint c, uint r, bool f) {
    OPM.Init(c, r, f);
  }

  virtual void SetReg(uint addr, uint data) {
    OPM.SetReg(addr, data);
  }

  /*virtual uint GetReg(uint addr) {
    return OPM.GetReg(addr);
    }*/

  virtual uint ReadStatus() {
    return OPM.ReadStatus();
  }

  virtual void Mix(short* buffer, int nsamples) {
    OPM.Mix(buffer, nsamples);
  }

  virtual void SetVolume(int db) {
    OPM.SetVolume(db);
  }

  virtual void SetIrqCallback(CALLBACK *cb) {
    OPM.callback = cb;
  }

private:
  class X68OPM : public FM::OPM {
  public:
    virtual void Intr(bool irq) {
      if (irq) {
        if (callback) {
          callback();
        }
      }
    }
    CALLBACK *callback;
  };
  X68OPM OPM;
};

OPM_Delegate *OPM_Delegate::getFmgen() {
  return new OPMFmgen();
}

