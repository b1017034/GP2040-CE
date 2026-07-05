add_library(ADS8332 ADS8332.cpp)
target_link_libraries(ADS8332 PUBLIC PicoPeripherals pico_time)
target_include_directories(ADS8332 INTERFACE .)
target_include_directories(ADS8332 PUBLIC .)
