#include "addons/hml_analog.h"
#include "storagemanager.h"
#include "peripheralmanager.h"
#include "hardware/adc.h"
#include "pico/time.h"
#include <cmath>
#include <algorithm>

uint16_t HMLAnalogAddon::rawChannels[ADS8332_CHANNEL_COUNT] = {0};
uint16_t HMLAnalogAddon::rawAdc[2] = {0};
bool HMLAnalogAddon::rawValid = false;
HMLAnalogAddon* HMLAnalogAddon::instance = nullptr;

// Sensitivity curve presets (percent in/out), Firebird-compatible
static const uint8_t CURVE_PRESETS[5][3][2] = {
    {{30,30},{70,70},{100,100}},   // 0 linear
    {{60,30},{90,80},{100,100}},   // 1 slow start
    {{30,60},{70,90},{100,100}},   // 2 quick start
    {{20,80},{60,95},{100,100}},   // 3 instant
    {{30,25},{70,75},{100,100}},   // 4 smooth start
};

bool HMLAnalogAddon::available() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    return opts.enabled && PeripheralManager::getInstance().isSPIEnabled(0);
}

void HMLAnalogAddon::setup() {
    instance = this;
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    const PeripheralOptions& periph = Storage::getInstance().getPeripheralOptions();

    PeripheralSPI *spi = PeripheralManager::getInstance().getSPI(0);
    ads = new ADS8332(spi, periph.blockSPI0.cs, periph.blockSPI0.rx);
    ads->init();

    if (opts.ltAdcPin >= 26 && opts.ltAdcPin <= 29) adc_gpio_init(opts.ltAdcPin);
    if (opts.rtAdcPin >= 26 && opts.rtAdcPin <= 29) adc_gpio_init(opts.rtAdcPin);
    adcInitialized = true;

    Gamepad *gamepad = Storage::getInstance().GetGamepad();
    if (opts.ltAdcPin >= 0 || opts.rtAdcPin >= 0)
        gamepad->hasAnalogTriggers = true;

    lastMicros = to_us_since_boot(get_absolute_time());
}

// Map raw ADC to normalized -1..1 using min/center/max calibration
float HMLAnalogAddon::calibrate(uint16_t raw, uint32_t mn, uint32_t ctr, uint32_t mx) {
    if (mx <= mn || ctr <= mn || mx <= ctr) {
        // Uncalibrated: assume full range, center at midpoint
        return ((float)raw - 32768.0f) / 32768.0f;
    }
    float v;
    if (raw >= ctr) {
        v = (float)(raw - ctr) / (float)(mx - ctr);
    } else {
        v = -(float)(ctr - raw) / (float)(ctr - mn);
    }
    return std::clamp(v, -1.0f, 1.0f);
}

// Piecewise-linear sensitivity curve on stick magnitude (0..1 in, 0..1 out)
float HMLAnalogAddon::applyCurve(float mag, const void* stickOptsV) {
    const HMLStickOptions& s = *(const HMLStickOptions*)stickOptsV;
    float px = 0.0f, py = 0.0f;
    uint32_t preset = s.curvePreset;
    uint32_t count;
    float xs[HML_CURVE_MAX_POINTS + 1], ys[HML_CURVE_MAX_POINTS + 1];

    if (preset >= 5) { // custom
        count = std::min<uint32_t>(s.curveX_count, (uint32_t)HML_CURVE_MAX_POINTS);
        if (s.curvePointCount > 0 && s.curvePointCount < count) count = s.curvePointCount;
        if (count == 0) return mag; // no custom points -> linear
        for (uint32_t i = 0; i < count; i++) {
            xs[i] = s.curveX[i] / 100.0f;
            ys[i] = s.curveY[i] / 100.0f;
        }
    } else {
        count = 3;
        for (uint32_t i = 0; i < 3; i++) {
            xs[i] = CURVE_PRESETS[preset][i][0] / 100.0f;
            ys[i] = CURVE_PRESETS[preset][i][1] / 100.0f;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        float nx = xs[i], ny = ys[i];
        if (mag <= nx) {
            float denom = nx - px;
            if (denom <= 0.0001f) return ny;
            return py + (mag - px) * (ny - py) / denom;
        }
        px = nx; py = ny;
    }
    return py > 0.0f ? py : mag;
}

// RC filter strength lookup for advanced (velocity curve) mode
static float rcStrengthForVelocity(const HMLStickOptions& s, float vel128) {
    uint32_t count = (s.rcCurveX_count < HML_CURVE_MAX_POINTS) ? s.rcCurveX_count : HML_CURVE_MAX_POINTS;
    if (s.rcCurvePointCount > 0 && s.rcCurvePointCount < count) count = s.rcCurvePointCount;
    if (count == 0) return (float)s.rcFilterStrength;
    float px = 0.0f, py = (float)s.rcCurveY[0];
    for (uint32_t i = 0; i < count; i++) {
        float nx = (float)s.rcCurveX[i];
        float ny = (float)s.rcCurveY[i];
        if (vel128 <= nx) {
            float denom = nx - px;
            if (denom <= 0.001f) return ny;
            return py + (vel128 - px) * (ny - py) / denom;
        }
        px = nx; py = ny;
    }
    return py;
}

// Firebird-style RC filter on one axis.
// strength > 0: first-order low-pass (jitter suppression, slower response)
// strength < 0: inverse boost (sharper response, more jitter)
// snapback (0..100): correction blend ratio; 10 == exactly the filtered value
static float rcFilterAxis(float raw, float prevRaw, float& filt, float strength, uint32_t snapback, float dtMs) {
    float corrected;
    if (strength > 0.5f) {
        float alpha = 1.0f / (1.0f + strength * 0.02f * dtMs);
        filt += alpha * (raw - filt);
        corrected = filt;
    } else if (strength < -0.5f) {
        float boost = -strength * 0.004f;
        corrected = raw + boost * (raw - prevRaw);
        filt = raw;
    } else {
        filt = raw;
        corrected = raw;
    }
    float blend = (float)snapback / 10.0f;
    float out = raw + blend * (corrected - raw);
    if (out > 1.25f) out = 1.25f;
    if (out < -1.25f) out = -1.25f;
    return out;
}

void HMLAnalogAddon::processStick(const void* stickOptsV, HMLStickState& st, float rawX, float rawY, float& outX, float& outY) {
    const HMLStickOptions& s = *(const HMLStickOptions*)stickOptsV;

    if (s.invertX) rawX = -rawX;
    if (s.invertY) rawY = -rawY;

    uint32_t nowUs = to_us_since_boot(get_absolute_time());
    float dtMs = (nowUs - lastMicros) / 1000.0f;
    if (dtMs < 0.1f) dtMs = 0.1f;
    if (dtMs > 10.0f) dtMs = 10.0f;

    // RC filter parameters
    float strength = (float)s.rcFilterStrength;
    uint32_t snapback = 10; // simple mode: fixed correction ratio
    if (s.rcAdvanced) {
        snapback = s.rcSnapback;
        float velMult = (s.rcVelocityMult100 > 0 ? s.rcVelocityMult100 : 100) / 100.0f;
        float dx = rawX - st.prevRawX;
        float dy = rawY - st.prevRawY;
        float vel = sqrtf(dx*dx + dy*dy) / dtMs;   // normalized units per ms
        float vel128 = vel * 64.0f * velMult;
        if (vel128 > 128.0f) vel128 = 128.0f;
        strength = rcStrengthForVelocity(s, vel128);
    }

    float fx = rcFilterAxis(rawX, st.prevRawX, st.filtX, strength, snapback, dtMs);
    float fy = rcFilterAxis(rawY, st.prevRawY, st.filtY, strength, snapback, dtMs);
    st.prevRawX = rawX;
    st.prevRawY = rawY;

    // Radial deadzones + sensitivity curve
    float mag = sqrtf(fx*fx + fy*fy);
    float inner = s.innerDeadzone / 100.0f;
    float outer = 1.0f - (s.outerDeadzone / 100.0f);
    if (mag <= inner || mag < 0.0001f) {
        outX = 0.0f; outY = 0.0f;
        return;
    }
    float span = outer - inner;
    float normMag = 1.0f;
    if (span > 0.0001f) {
        normMag = (mag - inner) / span;
        if (normMag > 1.0f) normMag = 1.0f;
        if (normMag < 0.0f) normMag = 0.0f;
    }
    float outMag = applyCurve(normMag, stickOptsV);
    float scale = outMag / mag;
    outX = std::clamp(fx * scale, -1.0f, 1.0f);
    outY = std::clamp(fy * scale, -1.0f, 1.0f);
}

bool HMLAnalogAddon::webRead() {
    if (instance == nullptr || instance->ads == nullptr) return false;
    static bool webAdcInit = false;
    if (!webAdcInit) {
        adc_gpio_init(26);
        adc_gpio_init(27);
        webAdcInit = true;
    }
    instance->ads->readAll(rawChannels);
    adc_select_input(0);
    rawAdc[0] = adc_read();
    adc_select_input(1);
    rawAdc[1] = adc_read();
    rawValid = true;
    return true;
}

void HMLAnalogAddon::preprocess() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (ads == nullptr) return;

    ads->readAll(rawChannels);
    if (adcInitialized) {
        if (opts.ltAdcPin >= 26 && opts.ltAdcPin <= 29) {
            adc_select_input(opts.ltAdcPin - 26);
            rawAdc[0] = adc_read();
        }
        if (opts.rtAdcPin >= 26 && opts.rtAdcPin <= 29) {
            adc_select_input(opts.rtAdcPin - 26);
            rawAdc[1] = adc_read();
        }
    }
    rawValid = true;

    // Analog PS/Home button (SMD button behind headphone jack, rightMt line)
    if (opts.chPsButton >= 0 && opts.chPsButton < ADS8332_CHANNEL_COUNT) {
        Gamepad *gamepad = Storage::getInstance().GetGamepad();
        uint16_t v = rawChannels[opts.chPsButton];
        bool pressed = opts.psActiveHigh ? (v >= opts.psThreshold) : (v <= opts.psThreshold);
        if (pressed) {
            gamepad->state.buttons |= GAMEPAD_MASK_A1;
        }
    }
}

void HMLAnalogAddon::process() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!rawValid) return;
    Gamepad *gamepad = Storage::getInstance().GetGamepad();

    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    if (opts.chLX >= 0 && opts.chLY >= 0) {
        const HMLStickOptions& s = opts.leftStick;
        float rawX = calibrate(rawChannels[opts.chLX], s.calMinX, s.calCenterX, s.calMaxX);
        float rawY = calibrate(rawChannels[opts.chLY], s.calMinY, s.calCenterY, s.calMaxY);
        processStick(&s, leftState, rawX, rawY, lx, ly);
        gamepad->state.lx = (uint16_t)std::clamp((lx + 1.0f) * 32767.5f, 0.0f, 65535.0f);
        gamepad->state.ly = (uint16_t)std::clamp((ly + 1.0f) * 32767.5f, 0.0f, 65535.0f);
    }
    if (opts.chRX >= 0 && opts.chRY >= 0) {
        const HMLStickOptions& s = opts.rightStick;
        float rawX = calibrate(rawChannels[opts.chRX], s.calMinX, s.calCenterX, s.calMaxX);
        float rawY = calibrate(rawChannels[opts.chRY], s.calMinY, s.calCenterY, s.calMaxY);
        processStick(&s, rightState, rawX, rawY, rx, ry);
        gamepad->state.rx = (uint16_t)std::clamp((rx + 1.0f) * 32767.5f, 0.0f, 65535.0f);
        gamepad->state.ry = (uint16_t)std::clamp((ry + 1.0f) * 32767.5f, 0.0f, 65535.0f);
    }

    // Analog triggers: hall outputs idle high and pull toward 0 when pressed.
    // Map [idle..pressed] to [0..255] per-trigger.
    auto triggerTravel = [](uint16_t raw, int idle, int pressed) -> uint8_t {
        int span = idle - pressed;
        if (span <= 0) return 0;
        int travel = idle - (int)raw;
        if (travel < 0) travel = 0;
        int v = travel * 255 / span;
        return (uint8_t)(v > 255 ? 255 : v);
    };
    if (opts.ltAdcPin >= 26) {
        uint8_t lt = triggerTravel(rawAdc[0], HML_TRIGGER_LT_IDLE, HML_TRIGGER_LT_PRESSED);
        gamepad->state.lt = lt;
        if (lt >= (255 * HML_TRIGGER_DIGITAL_PERCENT / 100))
            gamepad->state.buttons |= GAMEPAD_MASK_L2;
    }
    if (opts.rtAdcPin >= 26) {
        uint8_t rt = triggerTravel(rawAdc[1], HML_TRIGGER_RT_IDLE, HML_TRIGGER_RT_PRESSED);
        gamepad->state.rt = rt;
        if (rt >= (255 * HML_TRIGGER_DIGITAL_PERCENT / 100))
            gamepad->state.buttons |= GAMEPAD_MASK_R2;
    }

    lastMicros = to_us_since_boot(get_absolute_time());
}
