#include "ADS8332.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

// CS pin candidates: the configured peripheral CS first, then every pin the
// stock firmware marks ASSIGNED_TO_ADDON with no other known function.
static const int8_t CS_CANDIDATES[] = { 1, 4, 5, 6, 7, 10, 11, 20, 24, 25 };
#define CS_CANDIDATE_COUNT (sizeof(CS_CANDIDATES) / sizeof(CS_CANDIDATES[0]))

void ADS8332::setFormat(uint8_t mode) {
    spi->beginTransaction(ADS8332_SPI_SPEED_HZ, spi_order_t::SPI_MSB_FIRST,
                          (SPIMode)mode);
    spi_cpol_t cpol = (mode == 2 || mode == 3) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (mode == 1 || mode == 3) ? SPI_CPHA_1 : SPI_CPHA_0;
    // PeripheralSPI hardcodes 8-bit frames; the ADS8332 uses 16-bit words
    spi_set_format(spi->getController(), 16, cpol, cpha, SPI_MSB_FIRST);
    spi->endTransaction();
}

uint8_t ADS8332::burst(const uint16_t *tx, uint16_t *rx, uint8_t count) {
    if (!hwCs) {
        gpio_put(csPin, 0);
        busy_wait_us(1); // tD3: MSB valid 35ns after CS falling edge
    }
    // With software CS the data MSB sits on SDO before the first SCLK; capture
    // it from the pad (SIO input works even while the pin is muxed to SPI).
    uint8_t msb = gpio_get(rxPin) ? 1 : 0;
    spi_write16_read16_blocking(spi->getController(), tx, rx, count);
    if (!hwCs) {
        busy_wait_us(1); // tSU5/tH4 margins
        gpio_put(csPin, 1);
        busy_wait_us(1); // tWH1: CS high >= 40ns between frames
    }
    return msb;
}

void ADS8332::decode(uint8_t msb, const uint16_t rx[2], uint16_t &value, uint8_t &tag) const {
    if (stitch) {
        // Sampled stream is one bit early: rx[0] = data<<1 | tag3, and the
        // true data MSB was captured from the pad at CS assertion.
        value = (uint16_t)(msb << 15) | (rx[0] >> 1);
        tag = (uint8_t)(((rx[0] & 1) << 3) | (rx[1] >> 13));
    } else {
        // word0 = 16-bit conversion result; word1 top 3 bits [15:13] = channel
        // TAG (bit12 is a trailing zero). Reading >>12 loses the tag LSB and
        // only ever yields even channels, so use >>13.
        value = rx[0];
        tag = (uint8_t)(rx[1] >> 13);
    }
}

// Program CFR then validate by reading it back. The Read CFR response is
// [previous data 15:12][CFR 11:0] and arrives within the same 16-bit frame.
bool ADS8332::checkCfr(uint16_t cfrValue) {
    uint16_t rx[2];
    uint16_t cmd;
    cmd = ADS8332_CMD_DEFAULT_MODE; burst(&cmd, rx, 1);
    busy_wait_us(20);
    cmd = ADS8332_CMD_WAKEUP;       burst(&cmd, rx, 1);
    busy_wait_us(20);
    cmd = ADS8332_CMD_WRITE_CFR | cfrValue; burst(&cmd, rx, 1);
    busy_wait_us(20);

    uint8_t directOk = 0, stitchOk = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t r[2] = { 0, 0 };
        uint16_t tx1 = ADS8332_CMD_READ_CFR;
        uint8_t msb = burst(&tx1, r, 1);
        if ((r[0] & 0x0FFF) == cfrValue) directOk++;
        uint16_t st = (uint16_t)(msb << 15) | (r[0] >> 1);
        if ((st & 0x0FFF) == cfrValue) stitchOk++;
        busy_wait_us(10);
    }
    if (directOk >= 3) { stitch = false; return true; }
    if (stitchOk >= 3) { stitch = true;  return true; }
    return false;
}

void ADS8332::init() {
    static const uint8_t order[4] = { 2, 3, 1, 0 };
    int8_t cfgCs = csPin;

    // Strategy A (preferred): software CS on the configured pin with AUTO
    // channel sequencing + TAG bits. Each 32-clock frame self-identifies its
    // channel, so we never depend on manual-select timing being honoured -
    // this is what fixes "all 8 channels return the same axis".
    if (cfgCs >= 0) {
        gpio_init(cfgCs);
        gpio_set_dir(cfgCs, GPIO_OUT);
        gpio_put(cfgCs, 1);
        busy_wait_us(10);
        hwCs = false;
        for (uint8_t i = 0; i < 4; i++) {
            setFormat(order[i]);
            if (checkCfr(ADS8332_CFR_AUTO_TAG)) {
                probedMode = order[i];
                probedCs = cfgCs;
                linkOk = true;
                return;
            }
        }
    }

    // Strategy B: hardware CS, manual channel select (fallback if the software
    // CS timing above cannot establish the link).
    if (cfgCs >= 0) {
        gpio_set_function(cfgCs, GPIO_FUNC_SPI);
        hwCs = true;
        for (uint8_t i = 0; i < 4; i++) {
            setFormat(order[i]);
            if (checkCfr(ADS8332_CFR_MANUAL)) {
                probedMode = order[i];
                probedCs = cfgCs;
                linkOk = true;
                return;
            }
        }
        gpio_set_function(cfgCs, GPIO_FUNC_NULL);
    }

    // Strategy C: software CS on other candidate pins.
    hwCs = false;
    for (uint8_t p = 0; p < CS_CANDIDATE_COUNT; p++) {
        int8_t pin = CS_CANDIDATES[p];
        if (pin == cfgCs) continue;
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 1);
        busy_wait_us(10);
        csPin = pin;
        for (uint8_t i = 0; i < 4; i++) {
            setFormat(order[i]);
            if (checkCfr(ADS8332_CFR_AUTO_TAG)) {
                probedMode = order[i];
                probedCs = pin;
                linkOk = true;
                return;
            }
        }
        gpio_set_dir(pin, GPIO_IN);
        gpio_deinit(pin);
    }
    linkOk = false;
}

void ADS8332::readAll(uint16_t values[ADS8332_CHANNEL_COUNT]) {
    if (!linkOk) return;

    if (hwCs) {
        // Manual channel select + read, one 16-bit frame each. Auto-trigger
        // keeps converting the selected channel; wait one full conversion
        // after switching before reading.
        for (uint8_t ch = 0; ch < ADS8332_CHANNEL_COUNT; ch++) {
            uint16_t rx[2];
            uint16_t cmd = ADS8332_CMD_SELECT_CH(ch);
            burst(&cmd, rx, 1);
            busy_wait_us(6); // acquisition + conversion at 500kSPS ~= 4us
            cmd = ADS8332_CMD_READ_DATA;
            uint8_t msb = burst(&cmd, rx, 1);
            uint16_t v = stitch ? (uint16_t)((msb << 15) | (rx[0] >> 1)) : rx[0];
            values[ch] = v;
        }
        return;
    }

    // Software CS + auto-sequencing + TAG binning. Manual channel select is
    // silently ignored by this part, so auto mode is the only way to address
    // all channels. Individual frames occasionally mispair tag and data
    // (read races the sequencer), so gather many frames and take the median
    // per bin - mispairs become outliers and drop out.
    uint16_t tx[2] = { ADS8332_CMD_READ_DATA, 0x0000 };
    uint16_t samples[ADS8332_CHANNEL_COUNT][5];
    uint8_t counts[ADS8332_CHANNEL_COUNT] = {0};
    for (uint8_t attempt = 0; attempt < 48; attempt++) {
        uint16_t rx[2];
        uint8_t msb = burst(tx, rx, 2);
        uint16_t value; uint8_t tag;
        decode(msb, rx, value, tag);
        if (tag < ADS8332_CHANNEL_COUNT && counts[tag] < 5) {
            samples[tag][counts[tag]++] = value;
        }
        busy_wait_us(4); // let the sequencer advance
        // stop early once every bin has enough samples for a median
        bool done = true;
        for (uint8_t i = 0; i < ADS8332_CHANNEL_COUNT; i++)
            if (counts[i] < 3) { done = false; break; }
        if (done) break;
    }
    for (uint8_t ch = 0; ch < ADS8332_CHANNEL_COUNT; ch++) {
        uint8_t n = counts[ch];
        if (n == 0) continue;              // keep previous value
        // insertion-sort the few samples and take the middle one
        uint16_t *s = samples[ch];
        for (uint8_t i = 1; i < n; i++) {
            uint16_t key = s[i];
            int8_t j = i - 1;
            while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; j--; }
            s[j + 1] = key;
        }
        values[ch] = s[n / 2];
    }
}
