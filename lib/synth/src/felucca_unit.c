// Felucca's DSP, built as the fm1-cartridge port builds it: one translation
// unit (its adapter includes the upstream sources), signed wraparound, and
// optimized for speed rather than Arduino's default size
// (lib/synth/third_party/README.md).
#pragma GCC optimize("O2", "wrapv")

#include <esp_heap_caps.h>
#include <stdbool.h>

// Felucca's long delay and looper lines (upstream's ".pool", 174 KB) go in
// PSRAM, as the cartridge's felucca.lf puts them; the rest stays in internal
// RAM. Arduino's prebuilt ESP-IDF can't put .bss in PSRAM, so each line's
// declaration becomes a pointer to an array of the same type: `T name[N]`
// reads `T (*felucca_psram_name)[N]`, and every use, sizeof included, works
// unchanged through the macro. felucca_memory_begin() allocates them.
#define dly_buf (*felucca_psram_dly_buf)
#define cho_buf (*felucca_psram_cho_buf)
#define rev_comb (*felucca_psram_rev_comb)
#define sl_buf (*felucca_psram_sl_buf)

#include "../third_party/fm1-cartridge/synth/felucca_synth.c"

#define FELUCCA_PSRAM(p) ((p) || ((p) = heap_caps_calloc(1, sizeof *(p), MALLOC_CAP_SPIRAM)))

bool felucca_memory_begin(void) {
  return FELUCCA_PSRAM(felucca_psram_dly_buf) && FELUCCA_PSRAM(felucca_psram_cho_buf) &&
         FELUCCA_PSRAM(felucca_psram_rev_comb) && FELUCCA_PSRAM(felucca_psram_sl_buf);
}
