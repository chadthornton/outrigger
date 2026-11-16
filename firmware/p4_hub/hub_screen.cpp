#include "hub_screen.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

constexpr int kMosi = 20, kSclk = 21, kCs = 23, kDc = 26, kRst = 27, kBacklight = 28;

Arduino_HWSPI bus(kDc, kCs, kSclk, kMosi, GFX_NOT_DEFINED, &SPI, false);
// Rotation 4 mirrors X without swapping axes, as Waveshare's board layer does.
Arduino_ST7796 panel(&bus, kRst, 4, true, kScreenW, kScreenH);
Arduino_Canvas canvas(kScreenW, kScreenH, &panel);
SemaphoreHandle_t lock = nullptr;
bool ready = false;
uint32_t frames = 0, flush_us = 0;

}  // namespace

bool hub_screen_begin() {
  lock = xSemaphoreCreateMutex();
  ledcAttach(kBacklight, 5000, 10);
  ledcWrite(kBacklight, 0);
  if (!canvas.begin(80000000)) return false;
  ledcWrite(kBacklight, 1023);
  ready = true;
  return true;
}

void hub_screen_draw(const char *const *lines, int count) {
  if (!ready) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  canvas.fillScreen(RGB565_BLACK);
  canvas.fillRect(0, 0, kScreenW, 34, RGB565(20, 60, 120));
  canvas.setTextColor(RGB565_WHITE);
  canvas.setTextSize(2);
  int y = 10;
  for (int i = 0; i < count; ++i) {
    if (i == 1) y = 46;
    canvas.setTextSize(i == 0 ? 2 : (i < 4 ? 2 : 1));
    canvas.setTextColor(i == 0 ? RGB565_WHITE : (i < 4 ? RGB565(200, 230, 255) : RGB565(180, 180, 180)));
    canvas.setCursor(8, y);
    canvas.print(lines[i]);
    y += i < 4 ? 24 : 13;
    if (y > kScreenH - 12) break;
  }
  const uint32_t t0 = micros();
  canvas.flush();
  flush_us = micros() - t0;
  ++frames;
  xSemaphoreGive(lock);
}

bool hub_screen_snapshot(uint16_t *out) {
  if (!ready) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(out, canvas.getFramebuffer(), kScreenW * kScreenH * 2);
  xSemaphoreGive(lock);
  return true;
}

uint32_t hub_screen_frames() { return frames; }
uint32_t hub_screen_flush_us() { return flush_us; }
