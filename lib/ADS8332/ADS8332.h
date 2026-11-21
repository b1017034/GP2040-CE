// ADS8332 driver for GP2040-CE (HML board)
// TI ADS8332: 16-bit, 8-channel, 500kSPS SAR ADC, SPI interface.
//
// The exact wiring of the vendor board is unknown (CS pin and SPI phase are
// hardcoded in the stock firmware), so init() auto-probes:
//   Strategy A: hardware CS on the configured peripheral CS pin (PL022 frames
//               every 16-bit word), manual channel select mode.
//   Strategy B: software CS on each candidate GPIO, auto channel sequencing
//               with TAG bits (32-clock frames).
// Every candidate (CS x SPI mode x 1-bit stitch) is validated by writing the
// CFR register and reading it back. Diagnostics are exposed for webconfig.

#ifndef _ADS8332_H_
#define _ADS8332_H_

#include "peripheral_spi.h"

#define ADS8332_CHANNEL_COUNT 8

#ifndef ADS8332_SPI_SPEED_HZ
#define ADS8332_SPI_SPEED_HZ 2000000
#endif

// Software-CS auto-sequence read path:
// Per-channel sliding median over the last N tagged samples. A single
// mispaired frame (tag/data race) is an outlier the median drops, same as the
// old block median, but every new sample updates the output immediately
// instead of waiting for a whole multi-sweep block. 3 rejects one outlier, 5
// rejects two (one sample more lag per step).
#ifndef ADS8332_MEDIAN_WINDOW
#define ADS8332_MEDIAN_WINDOW 3
#endif
// SPI frames per readAll() call. 8 = one sequencer sweep, so every channel
// normally gets one fresh sample per call (~185us at 2MHz).
#ifndef ADS8332_BURSTS_PER_CALL
#define ADS8332_BURSTS_PER_CALL 8
#endif
// Link health window: within this many frames every channel must have been
// seen at least ADS8332_HEALTH_MIN_SAMPLES times, else the window counts as
// unhealthy; 3 unhealthy windows in a row drop the link for a re-probe.
#define ADS8332_HEALTH_WINDOW 48
#define ADS8332_HEALTH_MIN_SAMPLES 3

// CMR commands (upper 4 bits of the 16-bit frame)
#define ADS8332_CMD_SELECT_CH(ch) ((uint16_t)((ch) & 0x7) << 12)
#define ADS8332_CMD_WAKEUP        0xB000
#define ADS8332_CMD_READ_CFR      0xC000
#define ADS8332_CMD_READ_DATA     0xD000
#define ADS8332_CMD_WRITE_CFR     0xE000
#define ADS8332_CMD_DEFAULT_MODE  0xF000

// Common CFR bits: D10=1 internal conversion clock, D9=0 auto-trigger,
// D8=0 500kSPS, D7=0/D6=1/D5=1 EOC output (unused), D4=1 no auto-nap,
// D3=1/D2=1 no power-down, D0=1 normal operation
// Strategy A: D11=0 manual channel select, D1=0 TAG off
#define ADS8332_CFR_MANUAL   0x047D
// Auto mode: D11=1 auto channel sequencing, D10=0 CCLK=SCLK/2 (conversions
// advance ONLY while SPI clocks run - the sequencer freezes between frames,
// so TAG and data stay deterministically paired), D1=1 TAG on
#define ADS8332_CFR_AUTO_TAG 0x087F

class ADS8332 {
public:
    ADS8332(PeripheralSPI *spi, int8_t csPin, int8_t rxPin)
        : spi(spi), csPin(csPin), rxPin(rxPin) {}

    void init();
    // Read all 8 channels; channels not read keep their previous values.
    // Software-CS mode does ADS8332_BURSTS_PER_CALL SPI frames per call and
    // updates each channel's sliding median as its samples arrive; values[]
    // is valid once scanCompleted is set (every channel has a full window).
    void readAll(uint16_t values[ADS8332_CHANNEL_COUNT]);

    // Diagnostics (exposed via webconfig)
    bool linkOk = false;        // CFR readback succeeded during probe
    bool scanCompleted = false; // at least one full 8-channel scan finished
    uint8_t probedMode = 0xFF;  // winning SPI mode (0-3)
    int8_t probedCs = -1;       // winning CS pin
    bool hwCs = false;          // PL022 hardware CS in use
    bool stitch = false;        // 1-bit early reconstruction in use
    // Full scan results: every CS pin that answered CFR readback
    int8_t foundCs[4] = { -1, -1, -1, -1 };
    uint8_t foundModes[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t foundHw[4] = { 0, 0, 0, 0 };
    uint8_t foundCount = 0;

private:
    void setFormat(uint8_t mode);
    uint8_t burst(const uint16_t *tx, uint16_t *rx, uint8_t count);
    void decode(uint8_t msb, const uint16_t rx[2], uint16_t &value, uint8_t &tag) const;
    bool checkCfr(uint16_t cfrValue);
    // Read a batch of auto-sequence frames and verify the TAG stream is sane
    // (CFR readback can pass while the data path is bit-misaligned)
    bool checkData();
    // checkData with automatic stitch-flip retry
    bool validateLink();

    PeripheralSPI *spi;
    int8_t csPin;
    int8_t rxPin;

    void resetScanState();

    // Sliding-median state per channel (software CS mode)
    uint16_t hist[ADS8332_CHANNEL_COUNT][ADS8332_MEDIAN_WINDOW];
    uint8_t histCount[ADS8332_CHANNEL_COUNT] = {0}; // samples held (<= window)
    uint8_t histPos[ADS8332_CHANNEL_COUNT] = {0};   // next write slot
    // Link health (see ADS8332_HEALTH_WINDOW)
    uint8_t healthSeen[ADS8332_CHANNEL_COUNT] = {0};
    uint8_t healthFrames = 0;
    // Consecutive unhealthy windows (TAG stream broken) -> drop the link
    uint8_t cappedScans = 0;
};

#endif
