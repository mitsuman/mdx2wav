#if !defined(__OPM_DELEGTE__)
#define __OPM_DELEGTE__

#include "../types.h"

class OPM_Delegate {
public:
  static OPM_Delegate *getFmgen();
  static OPM_Delegate *getMame();
  typedef void CALLBACK(void);

  virtual ~OPM_Delegate() {}

  // Timer
  virtual void Reset() = 0;
  virtual bool Count(int32 us) = 0;
  virtual int32 GetNextEvent() = 0;

  // OPM
  virtual bool Init(uint c, uint r, bool filter = false) = 0;
  //virtual void SetLPFCutoff(uint freq) = 0;
  virtual void SetReg(uint addr, uint data) = 0;
  //virtual uint GetReg(uint addr) = 0;
  virtual uint ReadStatus() = 0;
  virtual void Mix(short* buffer, int nsamples) = 0;
  virtual void SetVolume(int db) = 0;
  //virtual void SetChannelMask(uint mask) = 0;
  virtual void SetIrqCallback(CALLBACK *callback) = 0;
};

#endif
