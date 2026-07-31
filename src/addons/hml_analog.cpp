#include "addons/hml_analog.h"
#include "storagemanager.h"
#include "peripheralmanager.h"
#include "eventmanager.h"
#include "addons/gamepad_usb_host_listener.h"
#include <string.h>
#include "hardware/adc.h"
#include "hardware/pio.h"
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

// Two stick sources share this add-on, picked by what the board actually has:
// SPI block 0 present -> ADS8332 hall sticks (HML board); absent -> the sticks
// already in gamepad state, put there by a USB host add-on (a DualSense on the
// MeisterConverter). Auto-detected so no extra config knob exists to get wrong.
bool HMLAnalogAddon::available() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    return opts.enabled;
}

void HMLAnalogAddon::setup() {
    instance = this;
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    const PeripheralOptions& periph = Storage::getInstance().getPeripheralOptions();

    adcSource = PeripheralManager::getInstance().isSPIEnabled(0);
    if (adcSource) {
        PeripheralSPI *spi = PeripheralManager::getInstance().getSPI(0);
        ads = new ADS8332(spi, periph.blockSPI0.cs, periph.blockSPI0.rx);
        ads->init();
    }

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

    lastMicros = to_us_since_boot(get_absolute_time());
}

// One WS2812 lightbar showing a fixed user-configured color. This avoids the
// generic NeoPico/player-LED path, whose animation-brightness and per-player
// level gates zero the pixel.
// PIO placement: Pico-PIO-USB (PS5 dongle host) uses pio0/sm0 for TX and
// pio1/sm0 (RX) + pio1/sm1 (EOP detect). Claiming pio1/sm1 kills the host's
// EOP state machine - the dongle then never authenticates and the PS5 drops
// the controller - so the lightbar lives on pio0/sm1 instead.
void HMLAnalogAddon::lightbarSetup() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!opts.lightbarEnabled || opts.lightbarPin < 0 || opts.lightbarPin >= 30)
        return;
    lightbar = new NeoPico();
    lightbar->Setup(opts.lightbarPin, 1, LED_FORMAT_GRB, pio0, 1);
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

// RC filter strength lookup for advanced (velocity curve) mode.
// FireBird convention: point 0 is 静止 (velocity 0), last point is 最大速度
// (velocity 128); the UI keeps the X values consistent with that.
static float rcStrengthForVelocity(const HMLStickOptions& s, float vel128) {
    uint32_t count = (s.rcCurveX_count < HML_RC_CURVE_MAX_POINTS) ? s.rcCurveX_count : HML_RC_CURVE_MAX_POINTS;
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
    return (v == 0 ? 100.0f : (float)v) / 100.0f;
}

// FireBird per-axis deadzone: inner% .. outer% of travel maps to 0..1
static float axisDeadzone(float v, uint32_t innerPct, uint32_t outerPct) {
    float inner = (float)innerPct / 100.0f;
    float outer = (outerPct == 0 ? 100.0f : (float)outerPct) / 100.0f;
    if (outer <= inner + 0.001f) outer = inner + 0.001f;
    float av = fabsf(v);
    if (av <= inner) return 0.0f;
    float nv = (av - inner) / (outer - inner);
    if (nv > 1.0f) nv = 1.0f;
    return v >= 0 ? nv : -nv;
}

// FireBird アンチデッドゾーン: minimum output once outside the inner deadzone
static float antiDeadzone(float v, uint32_t antiPct) {
    if (antiPct == 0 || fabsf(v) < 0.0001f) return v;
    float anti = (float)antiPct / 100.0f;
    float av = anti + fabsf(v) * (1.0f - anti);
    if (av > 1.0f) av = 1.0f;
    return v >= 0 ? av : -av;
}

// Snapback filter approximation: RC low-pass (same formula as the positive RC
// filter, R = strength*5) applied only while the axis is returning toward /
// crossing center, so press response is untouched
static float snapbackAxis(float v, float& filt, float strength, float dtMs) {
    if (strength < 0.5f) { filt = v; return v; }
    bool returning = (v * filt < 0.0f) || (fabsf(v) < fabsf(filt));
    if (returning) {
        float q = 100.0f / dtMs;
        float r = strength * 5.0f;
        filt += (v - filt) * q / (q + r);
    } else {
        filt = v;
    }
    return filt;
}

// FireBird 角度補正 map: enabled entries (in -> out, snap deadzone) remap the
// angle piecewise-linearly around the circle. Angles in degrees, 0=up, cw.
static float applyAngleMap(const HMLStickOptions& s, float ang) {
    uint32_t total = s.angleMapCount;
    if (total > s.angleMapIn100_count) total = s.angleMapIn100_count;
    if (total > s.angleMapOut100_count) total = s.angleMapOut100_count;
    if (total > 16) total = 16;
    // gather enabled entries (assumed sorted by input angle in the UI)
    float in[16], out[16], dz[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!(s.angleMapEnable & (1u << i))) continue;
        in[n] = (float)s.angleMapIn100[i] / 100.0f;
        out[n] = (float)s.angleMapOut100[i] / 100.0f;
        dz[n] = (i < s.angleMapDz100_count) ? (float)s.angleMapDz100[i] / 100.0f : 0.0f;
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

// Firebird RC filter, bit-faithful to stock FireBird v3.08 (disassembled):
// R > 0:  filt += (raw - filt) * q / (q + R)   with q = 100 at a 1kHz loop
//         (alpha = 100/(100+R); R=+500 is only ~5ms of lag -- jitter dies in
//         the console's 8-bit output quantization, response stays crisp)
// R < 0:  B = -R; base low-pass with w = 200 at 1kHz, then a velocity lead
//         term (B+25)/25 per sample fed back into the state
static float rcFilterAxis(float raw, float prevRaw, float& filt, float strength, float dtMs) {
    if (strength > 0.5f) {
        float q = 100.0f / dtMs;
        filt += (raw - filt) * q / (q + strength);
    } else if (strength < -0.5f) {
        float b = -strength;
        float w = 200.0f / dtMs;
        float smoothed = (raw * w + filt * b) / (w + b);
        filt = smoothed + (raw - prevRaw) * (b + 25.0f) / 25.0f;
        if (filt > 1.25f) filt = 1.25f;
        if (filt < -1.25f) filt = -1.25f;
    } else {
        filt = raw;
    }
    return filt;
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
    if (s.rcAdvanced) {
        float velMult = (s.rcVelocityMult100 > 0 ? s.rcVelocityMult100 : 100) / 100.0f;
        float dx = rawX - st.prevRawX;
        float dy = rawY - st.prevRawY;
        float vel = sqrtf(dx*dx + dy*dy) / dtMs;   // normalized units per ms
        float vel128 = vel * 64.0f * velMult;
        if (vel128 > 128.0f) vel128 = 128.0f;
        strength = rcStrengthForVelocity(s, vel128);
    }

    float fx = rcFilterAxis(rawX, st.prevRawX, st.filtX, strength, dtMs);
    float fy = rcFilterAxis(rawY, st.prevRawY, st.filtY, strength, dtMs);
    st.prevRawX = rawX;
    st.prevRawY = rawY;

    // Snapback filter: damp only the return-to-center phase (approximation of
    // FireBird's snapback filter; strength 0 = off)
    fx = snapbackAxis(fx, st.snapX, (float)s.rcSnapback, dtMs);
    fy = snapbackAxis(fy, st.snapY, (float)s.rcSnapback, dtMs);

    // FireBird 入力スケール: per-direction gain (100 = calibrated range)
    fx *= (fx >= 0 ? scalePct(s.scaleRight) : scalePct(s.scaleLeft));
    fy *= (fy >= 0 ? scalePct(s.scaleUp) : scalePct(s.scaleDown));

    // FireBird per-axis deadzones: inner (0 = none) and outer (100 = full
    // travel needed for max output)
    fx = axisDeadzone(fx, s.innerX, s.outerX);
    fy = axisDeadzone(fy, s.innerY, s.outerY);

    float mag = sqrtf(fx*fx + fy*fy);
    if (mag < 0.0001f) {
        emitStick(stickOptsV, st, 0.0f, 0.0f, outX, outY);
        return;
    }
    float outMag = applyCurve(std::min(mag, 1.0f), stickOptsV);

    // FireBird 角度補正: angle map -> global offset -> angle snap.
    // Angle convention matches the FireBird UI: 0=up, clockwise, degrees.
    float ca = fx / mag, sa = fy / mag;
    if (s.angleOffset100 != 0 || s.angleSnap != 0 ||
        (s.angleMapCount > 0 && s.angleMapEnable != 0)) {
        float ang = atan2f(ca, sa) * 57.29578f; // 0=up, cw
        if (ang < 0) ang += 360.0f;
        ang = applyAngleMap(s, ang);
        ang += (float)s.angleOffset100 / 100.0f;
        if (s.angleSnap == 1) ang = roundf(ang / 45.0f) * 45.0f;
        else if (s.angleSnap == 2) ang = roundf(ang / 90.0f) * 90.0f;
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
        float maxSquare = std::min(1.0f / absCa, 1.0f / absSa);
        float ex = 1.0f + 0.4142f * (float)s.diagScaleX / 100.0f;
        float ey = 1.0f + 0.4142f * (float)s.diagScaleY / 100.0f;
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
void HMLAnalogAddon::emitStick(const void* stickOptsV, HMLStickState& st, float x, float y, float& outX, float& outY) {
    const HMLStickOptions& s = *(const HMLStickOptions*)stickOptsV;
    // スティック分解能: quantize to N steps across the full -1..1 range
    if (s.resolution >= 2) {
        float steps = (float)(s.resolution - 1);
        x = roundf((x + 1.0f) * 0.5f * steps) / steps * 2.0f - 1.0f;
        y = roundf((y + 1.0f) * 0.5f * steps) / steps * 2.0f - 1.0f;
    }
    // 追加入力遅延: ring buffer, replay the sample from delayMs ago
    if (s.delayMs > 0) {
        uint32_t now = to_us_since_boot(get_absolute_time());
        st.dlyX[st.dlyHead] = x;
        st.dlyY[st.dlyHead] = y;
        st.dlyT[st.dlyHead] = now;
        st.dlyHead = (st.dlyHead + 1) % HML_DELAY_SLOTS;
        if (st.dlyCount < HML_DELAY_SLOTS) st.dlyCount++;
        uint32_t target = now - s.delayMs * 1000;
        // oldest -> newest, keep the newest sample that is old enough
        float dx = x, dy = y;
        bool found = false;
        for (uint8_t i = 0; i < st.dlyCount; i++) {
            uint8_t idx = (st.dlyHead + HML_DELAY_SLOTS - st.dlyCount + i) % HML_DELAY_SLOTS;
            if ((int32_t)(st.dlyT[idx] - target) > 0) break;
            dx = st.dlyX[idx];
            dy = st.dlyY[idx];
            found = true;
        }
        if (found) { x = dx; y = dy; }
        else { x = 0.0f; y = 0.0f; } // buffer not old enough yet
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
    for (int it = 0; it < iterations; it++) {
        instance->ads->readAll(ch);
        for (int i = 0; i < ADS8332_CHANNEL_COUNT; i++) {
            if (ch[i] < mn[i]) mn[i] = ch[i];
            if (ch[i] > mx[i]) mx[i] = ch[i];
        }
    }
    return true;
}

void HMLAnalogAddon::preprocess() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (ads == nullptr) return;

    // Self-heal: after a warm reboot the ADC may still be desynced when the
    // first probe runs; keep retrying the link until it comes up.
    if (!ads->linkOk) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - adsRetryMs) >= 0) {
            adsRetryMs = now + 500;
            ads->init();
        }
        if (!ads->linkOk) return; // sticks stay centered until the link is up
    }

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
    // Only report valid once a complete scan has run: otherwise the zeroed
    // buffer reads as full deflection (stick pinned to a corner) after boot
    rawValid = ads->scanCompleted;

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
static uint16_t gateAxis(uint16_t raw, uint16_t& held, uint32_t band) {
    if (band == 0) { held = raw; return raw; }
    // 静止時ジッターの抑制が目的なのでバンドはフルスケール5%まで: 計測異常で
    // 巨大な値が保存されても軸が凍らないようにする
    if (band > 3276) band = 3276;
    int32_t d = (int32_t)raw - (int32_t)held;
    if (d > (int32_t)band || d < -(int32_t)band) held = raw;
    return held;
}

void HMLAnalogAddon::process() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!adcSource) {
        processGamepadState();
        lastMicros = to_us_since_boot(get_absolute_time());
        return;
    }
    if (!rawValid) return;
    Gamepad *gamepad = Storage::getInstance().GetGamepad();

    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    if (opts.chLX >= 0 && opts.chLY >= 0) {
        const HMLStickOptions& s = opts.leftStick;
        uint16_t rx16 = rawChannels[opts.chLX], ry16 = rawChannels[opts.chLY];
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
        gamepad->state.lx = (uint16_t)std::clamp((lx + 1.0f) * 32767.5f, 0.0f, 65535.0f);
        gamepad->state.ly = (uint16_t)std::clamp((ly + 1.0f) * 32767.5f, 0.0f, 65535.0f);
    }
    if (opts.chRX >= 0 && opts.chRY >= 0) {
        const HMLStickOptions& s = opts.rightStick;
        uint16_t rx16 = rawChannels[opts.chRX], ry16 = rawChannels[opts.chRY];
        if (s.noiseSuppress) {
            if (!rightState.heldInit) { rightState.heldX = rx16; rightState.heldY = ry16; rightState.heldInit = true; }
            rx16 = gateAxis(rx16, rightState.heldX, s.noiseX);
            ry16 = gateAxis(ry16, rightState.heldY, s.noiseY);
        }
        float rawX = -calibrate(rx16, s.calMinX, s.calCenterX, s.calMaxX);
        float rawY = calibrate(ry16, s.calMinY, s.calCenterY, s.calMaxY);
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

    lightbarUpdate();

    lastMicros = to_us_since_boot(get_absolute_time());
}

// ---------------------------------------------------------------------------
// Gate calibration
//
// A stick gate is squarish, and differently so per controller, so the same
// physical deflection reaches a different magnitude depending on direction.
// Worse, everything past the unit circle used to be thrown away by the
// clamp in processStick, which is most of the reach in the diagonals on a
// square-gated stick. Measuring the gate and dividing by it makes "pushed all
// the way" equal 1.0 in every direction and on every controller; the FireBird
// diagonal scaling then shapes the output, identically for all of them.
// ---------------------------------------------------------------------------

uint16_t HMLAnalogAddon::calRadius[HML_GATE_BINS] = {0};
uint16_t HMLAnalogAddon::calCenter[2] = {0, 0};
uint8_t  HMLAnalogAddon::calStick = 0;
static bool     calRunning = false;
static uint32_t calCenterAcc[2] = {0, 0};
static uint32_t calCenterCount = 0;
static uint16_t calAxisMin[2], calAxisMax[2];

// Accumulate the sweep far finer than it is stored. Taking the largest radius
// across a whole 22.5 degree wedge and then treating it as the radius at the
// wedge centre badly overstates it wherever the gate turns quickly - on a
// square gate the error reaches 15% approaching the corners, which shows up as
// not being able to reach full output. Sampling at 2.8 degrees and reading off
// the value at each stored direction keeps that under a couple of percent,
// which the outer deadzone absorbs.
#define CAL_FINE_BINS 128
static uint16_t calFine[CAL_FINE_BINS];

// Samples averaged for the centre before the sweep is recorded. At loop rate
// this is a fraction of a second, so "let go, then start" is enough.
#define CAL_CENTER_SAMPLES 256

// Direction of (x, y) in bins, and the raw magnitude in the same units the
// radius table stores (1/1000 of half-scale).
// Unit vectors of the stored directions, built once. Recomputing them with
// sinf/cosf every sample would be a few thousand trig calls a second on a chip
// with no FPU, for sixteen constants.
static float gateSin[HML_GATE_BINS], gateCos[HML_GATE_BINS];
static bool  gateDirInit = false;
static void gateDirBuild() {
    if (gateDirInit) return;
    for (int i = 0; i < HML_GATE_BINS; i++) {
        float a = (float)i * (2.0f * 3.14159265f / HML_GATE_BINS);
        gateSin[i] = sinf(a);
        gateCos[i] = cosf(a);
    }
    gateDirInit = true;
}
static const float* gateDirSin() { gateDirBuild(); return gateSin; }
static const float* gateDirCos() { gateDirBuild(); return gateCos; }

static inline float calAngleDeg(float x, float y) {
    float a = atan2f(x, y) * 57.29578f;
    if (a < 0.0f) a += 360.0f;
    return a;
}

// Read the stored directions out of the fine sweep: value at the bin centre,
// widening only far enough to find a sample if that exact slice was missed.
static void calBuildCoarse(uint16_t* out) {
    const int perBin = CAL_FINE_BINS / HML_GATE_BINS;
    for (int i = 0; i < HML_GATE_BINS; i++) {
        int centre = i * perBin;
        uint16_t v = 0;
        for (int w = 0; w < perBin && v == 0; w++) {
            int a = (centre - w + CAL_FINE_BINS) % CAL_FINE_BINS;
            int b = (centre + w) % CAL_FINE_BINS;
            v = std::max(calFine[a], calFine[b]);
        }
        out[i] = v;
    }
}

void HMLAnalogAddon::calStart(uint8_t stick) {
    calStick = stick > 1 ? 1 : stick;
    memset(calRadius, 0, sizeof(calRadius));
    memset(calFine, 0, sizeof(calFine));
    calCenterAcc[0] = calCenterAcc[1] = 0;
    calCenterCount = 0;
    calCenter[0] = calCenter[1] = 0;
    calAxisMin[0] = calAxisMin[1] = 0xFFFF;
    calAxisMax[0] = calAxisMax[1] = 0;
    calRunning = true;
}

void HMLAnalogAddon::calAbort() { calRunning = false; }
bool HMLAnalogAddon::calActive() { return calRunning; }

uint8_t HMLAnalogAddon::calCoverage() {
    calBuildCoarse(calRadius);
    uint8_t n = 0;
    for (int i = 0; i < HML_GATE_BINS; i++) if (calRadius[i] > 0) n++;
    return (uint8_t)(n * 100 / HML_GATE_BINS);
}

void HMLAnalogAddon::calTick() {
    if (!calRunning) return;
    Gamepad *gamepad = Storage::getInstance().GetGamepad();
    uint16_t rx = calStick == 0 ? gamepad->state.lx : gamepad->state.rx;
    uint16_t ry = calStick == 0 ? gamepad->state.ly : gamepad->state.ry;

    // Centre first, from the stick sitting at rest
    if (calCenterCount < CAL_CENTER_SAMPLES) {
        calCenterAcc[0] += rx;
        calCenterAcc[1] += ry;
        if (++calCenterCount == CAL_CENTER_SAMPLES) {
            calCenter[0] = (uint16_t)(calCenterAcc[0] / CAL_CENTER_SAMPLES);
            calCenter[1] = (uint16_t)(calCenterAcc[1] / CAL_CENTER_SAMPLES);
        }
        return;
    }

    if (rx < calAxisMin[0]) calAxisMin[0] = rx;
    if (rx > calAxisMax[0]) calAxisMax[0] = rx;
    if (ry < calAxisMin[1]) calAxisMin[1] = ry;
    if (ry > calAxisMax[1]) calAxisMax[1] = ry;

    float x = ((float)rx - (float)calCenter[0]) / 32768.0f;
    float y = ((float)ry - (float)calCenter[1]) / 32768.0f;
    float mag = sqrtf(x * x + y * y);
    if (mag < 0.15f) return;  // near centre the direction is meaningless

    int bin = (int)(calAngleDeg(x, y) / (360.0f / CAL_FINE_BINS));
    if (bin < 0) bin = 0;
    if (bin >= CAL_FINE_BINS) bin = CAL_FINE_BINS - 1;
    uint16_t r = (uint16_t)(mag * 1000.0f);
    if (r > calFine[bin]) calFine[bin] = r;
}

bool HMLAnalogAddon::calSave(uint16_t vendorId, uint16_t productId) {
    calRunning = false;
    // Every direction must have been reached: a partial sweep would normalize
    // the missing ones against interpolated guesses
    if (calCoverage() < 100 || calCenterCount < CAL_CENTER_SAMPLES) return false;

    HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;

    HMLCalProfile* slot = nullptr;
    for (pb_size_t i = 0; i < opts.calProfiles_count; i++) {
        if (opts.calProfiles[i].vendorId == vendorId && opts.calProfiles[i].productId == productId) {
            slot = &opts.calProfiles[i];
            break;
        }
    }
    if (slot == nullptr) {
        if (opts.calProfiles_count >= 8) return false;   // full; drop one first
        slot = &opts.calProfiles[opts.calProfiles_count++];
        *slot = HMLCalProfile HMLCalProfile_init_zero;
    }
    slot->vendorId = vendorId;   slot->has_vendorId = true;
    slot->productId = productId; slot->has_productId = true;

    HMLGateProfile& g = (calStick == 0) ? slot->leftGate : slot->rightGate;
    if (calStick == 0) slot->has_leftGate = true; else slot->has_rightGate = true;
    g = HMLGateProfile HMLGateProfile_init_zero;
    g.calCenterX = calCenter[0]; g.has_calCenterX = true;
    g.calCenterY = calCenter[1]; g.has_calCenterY = true;
    g.calMinX = calAxisMin[0];   g.has_calMinX = true;
    g.calMaxX = calAxisMax[0];   g.has_calMaxX = true;
    g.calMinY = calAxisMin[1];   g.has_calMinY = true;
    g.calMaxY = calAxisMax[1];   g.has_calMaxY = true;
    calBuildCoarse(calRadius);
    g.radius_count = HML_GATE_BINS;
    for (int i = 0; i < HML_GATE_BINS; i++) g.radius[i] = calRadius[i];
    g.valid = true; g.has_valid = true;

    EventManager::getInstance().triggerEvent(new GPStorageSaveEvent(true));
    return true;
}

const void* HMLAnalogAddon::gateFor(uint8_t stick) {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    if (!opts.gateNormalize) return nullptr;
    const auto& m = GamepadUSBHostListener::lastMount;
    if (!m.seen) return nullptr;
    for (pb_size_t i = 0; i < opts.calProfiles_count; i++) {
        const HMLCalProfile& p = opts.calProfiles[i];
        if (p.vendorId != m.vid || p.productId != m.pid) continue;
        const HMLGateProfile& g = (stick == 0) ? p.leftGate : p.rightGate;
        return g.valid ? (const void*)&g : nullptr;
    }
    return nullptr;
}

bool HMLAnalogAddon::applyGate(const void* gateV, uint16_t rawX, uint16_t rawY, float& outX, float& outY) {
    if (gateV == nullptr) return false;
    const HMLGateProfile& g = *(const HMLGateProfile*)gateV;
    if (!g.valid || g.radius_count < HML_GATE_BINS) return false;

    float x = ((float)rawX - (float)g.calCenterX) / 32768.0f;
    float y = ((float)rawY - (float)g.calCenterY) / 32768.0f;
    float mag = sqrtf(x * x + y * y);
    if (mag < 1e-5f) { outX = 0.0f; outY = 0.0f; return true; }

    // Reconstruct the gate as a polygon through the measured points and
    // intersect the ray with it, rather than interpolating radius against
    // angle. These gates are squarish, and a straight edge is a straight line
    // in x/y but a 1/cos curve in polar - interpolating in polar cuts about 5%
    // off the reach midway between samples, while the polygon sits exactly on
    // a flat edge. Directions land on multiples of the step, so both the axes
    // and the diagonals - the corners of the shape - are stored samples.
    const float step = 360.0f / HML_GATE_BINS;
    float a = calAngleDeg(x, y) / step;
    int i0 = (int)a;
    int i1 = (i0 + 1) % HML_GATE_BINS;
    i0 %= HML_GATE_BINS;
    float r0 = (float)g.radius[i0] / 1000.0f;
    float r1 = (float)g.radius[i1] / 1000.0f;
    if (r0 < 0.05f || r1 < 0.05f) return false;   // implausible measurement

    const float* ds = gateDirSin();
    const float* dc = gateDirCos();
    float p0x = r0 * ds[i0], p0y = r0 * dc[i0];
    float p1x = r1 * ds[i1], p1y = r1 * dc[i1];
    float ex = p1x - p0x, ey = p1y - p0y;
    float dx = x / mag, dy = y / mag;              // ray direction
    float den = dx * ey - dy * ex;
    if (fabsf(den) < 1e-6f) return false;
    float r = (p0x * ey - p0y * ex) / den;
    if (r < 0.05f) return false;

    float scale = 1.0f / r;
    outX = std::clamp(x * scale, -1.0f, 1.0f);
    outY = std::clamp(y * scale, -1.0f, 1.0f);
    // Full deflection must not exceed the unit circle, or the clamp in
    // processStick starts discarding reach again
    float om = sqrtf(outX * outX + outY * outY);
    if (om > 1.0f) { outX /= om; outY /= om; }
    return true;
}

// Gamepad-state stick source (MeisterConverter: DualSense -> USB host add-on).
// The pair is already normalized 0..65535 by the host driver, so there is no
// ADC scan and no hardware X inversion to undo. Everything downstream - noise
// gate, RC filter, snapback, scale, deadzones, curve, angle, diagonal, output
// correction - is the same code the ADS8332 path runs; only the source differs.
//
// calibrate() is still used: with the defaults (all zero) it falls through to a
// plain (raw-32768)/32768 normalize, and if the user does calibrate it trims a
// drifting DualSense center for free. Note the DualSense is an 8-bit stick
// widened to 16 bits (center lands on 32896, one step = 257 counts), so the
// noise gate needs a band above 257 to do anything - the real wins here are the
// RC filter, curve and deadzones.
void HMLAnalogAddon::processGamepadState() {
    const HMLAnalogOptions& opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    Gamepad *gamepad = Storage::getInstance().GetGamepad();

    auto runStick = [&](const HMLStickOptions& s, HMLStickState& st,
                        uint16_t& axX, uint16_t& axY, uint8_t stickIndex) {
        uint16_t rx16 = axX, ry16 = axY;
        if (s.noiseSuppress) {
            if (!st.heldInit) { st.heldX = rx16; st.heldY = ry16; st.heldInit = true; }
            rx16 = gateAxis(rx16, st.heldX, s.noiseX);
            ry16 = gateAxis(ry16, st.heldY, s.noiseY);
        }
        // A measured gate supersedes the per-axis calibration: it carries its
        // own centre and normalizes every direction, not just the axes.
        float rawX, rawY;
        if (!applyGate(gateFor(stickIndex), rx16, ry16, rawX, rawY)) {
            rawX = calibrate(rx16, s.calMinX, s.calCenterX, s.calMaxX);
            rawY = calibrate(ry16, s.calMinY, s.calCenterY, s.calMaxY);
        }
        float outX = 0.0f, outY = 0.0f;
        processStick(&s, st, rawX, rawY, outX, outY);
        axX = (uint16_t)std::clamp((outX + 1.0f) * 32767.5f, 0.0f, 65535.0f);
        axY = (uint16_t)std::clamp((outY + 1.0f) * 32767.5f, 0.0f, 65535.0f);
    };

    runStick(opts.leftStick,  leftState,  gamepad->state.lx, gamepad->state.ly, 0);
    runStick(opts.rightStick, rightState, gamepad->state.rx, gamepad->state.ry, 1);
}
