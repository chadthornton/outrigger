#include "update_app.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include "apps.h"
#include "board_store.h"
#include "host_ui.h"
#include "diag.h"
#include "ota.h"
#include "ota_nvs.h"
#if APP_LOADER
#include "loader_app.h"
#endif

// Arduino confirms a PENDING_VERIFY image at startup unless this says it will
// do so itself (esp32-hal-misc.c). Outrigger confirms in update_loop, once it
// has run, so an image that crashes or hangs first goes back to the recovery app.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr uint32_t kConfirmAfterMs = 10000;
constexpr uint32_t kConfirmDeadlineMs = 120000;
constexpr const char *kStoreMount = "/store";

portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
UpdateStatus status;
char recovery_log[ota::kLogBytes + 1];
bool pending = false;  // PENDING_VERIFY, not yet confirmed
bool trial = false;    // and the record says it's an update's new image
bool confirm_failed = false;
esp_timer_handle_t deadline = nullptr;
ota::Record rec;
char recovery_sha_hex[17] = "";  // update_recovery_sha's cache; empty: not hashed yet

// An image arriving over the web task (update_receive_*). The server answers
// one request at a time, so only that task touches rx.
struct Receive {
  bool active = false, recovery = false;
  uint32_t size = 0, got = 0;
  uint8_t want[32] = {};
  char label[16] = {};
  FILE *file = nullptr;
  const esp_partition_t *slot = nullptr;
  mbedtls_sha256_context sha;
} rx;
// The checked u/next.bin, handed to the loop: start_ready is stored last.
std::atomic<bool> start_ready{false};
ota::Image next_image;
char next_label[16] = {};

const esp_partition_t *app_slot(esp_partition_subtype_t sub) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
}

void publish();

void store_path(char *out, size_t len, const char *name) { snprintf(out, len, "%s/%s", kStoreMount, name); }

bool load(ota::Record &out) {
  Preferences p;
  if (!p.begin(ota::kNvsNamespace, true)) return false;
  const bool ok = ota::nvs_load(p, out);
  const size_t n = p.getBytesLength(ota::kNvsLog);
  if (n && n < sizeof recovery_log) recovery_log[p.getBytes(ota::kNvsLog, recovery_log, n)] = 0;
  p.end();
  return ok;
}

bool save(const ota::Record &r) {
  Preferences p;
  if (!p.begin(ota::kNvsNamespace, false)) return false;
  const bool ok = ota::nvs_save(p, r);
  p.end();
  return ok;
}

bool app0_holds(const ota::Image &im) {
  const esp_partition_t *app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!app0 || im.size == 0 || im.size > app0->size) return false;
  static uint8_t buf[ota::kSector];
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  bool ok = true;
  for (uint32_t off = 0; ok && off < im.size; off += sizeof buf) {
    const size_t n = im.size - off < sizeof buf ? im.size - off : sizeof buf;
    ok = esp_partition_read(app0, off, buf, n) == ESP_OK;
    if (ok) mbedtls_sha256_update(&c, buf, n);
  }
  uint8_t got[32];
  mbedtls_sha256_finish(&c, got);
  mbedtls_sha256_free(&c);
  return ok && memcmp(got, im.sha256, 32) == 0;
}

// The store holds both images until the update settles, and after a power cut
// partway through saving them. The loader mounts the store again later;
// LittleFS.begin on a mounted store just says yes.
void remove_files() {
  if (!LittleFS.begin(false, kStoreMount, 5, "store")) return;
  for (const char *name : {ota::kNextFile, ota::kPrevFile}) {
    char path[32];
    store_path(path, sizeof path, name);
    remove(path);
  }
}

// The running image's length as built (header to appended SHA-256), 0 if it doesn't verify.
uint32_t running_image_len() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_pos_t pos = {run->address, run->size};
  esp_image_metadata_t meta;
  return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK ? meta.image_len : 0;
}

// Writes the running app0 image to u/prev.bin (temp file, fsync, rename), hashing it on the way.
bool save_running(ota::Image &out, const char **why) {
  out.size = running_image_len();
  if (!out.size) return *why = "the running image doesn't verify", false;
  char tmp[32], path[32];
  store_path(tmp, sizeof tmp, "u/prev.tmp");
  store_path(path, sizeof path, ota::kPrevFile);
  FILE *f = fopen(tmp, "wb");
  if (!f) return *why = "couldn't create u/prev.tmp", false;
  const esp_partition_t *run = esp_ota_get_running_partition();
  static uint8_t buf[ota::kSector];
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  bool ok = true;
  for (uint32_t off = 0; ok && off < out.size; off += sizeof buf) {
    const size_t n = out.size - off < sizeof buf ? out.size - off : sizeof buf;
    ok = esp_partition_read(run, off, buf, n) == ESP_OK && fwrite(buf, 1, n, f) == n;
    if (ok) mbedtls_sha256_update(&c, buf, n);
  }
  mbedtls_sha256_finish(&c, out.sha256);
  mbedtls_sha256_free(&c);
  ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(tmp, path) != 0) {
    remove(tmp);
    return *why = "couldn't save the running image to the store", false;
  }
  return true;
}

bool file_matches(const char *name, const ota::Image &im) {
  char path[32];
  store_path(path, sizeof path, name);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  static uint8_t buf[ota::kSector];
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  uint32_t total = 0;
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    mbedtls_sha256_update(&c, buf, n);
    total += n;
  }
  fclose(f);
  uint8_t got[32];
  mbedtls_sha256_finish(&c, got);
  mbedtls_sha256_free(&c);
  return total == im.size && memcmp(got, im.sha256, 32) == 0;
}

void start_failed(const char *why) {
  DLOG_LOOP("update: %s didn't start: %s", next_label, why);
  rec.state = ota::State::Idle;
  rec.result = ota::Result::Abandoned;
  strlcpy(rec.label, next_label, sizeof rec.label);
  if (!save(rec)) DLOG_LOOP("update: couldn't save the record");
  remove_files();
  publish();
}

// On the loop task, once u/next.bin has arrived and checked out.
void start_update() {
  start_ready = false;
  const char *why = "";
#if APP_LOADER
  if (loader_busy()) return start_failed("the Loader started a job");
#endif
  ota::Image prev;
  if (!save_running(prev, &why)) return start_failed(why);
  if (!file_matches(ota::kNextFile, next_image)) return start_failed("u/next.bin changed after it was checked");
  ota::Record r = ota::staged(next_image, prev, next_label);
  board_update_pins(r.pins_high, r.pins_low);
  if (!save(r)) return start_failed("couldn't save the record");
  // Validates the recovery app's image before otadata changes.
  if (esp_ota_set_boot_partition(app_slot(ESP_PARTITION_SUBTYPE_APP_OTA_1)) != ESP_OK)
    return start_failed("the recovery app doesn't validate");
  rec = r;
  publish();
  DLOG_LOOP("update: %s staged; restarting into the recovery app", next_label);
  // The recovery app can't draw: this stays on the panel, lit, until the new build starts.
  ui_show_switching("Updating Outrigger.\nDon't unplug.");
  for (int i = 0; i < 20; ++i) {
    ui_tick();
    delay(10);
  }
  delay(300);  // the HTTP answer and the log reach the Mac
  esp_restart();
}

void publish() {
  portENTER_CRITICAL(&status_lock);
  status.state = ota::state_name(rec.state);
  status.result = ota::result_name(rec.result);
  status.attempts = rec.attempts;
  strlcpy(status.label, rec.label, sizeof status.label);
  status.pending_verify = pending;
  portEXIT_CRITICAL(&status_lock);
}

// esp_timer's task, not the loop: it fires even when the loop never returns.
void on_deadline(void *) {
#if APP_LOADER
  if (loader_busy()) {  // a restart mid-install leaves the FM-1's app half-written
    esp_timer_start_once(deadline, 10 * 1000000ULL);
    return;
  }
#endif
  DLOG_LOOP("update: not confirmed in %lu s; back to the recovery app", (unsigned long)(kConfirmDeadlineMs / 1000));
  esp_restart();
}

void confirm() {
  if (!pending || confirm_failed) return;
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    // Once: the deadline timer still restarts the board, and the recovery app tries again.
    confirm_failed = true;
    DLOG_LOOP("update: the confirm failed");
    return;
  }
  if (deadline) esp_timer_stop(deadline);
  pending = false;
  if (trial) {
    rec = ota::confirmed(rec);
    if (!save(rec)) DLOG_LOOP("update: couldn't save the record");
    remove_files();
    DLOG_LOOP("update: %s confirmed on boot %u", rec.label, unsigned(rec.attempts));
  }
  publish();
}

}  // namespace

void update_boot() {
  esp_ota_img_states_t st;
  pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY;
  if (pending) {
    const esp_timer_create_args_t args = {on_deadline, nullptr, ESP_TIMER_TASK, "update", false};
    if (esp_timer_create(&args, &deadline) == ESP_OK) esp_timer_start_once(deadline, kConfirmDeadlineMs * 1000ULL);
  }
  const bool have = load(rec);
  if (!have) rec = ota::Record{};
  // Hashing app0 costs a few hundred ms, so only while an update is under way.
  const bool open = have && rec.state != ota::State::Idle;
  const ota::BootPlan plan =
      ota::plan_boot(have, rec, pending, open && app0_holds(rec.next), open && app0_holds(rec.prev));
  rec = plan.rec;
  trial = plan.trial;
  if (plan.write && !save(rec)) DLOG_LOOP("update: couldn't save the record");
  if (plan.remove_files) remove_files();
  if (have && rec.state == ota::State::Idle && rec.result != ota::Result::None)
    DLOG_LOOP("update: %s %s after %u boots", ota::result_name(rec.result), rec.label, unsigned(rec.attempts));
  if (pending) DLOG_LOOP("update: this image hasn't confirmed itself (%s)", trial ? "an update's trial" : "no record");
  publish();
}

void update_loop(uint32_t now_ms, bool healthy) {
  if (healthy && now_ms > kConfirmAfterMs) confirm();
  if (start_ready.load()) start_update();
}

void update_confirm_now(bool healthy) {
  if (healthy) confirm();
}

UpdateStatus update_status() {
  portENTER_CRITICAL(&status_lock);
  const UpdateStatus copy = status;
  portEXIT_CRITICAL(&status_lock);
  return copy;
}

const char *update_recovery_sha() {
  char *sha_hex = recovery_sha_hex;
  if (!sha_hex[0]) {
    const esp_partition_t *recovery =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    uint8_t sha[32];
    char out[17] = "none";
    if (recovery && esp_partition_get_sha256(recovery, sha) == ESP_OK)
      for (int i = 0; i < 8; ++i) snprintf(out + 2 * i, 3, "%02x", sha[i]);
    memcpy(sha_hex, out, sizeof recovery_sha_hex);
  }
  return sha_hex;
}

const char *update_recovery_log() { return recovery_log; }

bool update_receive_begin(bool recovery_slot, uint32_t size, const uint8_t sha256[32], const char *label,
                          const char **why) {
  if (rx.active || start_ready.load()) return *why = "an update is already arriving", false;
  if (pending) return *why = "this image hasn't confirmed itself yet", false;
  if (rec.state != ota::State::Idle) return *why = "an update is under way", false;
#if APP_LOADER
  if (loader_busy()) return *why = "the Loader is busy", false;
#endif
  rx = Receive{};
  rx.recovery = recovery_slot;
  rx.size = size;
  memcpy(rx.want, sha256, 32);
  strlcpy(rx.label, label, sizeof rx.label);
  if (recovery_slot) {
    rx.slot = app_slot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    if (!rx.slot || size > rx.slot->size) return *why = "no recovery slot that size", false;
    recovery_sha_hex[0] = 0;  // whatever happens next, hash it again
    const uint32_t span = (size + ota::kSector - 1) / ota::kSector * ota::kSector;
    if (esp_partition_erase_range(rx.slot, 0, span) != ESP_OK) return *why = "couldn't erase the recovery slot", false;
  } else {
    if (!strcmp(update_recovery_sha(), "none")) return *why = "no recovery app: send one first (slot=recovery)", false;
    if (!LittleFS.begin(false, kStoreMount, 5, "store")) return *why = "the store doesn't mount", false;
    remove_files();
    const uint64_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (free_bytes < uint64_t(size) + running_image_len() + 64 * 1024)
      return *why = "the store hasn't room for both images", false;
    char path[32];
    store_path(path, sizeof path, "u");
    mkdir(path, 0755);
    store_path(path, sizeof path, "u/next.tmp");
    rx.file = fopen(path, "wb");
    if (!rx.file) return *why = "couldn't create u/next.tmp", false;
  }
  mbedtls_sha256_init(&rx.sha);
  mbedtls_sha256_starts(&rx.sha, 0);
  rx.active = true;
  DLOG_LOOP("update: receiving %s, %lu bytes, for %s", rx.label, (unsigned long)size, recovery_slot ? "recovery" : "app0");
  return true;
}

bool update_receive_write(const uint8_t *data, size_t len, const char **why) {
  if (!rx.active || rx.got + len > rx.size) return *why = "more bytes than announced", false;
  // An ESP32 app image starts with 0xE9 (esp_image_header_t): anything else
  // would only be found out after the copy, three trial boots later.
  if (rx.got == 0 && len && data[0] != 0xE9) return *why = "not an ESP32 app image", false;
  const bool ok = rx.recovery ? esp_partition_write(rx.slot, rx.got, data, len) == ESP_OK
                              : fwrite(data, 1, len, rx.file) == len;
  if (!ok) return *why = rx.recovery ? "couldn't write the recovery slot" : "couldn't write the store", false;
  mbedtls_sha256_update(&rx.sha, data, len);
  rx.got += len;
  return true;
}

void update_receive_abort() {
  if (!rx.active) return;
  rx.active = false;
  mbedtls_sha256_free(&rx.sha);
  if (rx.file) {
    fclose(rx.file);
    rx.file = nullptr;
    char path[32];
    store_path(path, sizeof path, "u/next.tmp");
    remove(path);
  }
  DLOG_LOOP("update: %s abandoned after %lu of %lu bytes", rx.label, (unsigned long)rx.got, (unsigned long)rx.size);
}

bool update_receive_end(const char **why) {
  if (!rx.active || rx.got != rx.size) {
    update_receive_abort();
    return *why = "fewer bytes than announced", false;
  }
  uint8_t got[32];
  mbedtls_sha256_finish(&rx.sha, got);
  if (memcmp(got, rx.want, 32) != 0) {
    update_receive_abort();
    return *why = "the image doesn't match its sha256", false;
  }
  if (rx.recovery) {
    rx.active = false;
    mbedtls_sha256_free(&rx.sha);
    uint8_t sha[32];
    // Validates it as an app image, as the bootloader will.
    if (esp_partition_get_sha256(rx.slot, sha) != ESP_OK) return *why = "the recovery image doesn't validate", false;
    DLOG_LOOP("update: recovery app %s written", rx.label);
    return true;
  }
  bool ok = fflush(rx.file) == 0 && fsync(fileno(rx.file)) == 0;
  ok = fclose(rx.file) == 0 && ok;
  rx.file = nullptr;
  char tmp[32], path[32];
  store_path(tmp, sizeof tmp, "u/next.tmp");
  store_path(path, sizeof path, ota::kNextFile);
  if (!ok || rename(tmp, path) != 0) {
    update_receive_abort();
    remove(tmp);
    return *why = "couldn't save u/next.bin", false;
  }
  rx.active = false;
  mbedtls_sha256_free(&rx.sha);
  next_image.size = rx.size;
  memcpy(next_image.sha256, rx.want, 32);
  strlcpy(next_label, rx.label, sizeof next_label);
  start_ready.store(true);
  return true;
}
