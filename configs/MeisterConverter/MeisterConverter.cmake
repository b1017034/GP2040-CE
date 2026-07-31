# Run this board at the stock 125MHz.
#
# The tree-wide default is the SDK's "fastest supported clock", a 200MHz
# overclock. The MeisterConverter pairs its RP2040 with an XT25F32F, and at
# 200MHz with PICO_FLASH_SPI_CLKDIV 2 the XIP flash clock lands at 100MHz -
# outside what that part is specified for. The vendor's own build for this
# board runs stock, and so do we.
set(GP2040_OVERCLOCK OFF)
