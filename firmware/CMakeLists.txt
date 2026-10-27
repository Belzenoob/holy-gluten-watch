# MADRE watch firmware, ESP-IDF build: the same code as firmware/madre-c6
# (Arduino), with the Arduino core as a component so the project can turn
# on power management (automatic light sleep, BLE sleep).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Components the Arduino core pulls in (Modbus, Zigbee, …) trip warnings of
# the newer compiler; they are not ours to fix and we do not use them.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
project(madre)
