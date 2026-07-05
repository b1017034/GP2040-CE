#include "ADS8332.h"
#include "pico/time.h"

void ADS8332::init() {
    // Wake the converter and program the CFR
    frame(ADS8332_CMD_WAKEUP);
    busy_wait_us(10);
    frame(ADS8332_CMD_WRITE_CFR | ADS8332_CFR_VALUE);
    busy_wait_us(10);
    // Select channel 0 so auto-trigger conversions begin
    frame(ADS8332_CMD_SELECT_CH(0));
    busy_wait_us(10);
}

uint16_t ADS8332::frame(uint16_t tx) {
    spi->beginTransaction(ADS8332_SPI_SPEED_MHZ, spi_order_t::SPI_MSB_FIRST, ADS8332_SPI_MODE);
    spi->select(csPin);
    uint16_t rx = spi->transfer16(tx);
    spi->deselect();
    spi->endTransaction();
    return rx;
}

uint16_t ADS8332::readCFR() {
    uint16_t v = frame(ADS8332_CMD_READ_CFR);
    return v & 0x0FFF;
}

uint16_t ADS8332::readChannel(uint8_t ch) {
    frame(ADS8332_CMD_SELECT_CH(ch));
    // Auto-trigger at 500kSPS: a full acquisition + conversion of the newly
    // selected channel completes within ~4us; wait two cycles for safety.
    busy_wait_us(8);
    return frame(ADS8332_CMD_READ_DATA);
}

void ADS8332::readAll(uint16_t values[ADS8332_CHANNEL_COUNT]) {
    for (uint8_t ch = 0; ch < ADS8332_CHANNEL_COUNT; ch++) {
        values[ch] = readChannel(ch);
    }
}
