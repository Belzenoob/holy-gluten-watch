idf_component_register(
  SRCS madre.cpp brain.cpp face.cpp companion.cpp pet.cpp stage.cpp sounds.cpp voices.cpp audio.cpp hw.cpp link.cpp diag.cpp store.cpp ui.cpp font_medium_14.c
  INCLUDE_DIRS .
  REQUIRES board esp_pm esp_driver_usb_serial_jtag bt esp_partition)
# Shared with the Arduino build, which is lenient about printf formats
# (uint32_t is a long here).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-format)
