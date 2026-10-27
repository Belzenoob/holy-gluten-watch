# Waveshare's Arduino board libraries (display, touch, sensors, power
# manager), built against the Arduino core component. fetch-board-libs.sh
# downloads them into firmware/vendor (not in git).
set(LIB ${CMAKE_CURRENT_LIST_DIR}/../../vendor/waveshare/examples/arduino/libraries)
if(NOT EXISTS ${LIB})
  message(FATAL_ERROR "Board libraries missing: run firmware/fetch-board-libs.sh first")
endif()
idf_component_register(
  SRCS
    ${LIB}/Arduino_GFX/src/Arduino_DataBus.cpp
    ${LIB}/Arduino_GFX/src/Arduino_G.cpp
    ${LIB}/Arduino_GFX/src/Arduino_GFX.cpp
    ${LIB}/Arduino_GFX/src/Arduino_OLED.cpp
    ${LIB}/Arduino_GFX/src/Arduino_TFT.cpp
    ${LIB}/Arduino_GFX/src/databus/Arduino_ESP32QSPI.cpp
    ${LIB}/Arduino_GFX/src/display/Arduino_CO5300.cpp
    ${LIB}/Arduino_DriveBus/src/Arduino_DriveBus.cpp
    ${LIB}/Arduino_DriveBus/src/Arduino_IIC.cpp
    ${LIB}/Arduino_DriveBus/src/Arduino_IIC_Chip.cpp
    ${LIB}/Arduino_DriveBus/src/hardware/Arduino_HWIIC.cpp
    ${LIB}/Arduino_DriveBus/src/touch_chip/Arduino_FT3x68.cpp
    ${LIB}/SensorLib/src/platform/SensorCommDebug.cpp
    ${LIB}/SensorLib/src/platform/SensorCommStatic.cpp
    ${LIB}/XPowersLib/src/XPowersLibInterface.cpp
  INCLUDE_DIRS
    ${LIB}/Arduino_GFX/src
    ${LIB}/Arduino_DriveBus/src
    ${LIB}/SensorLib/src
    ${LIB}/SensorLib/src/REG
    ${LIB}/SensorLib/src/platform
    ${LIB}/XPowersLib/src
    ${LIB}/Mylibrary)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
