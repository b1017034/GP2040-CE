#include "addons/hml_analog.h"
#include "drivers/shared/USBReportScheduler.h"
#include "storagemanager.h"
#include "peripheralmanager.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include <cmath>
#include <algorithm>
#include <cstring>

uint16_t HMLAnalogAddon::rawChannels[ADS8332_CHANNEL_COUNT] = {0};
uint16_t HMLAnalogAddon::rawAdc[2] = {0};
bool HMLAnalogAddon::rawValid = false;
HMLAnalogAddon* HMLAnalogAddon::instance = nullptr;
HMLTimingStats HMLAnalogAddon::timing;
float HMLAnalogAddon::preCurveX[2] = {0.0f, 0.0f};
float HMLAnalogAddon::preCurveY[2] = {0.0f, 0.0f};

// Sensitivity curve presets (fraction in/out), Firebird-compatible
static const float CURVE_PRESETS[5][3][2] = {
    {{0.30f,0.30f},{0.70f,0.70f},{1.0f,1.0f}},   // 0 linear
    {{0.60f,0.30f},{0.90f,0.80f},{1.0f,1.0f}},   // 1 slow start
    {{0.30f,0.60f},{0.70f,0.90f},{1.0f,1.0f}},   // 2 quick start
    {{0.20f,0.80f},{0.60f,0.95f},{1.0f,1.0f}},   // 3 instant
    {{0.30f,0.25f},{0.70f,0.75f},{1.0f,1.0f}},   // 4 smooth start
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

    adc_init(); // RP2040 ADC peripheral must be initialized before any adc_read()
    if (opts.ltAdcPin >= 26 && opts.ltAdcPin <= 29) adc_gpio_init(opts.ltAdcPin);
    if (opts.rtAdcPin >= 26 && opts.rtAdcPin <= 29) adc_gpio_init(opts.rtAdcPin);
    adcInitialized = true;

    // Stick click buttons: plain active-low inputs
    if (opts.l3Pin >= 0 && opts.l3Pin < 30) {
        gpio_init(opts.l3Pin);
        gpio_set_dir(opts.l3Pin, GPIO_IN);
        gpio_pull_up(opts.l3Pin);
    }
    if (opts.r3Pin >= 0 && opts.r3Pin < 30) {
        gpio_init(opts.r3Pin);
        gpio_set_dir(opts.r3Pin, GPIO_IN);
        gpio_pull_up(opts.r3Pin);
    }

    Gamepad *gamepad = Storage::getInstance().GetGamepad();
    if (opts.ltAdcPin >= 0 || opts.rtAdcPin >= 0)
        gamepad->hasAnalogTriggers = true;

    lightbarSetup();

    lastProcessUs = time_us_32();
    acquireEndUs = lastProcessUs;
    timingWindowUs = lastProcessUs;
}

// One WS2812 lightbar showing a fixed user-configured color. This avoids the
// generic NeoPico/player-LED path, whose animation-brightness and per-player
// level gates zero the pixel.
// PIO placement: Pico-PIO-USB (PS5 dongle host) runs single-PIO on pio0
// (sm0 TX, sm1 RX, sm2 EOP; see peripheral_usb.cpp). Claiming any of those
// kills the host - the dongle then never authenticates and the PS5 drops the
// controller. The generic NeoPico addon uses pio1/sm0, so the lightbar takes
// pio1/sm1.
void HMLAnalogAddon::lightbarSetup() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!opts.lightbarEnabled || opts.lightbarPin < 0 || opts.lightbarPin >= 30)
        return;
    lightbar = new NeoPico();
    lightbar->Setup(opts.lightbarPin, 1, LED_FORMAT_GRB, pio1, 1);
    lightbar->Off(); // start dark
}

// Called from the web-config loop (where Core0 add-ons are otherwise skipped) so
// the lightbar keeps showing the configured color and previews edits live.
void HMLAnalogAddon::lightbarTick() {
    if (instance != nullptr) instance->lightbarUpdate();
}

void HMLAnalogAddon::lightbarUpdate() {
    if (lightbar == nullptr) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - lightbarNextMs) < 0) return;
    lightbarNextMs = now + 20; // ~50 Hz, also serves as the ws2812 latch gap

    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;

    // Always show the user's configured color (0xRRGGBB); the lightbar does not
    // track the console-sent color.
    uint8_t r = (opts.lightbarColor >> 16) & 0xFF;
    uint8_t g = (opts.lightbarColor >> 8) & 0xFF;
    uint8_t b = opts.lightbarColor & 0xFF;
    uint32_t br = opts.lightbarBrightness > 255 ? 255 : opts.lightbarBrightness;
    r = (uint8_t)((uint16_t)r * br / 255);
    g = (uint8_t)((uint16_t)g * br / 255);
    b = (uint8_t)((uint16_t)b * br / 255);
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
    // Rewrite periodically even when unchanged: electrical noise (e.g. warm
    // reboot glitching the data line) can corrupt the strip's latched color
    bool force = (int32_t)(now - lightbarForceMs) >= 0;
    if (grb != lightbarLastColor || force) {
        lightbarLastColor = grb;
        lightbarForceMs = now + 500;
        // SetFrame copies a full 100-pixel array; only pixel 0 is shown (count=1)
        uint32_t buf[100] = {0};
        buf[0] = grb;
        lightbar->SetFrame(buf);
        lightbar->Show();
    }
}

// ---------------------------------------------------------------------------
// Stick pipeline. Everything below runs every loop, so it lives in RAM
// (__not_in_flash_func): with the firmware far larger than the 16KB XIP
// cache, flash-resident hot code takes cache-miss stalls of several us.
// ---------------------------------------------------------------------------

// Map raw ADC to normalized -1..1 using min/center/max calibration
static float __not_in_flash_func(calibrate)(uint16_t raw, uint32_t mn, uint32_t ctr, uint32_t mx) {
    if (mx <= mn || ctr <= mn || mx <= ctr) {
        // Uncalibrated: assume full range, center at midpoint
        return ((float)raw - 32768.0f) * (1.0f / 32768.0f);
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
// Presets 0..4 are built in; 5..8 select custom slots 0..3, whose points are
// stored in tenths of a percent (FireBird A* 0.0..100.0 resolution).
static float __not_in_flash_func(applyCurve)(float mag, const HMLStickOptions& s) {
    const bool custom = s.curvePreset >= 5;
    uint32_t count;
    uint32_t base = 0;
    if (custom) {
        uint32_t slot = std::min<uint32_t>(s.curvePreset - 5, 3);
        base = slot * HML_CURVE_MAX_POINTS;
        count = (slot < s.curvesCount_count) ? s.curvesCount[slot] : 0;
        if (count > HML_CURVE_MAX_POINTS) count = HML_CURVE_MAX_POINTS;
        if (base + count > s.curvesX_count) count = s.curvesX_count > base ? s.curvesX_count - base : 0;
        if (base + count > s.curvesY_count) count = s.curvesY_count > base ? s.curvesY_count - base : 0;
        if (count == 0) return mag; // no custom points -> linear
    } else {
        count = 3;
    }
    const float (*preset)[2] = custom ? nullptr : CURVE_PRESETS[s.curvePreset];

    float px = 0.0f, py = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        float nx = custom ? (float)s.curvesX[base + i] * 0.001f : preset[i][0];
        float ny = custom ? (float)s.curvesY[base + i] * 0.001f : preset[i][1];
        if (mag <= nx) {
            float denom = nx - px;
            if (denom <= 0.0001f) return ny;
            return py + (mag - px) * (ny - py) / denom;
        }
        px = nx; py = ny;
    }
    return py > 0.0f ? py : mag;
}

// RC filter strength lookup for advanced (velocity curve) mode.
// FireBird convention: point 0 is 静止 (velocity 0), last point is 最大速度
// (velocity 128); the UI keeps the X values consistent with that.
static float __not_in_flash_func(rcStrengthForVelocity)(const HMLStickOptions& s, float vel128) {
    uint32_t count = (s.rcCurveX_count < HML_RC_CURVE_MAX_POINTS) ? s.rcCurveX_count : HML_RC_CURVE_MAX_POINTS;
    if (s.rcCurveY_count < count) count = s.rcCurveY_count;
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

static inline float scalePct(uint32_t v) {
    return v == 0 ? 1.0f : (float)v * 0.01f;
}

// FireBird per-axis deadzone: inner% .. outer% of travel maps to 0..1
static float __not_in_flash_func(axisDeadzone)(float v, uint32_t innerPct, uint32_t outerPct) {
    // Default settings (no inner, full outer) reduce to a clamp
    if (innerPct == 0 && (outerPct == 0 || outerPct == 100)) return std::clamp(v, -1.0f, 1.0f);
    float inner = (float)innerPct * 0.01f;
    float outer = (outerPct == 0 ? 1.0f : (float)outerPct * 0.01f);
    if (outer <= inner + 0.001f) outer = inner + 0.001f;
    float av = fabsf(v);
    if (av <= inner) return 0.0f;
    float nv = (av - inner) / (outer - inner);
    if (nv > 1.0f) nv = 1.0f;
    return v >= 0 ? nv : -nv;
}

// FireBird アンチデッドゾーン: minimum output once outside the inner deadzone
static float __not_in_flash_func(antiDeadzone)(float v, uint32_t antiPct) {
    if (antiPct == 0 || fabsf(v) < 0.0001f) return v;
    float anti = (float)antiPct * 0.01f;
    float av = anti + fabsf(v) * (1.0f - anti);
    if (av > 1.0f) av = 1.0f;
    return v >= 0 ? av : -av;
}

// Rate-independent RC low-pass coefficient.
// The FireBird filters are defined per 1kHz sample: alpha1 = k / (k + R).
// For a loop running at any other rate, the per-sample coefficient that gives
// the same continuous-time response is alpha = 1 - (1 - alpha1)^(dt/1ms).
// (The old q = k/dt form only matched at exactly 1kHz; at the real ~2-4kHz
// loop rate it made every filter several times weaker than intended, and the
// feel changed whenever the loop got faster or slower.)
static float __not_in_flash_func(rcAlpha)(HMLRcCache& c, float k, float strength, float dtMs) {
    if (!c.valid || c.strength != strength) {
        float alpha1 = k / (k + strength);
        c.logKeep = logf(1.0f - alpha1);
        c.strength = strength;
        c.valid = true;
    }
    return 1.0f - expf(c.logKeep * dtMs);
}

// Snapback filter approximation: RC low-pass (same formula as the positive RC
// filter, R = strength*5) applied only while the axis is returning toward /
// crossing center, so press response is untouched
static float __not_in_flash_func(snapbackAxis)(float v, float& filt, bool returningGate, float alpha) {
    bool returning = returningGate && ((v * filt < 0.0f) || (fabsf(v) < fabsf(filt)));
    if (returning) filt += (v - filt) * alpha;
    else filt = v;
    return filt;
}

// FireBird 角度補正 map: enabled entries (in -> out, snap deadzone) remap the
// angle piecewise-linearly around the circle. Angles in degrees, 0=up, cw.
static float __not_in_flash_func(applyAngleMap)(const HMLStickOptions& s, float ang) {
    uint32_t total = s.angleMapCount;
    if (total > s.angleMapIn100_count) total = s.angleMapIn100_count;
    if (total > s.angleMapOut100_count) total = s.angleMapOut100_count;
    if (total > 16) total = 16;
    // gather enabled entries (assumed sorted by input angle in the UI)
    float in[16], out[16], dz[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!(s.angleMapEnable & (1u << i))) continue;
        in[n] = (float)s.angleMapIn100[i] * 0.01f;
        out[n] = (float)s.angleMapOut100[i] * 0.01f;
        dz[n] = (i < s.angleMapDz100_count) ? (float)s.angleMapDz100[i] * 0.01f : 0.0f;
        n++;
    }
    if (n == 0) return ang;
    if (n == 1) return out[0];
    // find circular segment [A, B] containing ang
    uint32_t a = n - 1;
    for (uint32_t i = 0; i < n; i++) {
        if (ang >= in[i]) a = i;
    }
    if (ang < in[0]) a = n - 1;
    uint32_t b = (a + 1) % n;
    float inA = in[a], inB = in[b], outA = out[a], outB = out[b];
    float span = inB - inA;
    if (span <= 0) span += 360.0f;
    float rel = ang - inA;
    if (rel < 0) rel += 360.0f;
    // per-entry snap deadzones: inside them the output locks to the entry
    if (rel <= dz[a]) return outA;
    if (span - rel <= dz[b]) return outB;
    float usable = span - dz[a] - dz[b];
    if (usable < 0.001f) return outA;
    float t = (rel - dz[a]) / usable;
    // output advances in the same circular direction as the input
    float dOut = outB - outA;
    if (dOut < 0) dOut += 360.0f;
    float res = outA + t * dOut;
    if (res < 0) res += 360.0f;
    if (res >= 360.0f) res -= 360.0f;
    return res;
}

// Firebird RC filter (formulas from stock FireBird v3.08, defined per 1kHz
// sample; see rcAlpha() for the rate conversion):
// R > 0:  filt += (raw - filt) * alpha,  alpha@1kHz = 100/(100+R)
//         (R=+500 is only ~5ms of lag -- jitter dies in the console's 8-bit
//         output quantization, response stays crisp)
// R < 0:  B = -R; base low-pass with alpha@1kHz = 200/(200+B), then a
//         velocity lead term K = (B+25)/25 x (raw delta per 1kHz sample)
//         fed back into the state. leadDelta is the delta already scaled to
//         this frame and leadGain the rate-corrected K (see processStick), so
//         a moving stick gets the same lead at any loop rate while per-sample
//         ADC noise is not amplified more often.
static float __not_in_flash_func(rcFilterAxis)(float raw, float leadDelta, float& filt, float strength, float alpha, float leadGain) {
    if (strength > 0.5f) {
        filt += (raw - filt) * alpha;
    } else if (strength < -0.5f) {
        float smoothed = filt + (raw - filt) * alpha;
        filt = smoothed + leadDelta * leadGain;
        if (filt > 1.25f) filt = 1.25f;
        if (filt < -1.25f) filt = -1.25f;
    } else {
        filt = raw;
    }
    return filt;
}

void __not_in_flash_func(HMLAnalogAddon::processStick)(const void* stickOptsV, HMLStickState& st, float rawX, float rawY, float& outX, float& outY) {
    const HMLStickOptions& s = *(const HMLStickOptions*)stickOptsV;
    const float dtMs = frameDtMs;

    if (s.invertX) rawX = -rawX;
    if (s.invertY) rawY = -rawY;

    // 1ms-window derivative reference: the newest stored raw sample that is
    // at least HML_DERIV_WINDOW_US old (or the oldest one while the history
    // is still shorter than that). At a 1kHz loop this is exactly the
    // previous sample, i.e. the FireBird per-sample definition.
    float refX = rawX, refY = rawY, winMs = 0.0f;
    if (st.rawCount > 0) {
        const uint32_t now = frameNowUs;
        uint8_t pick = (uint8_t)((st.rawHead + HML_RAW_HIST - st.rawCount) % HML_RAW_HIST); // oldest
        for (uint8_t i = 0; i < st.rawCount; i++) {
            uint8_t idx = (uint8_t)((st.rawHead + HML_RAW_HIST - 1 - i) % HML_RAW_HIST); // newest first
            if ((uint32_t)(now - st.rawHistT[idx]) >= HML_DERIV_WINDOW_US) { pick = idx; break; }
        }
        refX = st.rawHistX[pick];
        refY = st.rawHistY[pick];
        winMs = (float)(now - st.rawHistT[pick]) * 0.001f;
    }
    const float winDx = rawX - refX, winDy = rawY - refY;

    // RC filter parameters
    float strength = (float)s.rcFilterStrength;
    if (s.rcAdvanced) {
        float velMult = (s.rcVelocityMult100 > 0 ? (float)s.rcVelocityMult100 : 100.0f) * 0.01f;
        // normalized units per ms over the 1ms window
        float vel = (winMs > 0.05f) ? sqrtf(winDx*winDx + winDy*winDy) / winMs : 0.0f;
        float vel128 = vel * 64.0f * velMult;
        if (vel128 > 128.0f) vel128 = 128.0f;
        strength = rcStrengthForVelocity(s, vel128);
    }

    float fx, fy;
    if (strength > 0.5f) {
        float a = rcAlpha(st.rcCache, 100.0f, strength, dtMs);
        fx = rcFilterAxis(rawX, 0.0f, st.filtX, strength, a, 0.0f);
        fy = rcFilterAxis(rawY, 0.0f, st.filtY, strength, a, 0.0f);
    } else if (strength < -0.5f) {
        const float b = -strength;
        const float a = rcAlpha(st.rcCache, 200.0f, b, dtMs);
        // per-1kHz-sample delta, spread over this frame's share of the window
        const float k = (winMs > 0.05f) ? dtMs / winMs : 0.0f;
        // Rate-corrected lead gain: the 1kHz filter settles a ramp of slope v
        // at an offset v*(K-1+a1)/a1; spreading the lead over n frames of
        // alpha a needs K' = (K-1+a1) * a/(a1*dt) + 1 - a for the same
        // offset. dt = 1ms gives K' = K exactly.
        const float K = (b + 25.0f) * (1.0f / 25.0f);
        const float a1 = 200.0f / (200.0f + b);
        const float leadGain = (K - 1.0f + a1) * a / (a1 * dtMs) + 1.0f - a;
        fx = rcFilterAxis(rawX, winDx * k, st.filtX, strength, a, leadGain);
        fy = rcFilterAxis(rawY, winDy * k, st.filtY, strength, a, leadGain);
    } else {
        fx = st.filtX = rawX;
        fy = st.filtY = rawY;
    }
    st.prevRawX = rawX;
    st.prevRawY = rawY;
    st.rawHistX[st.rawHead] = rawX;
    st.rawHistY[st.rawHead] = rawY;
    st.rawHistT[st.rawHead] = frameNowUs;
    st.rawHead = (uint8_t)((st.rawHead + 1) % HML_RAW_HIST);
    if (st.rawCount < HML_RAW_HIST) st.rawCount++;

    // Snapback filter: damp only the return-to-center phase (approximation of
    // FireBird's snapback filter; strength 0 = off)
    if (s.rcSnapback > 0) {
        float a = rcAlpha(st.snapCache, 100.0f, (float)s.rcSnapback * 5.0f, dtMs);
        fx = snapbackAxis(fx, st.snapX, true, a);
        fy = snapbackAxis(fy, st.snapY, true, a);
    } else {
        st.snapX = fx;
        st.snapY = fy;
    }

    // FireBird 入力スケール: per-direction gain (100 = calibrated range)
    fx *= (fx >= 0 ? scalePct(s.scaleRight) : scalePct(s.scaleLeft));
    fy *= (fy >= 0 ? scalePct(s.scaleUp) : scalePct(s.scaleDown));

    // FireBird per-axis deadzones: inner (0 = none) and outer (100 = full
    // travel needed for max output)
    fx = axisDeadzone(fx, s.innerX, s.outerX);
    fy = axisDeadzone(fy, s.innerY, s.outerY);

    st.dbgX = fx;
    st.dbgY = fy;

    float mag2 = fx*fx + fy*fy;
    if (mag2 < 1e-8f) {
        emitStick(stickOptsV, st, 0.0f, 0.0f, outX, outY);
        return;
    }
    float mag = sqrtf(mag2);
    float outMag = applyCurve(std::min(mag, 1.0f), s);

    // FireBird 角度補正: angle map -> global offset -> angle snap.
    // Angle convention matches the FireBird UI: 0=up, clockwise, degrees.
    float invMag = 1.0f / mag;
    float ca = fx * invMag, sa = fy * invMag;
    if (s.angleOffset100 != 0 || s.angleSnap != 0 ||
        (s.angleMapCount > 0 && s.angleMapEnable != 0)) {
        float ang = atan2f(ca, sa) * 57.29578f; // 0=up, cw
        if (ang < 0) ang += 360.0f;
        ang = applyAngleMap(s, ang);
        ang += (float)s.angleOffset100 * 0.01f;
        if (s.angleSnap == 1) ang = roundf(ang * (1.0f / 45.0f)) * 45.0f;
        else if (s.angleSnap == 2) ang = roundf(ang * (1.0f / 90.0f)) * 90.0f;
        float rad = ang * 0.0174533f;
        ca = sinf(rad);
        sa = cosf(rad);
    }

    // Firebird-style diagonal scaling: scale the magnitude by the boundary
    // radius at this angle, where the boundary is min(unit square, ellipse
    // with semi-axes 1 + 0.4142*d/100). 0 = perfect circle (no-op), 100 =
    // full square (diagonals reach the corners). Angle is preserved.
    if (s.diagScaleX > 0 || s.diagScaleY > 0) {
        float absCa = std::max(fabsf(ca), 1e-6f);
        float absSa = std::max(fabsf(sa), 1e-6f);
        float maxSquare = 1.0f / std::max(absCa, absSa);
        float ex = 1.0f + 0.004142f * (float)s.diagScaleX;
        float ey = 1.0f + 0.004142f * (float)s.diagScaleY;
        float denom2 = (ca * ca) / (ex * ex) + (sa * sa) / (ey * ey);
        float maxEllipse = 1.0f / sqrtf(std::max(denom2, 1e-9f));
        outMag *= std::min(maxSquare, maxEllipse);
    }

    float ox = std::clamp(ca * outMag, -1.0f, 1.0f);
    float oy = std::clamp(sa * outMag, -1.0f, 1.0f);

    // FireBird アンチデッドゾーン: jump to a minimum output once deflected
    ox = antiDeadzone(ox, s.antiX);
    oy = antiDeadzone(oy, s.antiY);

    emitStick(stickOptsV, st, ox, oy, outX, outY);
}

// FireBird 出力補正 (resolution quantize + extra delay) and final output
void __not_in_flash_func(HMLAnalogAddon::emitStick)(const void* stickOptsV, HMLStickState& st, float x, float y, float& outX, float& outY) {
    const HMLStickOptions& s = *(const HMLStickOptions*)stickOptsV;
    // スティック分解能: quantize to N steps across the full -1..1 range
    if (s.resolution >= 2) {
        float steps = (float)(s.resolution - 1);
        float invSteps = 1.0f / steps;
        x = roundf((x + 1.0f) * 0.5f * steps) * invSteps * 2.0f - 1.0f;
        y = roundf((y + 1.0f) * 0.5f * steps) * invSteps * 2.0f - 1.0f;
    }
    // 追加入力遅延: time-decimated ring, replay the sample from delayMs ago
    if (s.delayMs > 0) {
        const uint32_t now = frameNowUs;
        if (st.dlyCount == 0 || (uint32_t)(now - st.dlyLastStoreUs) >= HML_DELAY_STORE_US) {
            st.dlyX[st.dlyHead] = x;
            st.dlyY[st.dlyHead] = y;
            st.dlyT[st.dlyHead] = now;
            st.dlyHead = (uint8_t)((st.dlyHead + 1) % HML_DELAY_SLOTS);
            if (st.dlyCount < HML_DELAY_SLOTS) st.dlyCount++;
            st.dlyLastStoreUs = now;
        }
        uint32_t target = now - s.delayMs * 1000;
        // oldest -> newest, keep the newest sample that is old enough
        float dx = x, dy = y;
        bool found = false;
        for (uint8_t i = 0; i < st.dlyCount; i++) {
            uint8_t idx = (uint8_t)((st.dlyHead + HML_DELAY_SLOTS - st.dlyCount + i) % HML_DELAY_SLOTS);
            if ((int32_t)(st.dlyT[idx] - target) > 0) break;
            dx = st.dlyX[idx];
            dy = st.dlyY[idx];
            found = true;
        }
        if (found) { x = dx; y = dy; }
        else if (st.dlyCount > 0) {
            // Not old enough yet (just enabled / just booted): hold the oldest
            // sample instead of snapping the stick to center
            uint8_t oldest = (uint8_t)((st.dlyHead + HML_DELAY_SLOTS - st.dlyCount) % HML_DELAY_SLOTS);
            x = st.dlyX[oldest];
            y = st.dlyY[oldest];
        }
    } else {
        st.dlyCount = 0;
    }
    outX = x;
    outY = y;
}

bool HMLAnalogAddon::webRead() {
    if (instance == nullptr || instance->ads == nullptr) return false;
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    static bool webAdcInit = false;
    if (!webAdcInit) {
        adc_init();
        // GPIO26=ADC0, 27=ADC1, 28=ADC2, 29=ADC3. RT is on GPIO28 (ADC2), so we
        // must init/select the configured pins, not a fixed 0/1 pair.
        if (opts.ltAdcPin >= 26 && opts.ltAdcPin <= 29) adc_gpio_init(opts.ltAdcPin);
        if (opts.rtAdcPin >= 26 && opts.rtAdcPin <= 29) adc_gpio_init(opts.rtAdcPin);
        webAdcInit = true;
    }
    instance->ads->readAll(rawChannels);
    if (opts.ltAdcPin >= 26 && opts.ltAdcPin <= 29) {
        adc_select_input(opts.ltAdcPin - 26);
        rawAdc[0] = adc_read();
    }
    if (opts.rtAdcPin >= 26 && opts.rtAdcPin <= 29) {
        adc_select_input(opts.rtAdcPin - 26);
        rawAdc[1] = adc_read();
    }
    rawValid = true;
    return true;
}

bool HMLAnalogAddon::noiseSample(uint16_t* mn, uint16_t* mx, int iterations) {
    if (instance == nullptr || instance->ads == nullptr) return false;
    uint16_t ch[ADS8332_CHANNEL_COUNT];
    for (int i = 0; i < ADS8332_CHANNEL_COUNT; i++) { mn[i] = 0xFFFF; mx[i] = 0; }
    // Reference per channel: wait for a full median window, then take the
    // median of a few published values. Readings further than
    // ADS8332_GLITCH_COUNTS from it are corrupted frames (seen in web-config,
    // where the network stack interrupts the SPI bursts), not stick noise -
    // counting them would store a huge gate band and freeze the stick.
    for (int it = 0; it < 64 && !instance->ads->scanCompleted; it++) instance->ads->readAll(ch);
    if (!instance->ads->scanCompleted) return false;
    uint16_t ref[ADS8332_CHANNEL_COUNT];
    {
        uint16_t s[5][ADS8332_CHANNEL_COUNT];
        for (int k = 0; k < 5; k++) { instance->ads->readAll(ch); memcpy(s[k], ch, sizeof(ch)); }
        for (int i = 0; i < ADS8332_CHANNEL_COUNT; i++) {
            uint16_t v[5];
            for (int k = 0; k < 5; k++) v[k] = s[k][i];
            std::sort(v, v + 5);
            ref[i] = v[2];
        }
    }
    for (int it = 0; it < iterations; it++) {
        instance->ads->readAll(ch);
        for (int i = 0; i < ADS8332_CHANNEL_COUNT; i++) {
            int32_t d = (int32_t)ch[i] - (int32_t)ref[i];
            if (d > ADS8332_GLITCH_COUNTS || d < -ADS8332_GLITCH_COUNTS) continue;
            if (ch[i] < mn[i]) mn[i] = ch[i];
            if (ch[i] > mx[i]) mx[i] = ch[i];
        }
    }
    for (int i = 0; i < ADS8332_CHANNEL_COUNT; i++)
        if (mn[i] > mx[i]) { mn[i] = mx[i] = ref[i]; } // every reading rejected
    return true;
}

// Runs right after the USB report was queued (see header): fetch the next
// frame's samples.
void __not_in_flash_func(HMLAnalogAddon::postprocess)(bool sent) {
    (void)sent;
    acquire();
    lightbarUpdate();
}

void __not_in_flash_func(HMLAnalogAddon::acquire)() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (ads == nullptr || !ads->linkOk) return;

    uint32_t t0 = time_us_32();
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
    // Only report valid once every channel has a full median window:
    // otherwise the zeroed buffer reads as full deflection after boot
    rawValid = ads->scanCompleted;
    acquireEndUs = time_us_32();
    USBReportScheduler::getInstance().setDataTimestamp(acquireEndUs);
    uint32_t took = acquireEndUs - t0;
    timing.acquireUs = (uint16_t)(took > 0xFFFF ? 0xFFFF : took);
}

void __not_in_flash_func(HMLAnalogAddon::preprocess)() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (ads == nullptr) return;

    // Self-heal: after a warm reboot the ADC may still be desynced when the
    // first probe runs; keep retrying the link until it comes up.
    if (!ads->linkOk) {
        rawValid = false;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - adsRetryMs) >= 0) {
            adsRetryMs = now + 500;
            ads->init();
        }
        if (!ads->linkOk) return; // sticks stay centered until the link is up
        acquire(); // first samples right away instead of a loop later
    }
    if (!rawValid) return;

    Gamepad *gamepad = Storage::getInstance().GetGamepad();

    // Analog PS/Home button (SMD button behind headphone jack, rightMt line)
    bool psPressed = false;
    if (opts.chPsButton >= 0 && opts.chPsButton < ADS8332_CHANNEL_COUNT) {
        uint16_t v = rawChannels[opts.chPsButton];
        psPressed = opts.psActiveHigh ? (v >= opts.psThreshold) : (v <= opts.psThreshold);
        if (psPressed) {
            gamepad->state.buttons |= GAMEPAD_MASK_A1;
        }
    }

    // Stick clicks: GPIO variant (active low)
    if (opts.l3Pin >= 0 && opts.l3Pin < 30 && !gpio_get(opts.l3Pin))
        gamepad->state.buttons |= GAMEPAD_MASK_L3;
    if (opts.r3Pin >= 0 && opts.r3Pin < 30 && !gpio_get(opts.r3Pin))
        gamepad->state.buttons |= GAMEPAD_MASK_R3;

    // Stick clicks: analog variant (ADS8332 channels, like the PS button)
    if (opts.chL3 >= 0 && opts.chL3 < ADS8332_CHANNEL_COUNT) {
        uint16_t v = rawChannels[opts.chL3];
        bool pressed = opts.l3ActiveHigh ? (v >= opts.l3Threshold) : (v <= opts.l3Threshold);
        if (pressed) gamepad->state.buttons |= GAMEPAD_MASK_L3;
    }
    if (opts.chR3 >= 0 && opts.chR3 < ADS8332_CHANNEL_COUNT) {
        uint16_t v = rawChannels[opts.chR3];
        bool pressed = opts.r3ActiveHigh ? (v >= opts.r3Threshold) : (v <= opts.r3Threshold);
        // Resistor ladder: PS shares this channel at a lower level - PS wins
        if (pressed && opts.chR3 == opts.chPsButton && psPressed) pressed = false;
        if (pressed) gamepad->state.buttons |= GAMEPAD_MASK_R3;
    }
}

// Firebird-style stabilization: backlash gate on raw counts. The held value
// only moves when the input leaves a band the size of the measured at-rest
// noise, so a stationary stick produces a bit-identical output every frame.
// FireBird 動作の安定化: 静止中 (ノイズ幅以内の変動) は直前の値をホールドし、
// ノイズ幅を超えて動いたら生値へ完全追従する。追従型 (raw±band を返す) にする
// と中央・最大値が常に band ぶんズレるので不可。
// Stick oversampling: moving average over the last 2^n raw readings per axis
// (each reading is already a 3-sample median from the ADC layer). Adds
// (2^n - 1)/2 readings of lag (~0.17ms each at the current loop rate).
static void __not_in_flash_func(oversample)(HMLStickState& st, uint32_t setting, uint16_t& x, uint16_t& y) {
    uint8_t shift = (uint8_t)(setting > 4 ? 4 : setting);
    if (shift == 0) { st.osShift = 0; st.osFill = 0; return; }
    if (shift != st.osShift) { // window changed: restart
        st.osShift = shift; st.osFill = 0; st.osHead = 0; st.osSumX = 0; st.osSumY = 0;
    }
    const uint8_t n = (uint8_t)(1u << shift);
    if (st.osFill == n) {
        st.osSumX -= st.osX[st.osHead];
        st.osSumY -= st.osY[st.osHead];
    } else {
        st.osFill++;
    }
    st.osX[st.osHead] = x;
    st.osY[st.osHead] = y;
    st.osSumX += x;
    st.osSumY += y;
    st.osHead = (uint8_t)((st.osHead + 1) & (n - 1));
    x = (uint16_t)((st.osSumX + st.osFill / 2) / st.osFill);
    y = (uint16_t)((st.osSumY + st.osFill / 2) / st.osFill);
}

static uint16_t __not_in_flash_func(gateAxis)(uint16_t raw, uint16_t& held, uint32_t band) {
    if (band == 0) { held = raw; return raw; }
    // 静止時ジッターの抑制が目的なのでバンドはフルスケール5%まで: 計測異常で
    // 巨大な値が保存されても軸が凍らないようにする
    if (band > 3276) band = 3276;
    int32_t d = (int32_t)raw - (int32_t)held;
    if (d > (int32_t)band || d < -(int32_t)band) held = raw;
    return held;
}

void __not_in_flash_func(HMLAnalogAddon::updateTiming)(uint32_t nowUs) {
    uint32_t loop = nowUs - lastProcessUs;
    if (loop > 0xFFFF) loop = 0xFFFF;
    // EMA with 1/16 weight, kept in 1/16 us
    loopAvgQ4 = loopAvgQ4 - (loopAvgQ4 >> 4) + loop;
    timing.loopAvgUs = (uint16_t)(loopAvgQ4 >> 4);
    if (loop > loopMaxWindowUs) loopMaxWindowUs = (uint16_t)loop;
    if ((uint32_t)(nowUs - timingWindowUs) >= 1000000u) {
        timing.loopMaxUs = loopMaxWindowUs;
        loopMaxWindowUs = 0;
        timingWindowUs = nowUs;
        USBReportSchedulerDiag d = USBReportScheduler::getInstance().takeDiag();
        auto clamp16 = [](uint32_t v) -> uint16_t { return (uint16_t)(v > 0xFFFF ? 0xFFFF : v); };
        timing.usbFrames = clamp16(d.frames);
        timing.usbReads = clamp16(d.reads);
        timing.usbPhaseAvgUs = clamp16(d.reads ? d.phaseSumUs / d.reads : 0);
        timing.usbPhaseMinUs = clamp16(d.reads ? d.phaseMinUs : 0);
        timing.usbPhaseMaxUs = clamp16(d.phaseMaxUs);
        timing.usbArmAgeAvgUs = clamp16(d.reads ? d.armAgeSumUs / d.reads : 0);
        timing.usbArmAgeMaxUs = clamp16(d.armAgeMaxUs);
        timing.usbDataAgeAvgUs = clamp16(d.dataReads ? d.dataAgeSumUs / d.dataReads : 0);
        timing.usbDataAgeMaxUs = clamp16(d.dataAgeMaxUs);
        timing.usbReplaced = clamp16(d.replaced);
        timing.usbDeferred = clamp16(d.deferred);
    }
    uint32_t age = nowUs - acquireEndUs;
    timing.ageUs = (uint16_t)(age > 0xFFFF ? 0xFFFF : age);
}

static inline uint16_t toAxis16(float v) {
    return (uint16_t)std::clamp((v + 1.0f) * 32767.5f, 0.0f, 65535.0f);
}

void __not_in_flash_func(HMLAnalogAddon::process)() {
    // One clock sample per frame: dt for every filter, delay timestamps
    const uint32_t nowUs = time_us_32();
    float dtMs = (float)(nowUs - lastProcessUs) * 0.001f;
    if (dtMs < 0.1f) dtMs = 0.1f;
    if (dtMs > 10.0f) dtMs = 10.0f;
    frameDtMs = dtMs;
    frameNowUs = nowUs;
    updateTiming(nowUs);
    lastProcessUs = nowUs;

    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!rawValid) return;
    Gamepad *gamepad = Storage::getInstance().GetGamepad();

    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    if (opts.chLX >= 0 && opts.chLY >= 0) {
        const HMLStickOptions& s = opts.leftStick;
        uint16_t rx16 = rawChannels[opts.chLX], ry16 = rawChannels[opts.chLY];
        oversample(leftState, s.oversampling, rx16, ry16);
        if (s.noiseSuppress) {
            if (!leftState.heldInit) { leftState.heldX = rx16; leftState.heldY = ry16; leftState.heldInit = true; }
            rx16 = gateAxis(rx16, leftState.heldX, s.noiseX);
            ry16 = gateAxis(ry16, leftState.heldY, s.noiseY);
        }
        // Hardware X wiring is reversed vs the console convention, so negate at
        // the source: with "X軸反転" OFF, right = +X (invert flag then optional)
        float rawX = -calibrate(rx16, s.calMinX, s.calCenterX, s.calMaxX);
        float rawY = calibrate(ry16, s.calMinY, s.calCenterY, s.calMaxY);
        processStick(&s, leftState, rawX, rawY, lx, ly);
        preCurveX[0] = leftState.dbgX; preCurveY[0] = leftState.dbgY;
        gamepad->state.lx = toAxis16(lx);
        gamepad->state.ly = toAxis16(ly);
    }
    if (opts.chRX >= 0 && opts.chRY >= 0) {
        const HMLStickOptions& s = opts.rightStick;
        uint16_t rx16 = rawChannels[opts.chRX], ry16 = rawChannels[opts.chRY];
        oversample(rightState, s.oversampling, rx16, ry16);
        if (s.noiseSuppress) {
            if (!rightState.heldInit) { rightState.heldX = rx16; rightState.heldY = ry16; rightState.heldInit = true; }
            rx16 = gateAxis(rx16, rightState.heldX, s.noiseX);
            ry16 = gateAxis(ry16, rightState.heldY, s.noiseY);
        }
        float rawX = -calibrate(rx16, s.calMinX, s.calCenterX, s.calMaxX);
        float rawY = calibrate(ry16, s.calMinY, s.calCenterY, s.calMaxY);
        processStick(&s, rightState, rawX, rawY, rx, ry);
        preCurveX[1] = rightState.dbgX; preCurveY[1] = rightState.dbgY;
        gamepad->state.rx = toAxis16(rx);
        gamepad->state.ry = toAxis16(ry);
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
}
