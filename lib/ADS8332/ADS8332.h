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
// Strategy A: D11=0 manual channel select, D8=1 250kSPS (doubled acquisition
// window - hall sensors have high source impedance and need S/H settle time),
// D1=0 TAG off
#define ADS8332_CFR_MANUAL   0x057D
// Strategy B: D11=1 auto channel sequencing, D1=1 TAG on
#define ADS8332_CFR_AUTO_TAG 0x0D7F

class ADS8332 {
public:
    ADS8332(PeripheralSPI *spi, int8_t csPin, int8_t rxPin)
        : spi(spi), csPin(csPin), rxPin(rxPin) {}

    void init();
    // Read all 8 channels; channels not read keep their previous values.
    void readAll(uint16_t values[ADS8332_CHANNEL_COUNT]);

    // Diagnostics (exposed via webconfig)
    bool linkOk = false;        // CFR readback succeeded during probe
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

    PeripheralSPI *spi;
    int8_t csPin;
    int8_t rxPin;
};

#endif
