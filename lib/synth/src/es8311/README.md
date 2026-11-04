# ES8311 codec driver

Espressif's `es8311` component (Apache-2.0, SPDX header in each file), as Waveshare ships it for the
ESP32-S3-Touch-LCD-3.5 with its register access moved onto Arduino's `Wire`:
`waveshareteam/ESP32-S3-Touch-LCD-3.5`, `examples/arduino/libraries/es8311/` (fetched 2026-11-03).

Outrigger's changes, each marked `Outrigger:` in the source:
- `es8311.h` includes `hal/i2c_types.h` for `i2c_port_t` instead of the legacy `driver/i2c.h`, which
  Wire's I2C driver conflicts with.
- `es8311.cpp` includes `freertos/FreeRTOS.h` for `vTaskDelay` instead of `driver/gpio.h`.
