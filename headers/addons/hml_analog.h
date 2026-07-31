// HML board analog addon: ADS8332 sticks + analog PS button + ADC triggers,
// with Firebird-style calibration, sensitivity curves and RC filter.

#ifndef _HMLAnalog_H_
#define _HMLAnalog_H_

#include "gpaddon.h"
#include "gamepad.h"
#include "GamepadEnums.h"
#include <ADS8332.h>
#include "NeoPico.h"

#ifndef HML_ANALOG_ENABLED
#define HML_ANALOG_ENABLED 0
#endif

#define HMLAnalogName "HMLAnalog"

#define HML_CURVE_MAX_POINTS 10
#define HML_RC_CURVE_MAX_POINTS 7
#define HML_DELAY_SLOTS 40

// Runtime state for one stick axis pair
struct HMLStickState {
    // filtered values (normalized -1..1)
    float filtX = 0.0f;
    float filtY = 0.0f;
    float prevRawX = 0.0f;
    float prevRawY = 0.0f;
    // snapback filter state (return-phase low-pass)
    float snapX = 0.0f;
    float snapY = 0.0f;
    // noise gate: last held raw counts (Firebird-style stabilization)
    uint16_t heldX = 0;
    uint16_t heldY = 0;
    bool heldInit = false;
    // FireBird 追加入力遅延: output ring buffer
    float dlyX[HML_DELAY_SLOTS] = {0};
    float dlyY[HML_DELAY_SLOTS] = {0};
    uint32_t dlyT[HML_DELAY_SLOTS] = {0};
    uint8_t dlyHead = 0;
    uint8_t dlyCount = 0;
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
    // Burst-sample all channels at rest and report per-channel min/max
    // (webconfig noise measurement for the stabilization gate)
    static bool noiseSample(uint16_t* mn, uint16_t* mx, int iterations);
    // Drive the lightbar from the webconfig loop (Core0 add-ons are skipped there)
    static void lightbarTick();
    static HMLAnalogAddon* instance;
    ADS8332* getAds() { return ads; }

private:
    float calibrate(uint16_t raw, uint32_t mn, uint32_t ctr, uint32_t mx);
    float applyCurve(float mag, const void* stickOpts);
    void processStick(const void* stickOpts, HMLStickState& st, float rawX, float rawY, float& outX, float& outY);
    void emitStick(const void* stickOpts, HMLStickState& st, float x, float y, float& outX, float& outY);
    // Stick source when the board has no ADS8332: tune whatever a USB host
    // add-on already put in gamepad state (DualSense on the MeisterConverter)
    void processGamepadState();

    ADS8332 *ads = nullptr;
    bool adcSource = false; // true = ADS8332 sticks, false = gamepad state
    HMLStickState leftState;
    HMLStickState rightState;
    bool adcInitialized = false;
    uint32_t lastMicros = 0;
    // Self-heal: retry ADC link bring-up (a warm reboot can leave the chip
    // desynced and the first probe may fail)
    uint32_t adsRetryMs = 0;

    // Dedicated single-WS2812 lightbar driven from the console-sent color
    void lightbarSetup();
    void lightbarUpdate();
    NeoPico *lightbar = nullptr;
    uint32_t lightbarLastColor = 0xFFFFFFFF; // force first write
    uint32_t lightbarNextMs = 0;
    uint32_t lightbarForceMs = 0; // periodic rewrite even when unchanged
};

#endif
