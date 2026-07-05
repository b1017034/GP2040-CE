// ADS8332 driver for GP2040-CE (HML board)
// TI ADS8332: 16-bit, 8-channel, 500kSPS SAR ADC, SPI interface.
// Uses auto-trigger mode (CFR D9=0) so no CONVST/EOC wiring is required;
// conversions run continuously and results are fetched with the Read Data command.

#ifndef _ADS8332_H_
#define _ADS8332_H_

#include "peripheral_spi.h"

#define ADS8332_CHANNEL_COUNT 8

#ifndef ADS8332_SPI_SPEED_MHZ
#define ADS8332_SPI_SPEED_MHZ 4
#endif

#ifndef ADS8332_SPI_MODE
#define ADS8332_SPI_MODE SPIMode::SPI_MODE1
#endif

// CMR commands (upper 4 bits of the 16-bit frame)
#define ADS8332_CMD_SELECT_CH(ch) ((uint16_t)((ch) & 0x7) << 12)
#define ADS8332_CMD_WAKEUP        0xB000
#define ADS8332_CMD_READ_CFR      0xC000
#define ADS8332_CMD_READ_DATA     0xD000
#define ADS8332_CMD_WRITE_CFR     0xE000
#define ADS8332_CMD_DEFAULT_MODE  0xF000

// CFR: D11=0 manual channel select, D10=1 internal conversion clock,
// D9=0 auto-trigger (continuous conversion), D8=0 500kSPS,
// D7=0 EOC active high, D6=1 EOC mode, D5=1 EOC/INT output,
// D4=1 auto-nap disabled, D3=1 nap disabled, D2=1 deep PD disabled,
// D1=0 TAG disabled, D0=1 normal operation
#define ADS8332_CFR_VALUE 0x047D

class ADS8332 {
public:
    ADS8332(PeripheralSPI *spi, int8_t csPin) : spi(spi), csPin(csPin) {}

    void init();
    // Read a single channel (blocking, includes conversion settle time)
    uint16_t readChannel(uint8_t ch);
    // Read all 8 channels into values[]
    void readAll(uint16_t values[ADS8332_CHANNEL_COUNT]);
    // Read back CFR (lower 12 bits) for bus sanity checks
    uint16_t readCFR();

private:
    uint16_t frame(uint16_t tx);
    PeripheralSPI *spi;
    int8_t csPin;
};

#endif
