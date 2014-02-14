#include "opm_delegate.h"
#include "../fmgen/opm.h"

extern "C" {
#include "../mame/ym2151.h"
}

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

class OPMMame :public OPM_Delegate {
public:
  OPMMame() {}

protected:
  virtual ~OPMMame() {
    ym2151_shutdown(chip);
  }

  virtual void Reset() {
    ym2151_reset_chip(chip);
  }

  virtual bool Count(int32 us) {
    return false;
  }

  virtual int32 GetNextEvent() {
    return 0x100000;
  }

  virtual bool Init(uint c, uint r, bool f) {
    chip = ym2151_init(this, c, r);
    return true;
  }

  virtual void SetReg(uint addr, uint data) {
    ym2151_write_reg(chip, addr, data);
  }

  /*virtual uint GetReg(uint addr) {
    return OPM.GetReg(addr);
    }*/

  virtual uint ReadStatus() {
    return ym2151_read_status(chip);
  }

  virtual void Mix(short* buffer, int nsamples) {
    ym2151_update_one(chip, buffer, nsamples);
  }

  virtual void SetVolume(int db) {
    ym2151_set_volume(chip, db);
  }

  static void irqHandler(device_t *device, int irq) {
    CALLBACK *cb = ((OPMMame*)device)->callback;
    if (cb && irq == 1) {
      cb();
    }
  }

  virtual void SetIrqCallback(CALLBACK *cb) {
    callback = cb;
    ym2151_set_irq_handler(chip, irqHandler);
  }

private:
  void *chip;
  CALLBACK *callback;
};

OPM_Delegate *OPM_Delegate::getMame() {
  return new OPMMame();
}
