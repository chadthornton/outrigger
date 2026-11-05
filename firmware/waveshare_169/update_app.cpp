#include "update_app.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "diag.h"
#include "ota.h"
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

UpdateStatus status;
char recovery_log[641];
bool pending = false;  // PENDING_VERIFY, not yet confirmed
bool trial = false;    // and the record says it's an update's new image
ota::Record rec;

bool load(ota::Record &out) {
  Preferences p;
  if (!p.begin("ota", true)) return false;
  uint8_t blob[ota::kRecordBytes];
  const bool ok = p.getBytesLength("rec") == sizeof blob && p.getBytes("rec", blob, sizeof blob) == sizeof blob &&
                  ota::decode(blob, sizeof blob, out);
  const size_t n = p.getBytesLength("log");
  if (n && n < sizeof recovery_log) recovery_log[p.getBytes("log", recovery_log, n)] = 0;
  p.end();
  return ok;
}

bool save(const ota::Record &r) {
  Preferences p;
  if (!p.begin("ota", false)) return false;
  uint8_t blob[ota::kRecordBytes];
  ota::encode(r, blob);
  const bool ok = p.putBytes("rec", blob, sizeof blob) == sizeof blob;
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

// The store holds both images until the update settles. The loader mounts the
// store again later; LittleFS.begin on a mounted store just says yes.
void remove_files() {
  if (!LittleFS.begin(false, kStoreMount, 5, "store")) return;
  for (const char *name : {ota::kNextFile, ota::kPrevFile}) {
    char path[32];
    snprintf(path, sizeof path, "%s/%s", kStoreMount, name);
    remove(path);
  }
}

void publish() {
  status.state = ota::state_name(rec.state);
  status.result = ota::result_name(rec.result);
  status.attempts = rec.attempts;
  strlcpy(status.label, rec.label, sizeof status.label);
  status.pending_verify = pending;
}

}  // namespace

void update_boot() {
  esp_ota_img_states_t st;
  pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY;
  const bool have = load(rec);
  if (!have) rec = ota::Record{};
  // Hashing app0 costs a few hundred ms, so only while an update is under way.
  const bool open = have && rec.state != ota::State::Idle;
  const ota::BootPlan plan =
      ota::plan_boot(have, rec, pending, open && app0_holds(rec.next), open && app0_holds(rec.prev));
  rec = plan.rec;
  trial = plan.trial;
  if (plan.write) {
    if (!save(rec)) DLOG_LOOP("update: couldn't save the record");
    remove_files();
  }
  if (have && rec.state == ota::State::Idle && rec.result != ota::Result::None)
    DLOG_LOOP("update: %s %s after %u boots", ota::result_name(rec.result), rec.label, unsigned(rec.attempts));
  if (pending) DLOG_LOOP("update: this image hasn't confirmed itself (%s)", trial ? "an update's trial" : "no record");

  const esp_partition_t *recovery =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  uint8_t sha[32];
  if (recovery && esp_partition_get_sha256(recovery, sha) == ESP_OK)
    for (int i = 0; i < 8; ++i) snprintf(status.recovery + 2 * i, 3, "%02x", sha[i]);
  publish();
}

void update_loop(uint32_t now_ms, bool healthy) {
  if (!pending) return;
  if (healthy && now_ms > kConfirmAfterMs) {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
      DLOG_LOOP("update: the confirm failed");
      return;
    }
    pending = false;
    if (trial) {
      rec = ota::confirmed(rec);
      if (!save(rec)) DLOG_LOOP("update: couldn't save the record");
      remove_files();
      DLOG_LOOP("update: %s confirmed on boot %u", rec.label, unsigned(rec.attempts));
    }
    publish();
    return;
  }
  if (now_ms > kConfirmDeadlineMs) {
#if APP_LOADER
    if (loader_busy()) return;  // a restart mid-install leaves the FM-1's app half-written
#endif
    DLOG_LOOP("update: not healthy in %lu s; back to the recovery app", (unsigned long)(kConfirmDeadlineMs / 1000));
    esp_restart();
  }
}

const UpdateStatus &update_status() { return status; }
const char *update_recovery_log() { return recovery_log; }
