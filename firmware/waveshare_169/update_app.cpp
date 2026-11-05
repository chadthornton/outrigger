#include "update_app.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "apps.h"
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
    snprintf(path, sizeof path, "%s/%s", kStoreMount, name);
    remove(path);
  }
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
  static char sha_hex[17] = "";
  if (!sha_hex[0]) {
    const esp_partition_t *recovery =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    uint8_t sha[32];
    char out[17] = "none";
    if (recovery && esp_partition_get_sha256(recovery, sha) == ESP_OK)
      for (int i = 0; i < 8; ++i) snprintf(out + 2 * i, 3, "%02x", sha[i]);
    memcpy(sha_hex, out, sizeof sha_hex);
  }
  return sha_hex;
}

const char *update_recovery_log() { return recovery_log; }
