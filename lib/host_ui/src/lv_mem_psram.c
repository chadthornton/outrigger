/* LVGL's allocator on the board: PSRAM first, internal RAM only if PSRAM is
 * full. Internal RAM is kept for what needs it: DMA (the display's draw
 * buffers, allocated by host_ui with MALLOC_CAP_DMA), the USB host and Wi-Fi.
 * Selected by LV_STDLIB_CUSTOM in firmware/config/lv_conf.h. */
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>

static const uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, kPsram);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size) {
  void *q = heap_caps_realloc(p, new_size, kPsram);
  return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
  mon->total_size = heap_caps_get_total_size(kPsram);
  mon->free_size = heap_caps_get_free_size(kPsram);
  mon->free_biggest_size = heap_caps_get_largest_free_block(kPsram);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
