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
    spi->beginTransaction(ADS8332_SPI_SPEED_HZ, spi_order_t::SPI_MSB_FIRST, ADS8332_SPI_MODE);
    // PeripheralSPI hardcodes 8-bit frames; the ADS8332 command word is 16 bits
    spi_cpol_t cpol = (ADS8332_SPI_MODE == SPIMode::SPI_MODE2 || ADS8332_SPI_MODE == SPIMode::SPI_MODE3) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (ADS8332_SPI_MODE == SPIMode::SPI_MODE1 || ADS8332_SPI_MODE == SPIMode::SPI_MODE3) ? SPI_CPHA_1 : SPI_CPHA_0;
    spi_set_format(spi->getController(), 16, cpol, cpha, SPI_MSB_FIRST);
    spi->select(csPin);
    busy_wait_us(2); // pull-driven CS needs settle time
    uint16_t rx;
    spi_write16_read16_blocking(spi->getController(), &tx, &rx, 1);
    busy_wait_us(2);
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
