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

// Analog trigger calibration in raw ADC counts (pressed pulls the line low).
// Board-specific and only meaningful when ltAdcPin/rtAdcPin are assigned, so
// boards without hall triggers - e.g. the MeisterConverter, whose triggers
// arrive over USB already scaled - just take these inert defaults.
#ifndef HML_TRIGGER_LT_IDLE
#define HML_TRIGGER_LT_IDLE 4095
#endif
#ifndef HML_TRIGGER_LT_PRESSED
#define HML_TRIGGER_LT_PRESSED 0
#endif
#ifndef HML_TRIGGER_RT_IDLE
#define HML_TRIGGER_RT_IDLE 4095
#endif
#ifndef HML_TRIGGER_RT_PRESSED
#define HML_TRIGGER_RT_PRESSED 0
#endif
#ifndef HML_TRIGGER_DIGITAL_PERCENT
#define HML_TRIGGER_DIGITAL_PERCENT 40
#endif

#define HMLAnalogName "HMLAnalog"

// Directions the gate is measured in. 16 (22.5 apart) is a deliberate ceiling:
// finer bins need a slower, more careful sweep to fill, and unfilled bins have
// to be interpolated, which costs more accuracy than the resolution gains.
#define HML_GATE_BINS 16

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
    // Derivative terms, refreshed on a new input sample and held in between.
    // The stick source can be slower than the add-on loop (a USB controller
    // reports at ~250 Hz), and recomputing these against a repeated sample
    // yields zero velocity on every frame but one, then a burst on that frame.
    // dRawX/dRawY are the raw delta and dRawDtMs the interval it spans, kept
    // unreduced so the velocity magnitude is still computed as
    // sqrt(dx*dx+dy*dy)/dt - dividing first would change the rounding, and the
    // ADS8332 path has to stay numerically identical to before. leadX/leadY
    // are what the FireBird velocity-lead term consumes, in its per-frame units.
    float dRawX = 0.0f;
    float dRawY = 0.0f;
    float dRawDtMs = 1.0f;
    float leadX = 0.0f;
    float leadY = 0.0f;
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

    // ---- Gate calibration ----
    // Capture runs in the web-config loop, where the USB host is serviced but
    // the add-on chain is not, so gamepad state holds the untouched controller
    // values. Sampling has to happen at loop rate - polling it over HTTP would
    // miss the brief moments the stick is actually against the gate.
    static void calStart(uint8_t stick);   // 0 = left, 1 = right
    static void calAbort();
    static bool calActive();
    static void calTick();                 // one sample; call every loop
    // Copy the captured sweep into the profile for this controller, replacing
    // any existing entry for the VID/PID. False if coverage is too poor.
    static bool calSave(uint16_t vendorId, uint16_t productId);
    // Live capture state for the UI
    static uint8_t  calCoverage();         // percent of directions measured
    static uint16_t calRadius[HML_GATE_BINS];
    static uint16_t calCenter[2];
    static uint8_t  calStick;

    // Map a raw stick pair through a measured gate onto a unit circle. Leaves
    // the values alone when no profile matches.
    static bool applyGate(const void* gateV, uint16_t rawX, uint16_t rawY, float& outX, float& outY);
    // Profile matching the currently mounted controller, or nullptr
    static const void* gateFor(uint8_t stick);
    static HMLAnalogAddon* instance;
    ADS8332* getAds() { return ads; }

private:
    float calibrate(uint16_t raw, uint32_t mn, uint32_t ctr, uint32_t mx);
    float applyCurve(float mag, const void* stickOpts);
    // sampleDtMs/newSample/leadScale describe the *input sample* rate, which is
    // the loop rate on the ADS8332 path but the controller's report rate on the
    // gamepad-state path. See the derivative-term comment in HMLStickState.
    void processStick(const void* stickOpts, HMLStickState& st, float rawX, float rawY, float& outX, float& outY,
                      float sampleDtMs, bool newSample, float leadScale);
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
    // Input-sample clock for the gamepad-state path (see processStick)
    uint32_t lastSampleTick = 0;
    uint32_t lastSampleMicros = 0;
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
