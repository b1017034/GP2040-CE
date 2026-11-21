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
// Input-delay ring: one sample stored every HML_DELAY_STORE_US, so the ring
// spans HML_DELAY_SLOTS * HML_DELAY_STORE_US independent of the loop rate
// (64 * 0.5ms = 32ms >= the 30ms UI maximum)
#define HML_DELAY_SLOTS 64
#define HML_DELAY_STORE_US 500

// Cached ln(1 - alpha@1kHz) for one RC-style low-pass, keyed by its strength,
// so the per-frame rate conversion is a single expf()
struct HMLRcCache {
    float strength = 0.0f;
    float logKeep = 0.0f;
    bool valid = false;
};

// Recent raw history for 1ms-window derivatives (lead term, RC velocity).
// FireBird defines them per 1kHz sample; taking the per-loop delta instead
// amplifies per-sample noise at faster loop rates.
#define HML_RAW_HIST 16
#define HML_DERIV_WINDOW_US 1000

// Runtime state for one stick axis pair
struct HMLStickState {
    // filtered values (normalized -1..1)
    float filtX = 0.0f;
    float filtY = 0.0f;
    float prevRawX = 0.0f;
    float prevRawY = 0.0f;
    // raw history ring (newest at rawHead-1)
    float rawHistX[HML_RAW_HIST] = {0};
    float rawHistY[HML_RAW_HIST] = {0};
    uint32_t rawHistT[HML_RAW_HIST] = {0};
    uint8_t rawHead = 0;
    uint8_t rawCount = 0;
    // snapback filter state (return-phase low-pass)
    float snapX = 0.0f;
    float snapY = 0.0f;
    HMLRcCache rcCache;
    HMLRcCache snapCache;
    // noise gate: last held raw counts (Firebird-style stabilization)
    uint16_t heldX = 0;
    uint16_t heldY = 0;
    bool heldInit = false;
    // FireBird 追加入力遅延: output ring buffer (time-decimated)
    float dlyX[HML_DELAY_SLOTS] = {0};
    float dlyY[HML_DELAY_SLOTS] = {0};
    uint32_t dlyT[HML_DELAY_SLOTS] = {0};
    uint8_t dlyHead = 0;
    uint8_t dlyCount = 0;
    uint32_t dlyLastStoreUs = 0;
};

// Pipeline timing (us), exposed over the config channel RAW page.
// avg = exponential moving average, max = worst value of the last second.
struct HMLTimingStats {
    uint16_t loopAvgUs = 0;   // process() to process()
    uint16_t loopMaxUs = 0;
    uint16_t acquireUs = 0;   // ADC acquisition (SPI frames + RP2040 ADC)
    uint16_t ageUs = 0;       // acquisition end -> stick processing
    // USB report scheduler, last 1s window (see USBReportSchedulerDiag)
    uint16_t usbFrames = 0;       // SOFs per second
    uint16_t usbReads = 0;        // reports read by the host per second
    uint16_t usbPhaseAvgUs = 0;   // host read time within the frame
    uint16_t usbPhaseMinUs = 0;
    uint16_t usbPhaseMaxUs = 0;
    uint16_t usbArmAgeAvgUs = 0;  // arm -> host read
    uint16_t usbArmAgeMaxUs = 0;
    uint16_t usbDataAgeAvgUs = 0; // stick ADC acquisition -> host read
    uint16_t usbDataAgeMaxUs = 0;
    uint16_t usbReplaced = 0;     // in-buffer replacements per second
    uint16_t usbDeferred = 0;     // deferred analog-only reports per second
};

class HMLAnalogAddon : public GPAddon {
public:
    virtual bool available();
    virtual void setup();
    // Loop placement (latency): preprocess() maps the ADC-backed buttons from
    // the latest samples, process() runs the stick pipeline, and the ADC
    // acquisition itself runs in postprocess() - i.e. right after the USB
    // report went out - so the ~200us of SPI frames never sit between the
    // GPIO button read and the report, and the samples used next frame are
    // only one loop-tail old.
    virtual void preprocess();
    virtual void process();
    virtual void postprocess(bool sent);
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
    static HMLTimingStats timing;
    ADS8332* getAds() { return ads; }

private:
    void acquire();
    void updateTiming(uint32_t nowUs);
    void processStick(const void* stickOpts, HMLStickState& st, float rawX, float rawY, float& outX, float& outY);
    void emitStick(const void* stickOpts, HMLStickState& st, float x, float y, float& outX, float& outY);

    ADS8332 *ads = nullptr;
    HMLStickState leftState;
    HMLStickState rightState;
    bool adcInitialized = false;
    // Per-frame clock: sampled once at the top of process()
    uint32_t frameNowUs = 0;
    float frameDtMs = 1.0f;
    uint32_t lastProcessUs = 0;
    uint32_t acquireEndUs = 0;
    uint32_t timingWindowUs = 0;
    uint16_t loopMaxWindowUs = 0;
    uint32_t loopAvgQ4 = 0;   // EMA in 1/16 us
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
