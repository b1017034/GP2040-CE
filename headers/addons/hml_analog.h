// HML board analog addon: ADS8332 sticks + analog PS button + ADC triggers,
// with Firebird-style calibration, sensitivity curves and RC filter.

#ifndef _HMLAnalog_H_
#define _HMLAnalog_H_

#include "gpaddon.h"
#include "gamepad.h"
#include "GamepadEnums.h"
#include <ADS8332.h>

#ifndef HML_ANALOG_ENABLED
#define HML_ANALOG_ENABLED 0
#endif

#define HMLAnalogName "HMLAnalog"

#define HML_CURVE_MAX_POINTS 5

// Runtime state for one stick axis pair
struct HMLStickState {
    // filtered values (normalized -1..1)
    float filtX = 0.0f;
    float filtY = 0.0f;
    float prevRawX = 0.0f;
    float prevRawY = 0.0f;
};

class HMLAnalogAddon : public GPAddon {
public:
    virtual bool available();
    virtual void setup();
    virtual void preprocess();
    virtual void process();
    virtual void postprocess(bool sent) {}
    virtual void reinit() {}
    virtual std::string name() { return HMLAnalogName; }

    // Latest raw channel values, exposed for webconfig live view
    static uint16_t rawChannels[ADS8332_CHANNEL_COUNT];
    static uint16_t rawAdc[2]; // GPIO26, GPIO27
    static bool rawValid;

    // Synchronous read for webconfig mode (addon loop does not run there)
    static bool webRead();
    static HMLAnalogAddon* instance;

private:
    float calibrate(uint16_t raw, uint32_t mn, uint32_t ctr, uint32_t mx);
    float applyCurve(float mag, const void* stickOpts);
    void processStick(const void* stickOpts, HMLStickState& st, float rawX, float rawY, float& outX, float& outY);

    ADS8332 *ads = nullptr;
    HMLStickState leftState;
    HMLStickState rightState;
    bool adcInitialized = false;
    uint32_t lastMicros = 0;
};

#endif
