// The recovery app: lives in the `recovery` slot (0x310000, 768 KB) and runs
// only when otadata points at it, which Outrigger does to start an update, and
// the bootloader does when an update's first boots end without a confirm. It
// copies the store's u/next.bin (or, rolling back, u/prev.bin) over app0,
// checks it, and hands back. The logic is lib/ota's run_recovery(), tested on
// the Mac with a power cut at every step (tests/test_ota.cpp).
//
// No screen library, no USB host, no Wi-Fi: Outrigger leaves "Updating..." on
// the panel, and the record names the backlight pin to keep lit. What it did
// goes to NVS ("ota"/"log"), which Outrigger's GET /crash shows.
//
// Installed once over USB (make flash-recovery), or by Outrigger itself; it
// never changes during an update.
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdio.h>

#include "ota.h"

namespace {

constexpr const char *kMount = "/store";
constexpr size_t kLogBytes = 640;  // the newest lines; NVS blob "ota"/"log"

class BoardPorts : public ota::Ports {
 public:
  bool begin() {
    app0_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    nvs_ok_ = prefs_.begin("ota", false);
    // Never formats: the store holds every FM-1 backup.
    store_ok_ = LittleFS.begin(false, kMount, 5, "store");
    char line[64];
    snprintf(line, sizeof line, "recovery: started (reset %d)%s%s", int(esp_reset_reason()),
             store_ok_ ? "" : ", store didn't mount", nvs_ok_ ? "" : ", no NVS");
    log(line);
    return app0_ != nullptr;
  }

  bool load(ota::Record &out) override {
    uint8_t blob[ota::kRecordBytes];
    if (!nvs_ok_ || prefs_.getBytesLength("rec") != sizeof blob) return false;
    return prefs_.getBytes("rec", blob, sizeof blob) == sizeof blob && ota::decode(blob, sizeof blob, out);
  }
  bool save(const ota::Record &r) override {
    uint8_t blob[ota::kRecordBytes];
    ota::encode(r, blob);
    return nvs_ok_ && prefs_.putBytes("rec", blob, sizeof blob) == sizeof blob;
  }

  int64_t file_size(const char *name) override {
    if (!open(name)) return -1;
    struct stat st;
    return fstat(fileno(file_), &st) == 0 ? int64_t(st.st_size) : -1;
  }
  bool file_read(const char *name, uint32_t offset, uint8_t *buf, size_t len) override {
    return open(name) && fseek(file_, long(offset), SEEK_SET) == 0 && fread(buf, 1, len, file_) == len;
  }

  uint32_t app0_size() override { return app0_->size; }
  bool app0_erase(uint32_t offset, uint32_t len) override {
    return esp_partition_erase_range(app0_, offset, len) == ESP_OK;
  }
  bool app0_write(uint32_t offset, const uint8_t *buf, size_t len) override {
    return esp_partition_write(app0_, offset, buf, len) == ESP_OK;
  }
  bool app0_read(uint32_t offset, uint8_t *buf, size_t len) override {
    return esp_partition_read(app0_, offset, buf, len) == ESP_OK;
  }
  bool boot_app0() override { return esp_ota_set_boot_partition(app0_) == ESP_OK; }

  void sha_begin() override {
    mbedtls_sha256_init(&sha_);
    mbedtls_sha256_starts(&sha_, 0);
  }
  void sha_update(const uint8_t *data, size_t len) override { mbedtls_sha256_update(&sha_, data, len); }
  void sha_finish(uint8_t out[32]) override {
    mbedtls_sha256_finish(&sha_, out);
    mbedtls_sha256_free(&sha_);
  }

  // Appends to the NVS log at once, so a power cut keeps what came before it.
  void log(const char *line) override {
    if (!nvs_ok_) return;
    char buf[kLogBytes + 1] = {};
    size_t n = prefs_.getBytesLength("log");
    if (n > kLogBytes) n = 0;
    if (n) prefs_.getBytes("log", buf, n);
    const size_t add = strlen(line) + 1;
    if (n + add > kLogBytes) {  // drop the oldest whole lines
      size_t cut = n + add - kLogBytes;
      while (cut < n && buf[cut - 1] != '\n') ++cut;
      memmove(buf, buf + cut, n - cut);
      n -= cut;
    }
    memcpy(buf + n, line, add - 1);
    buf[n + add - 1] = '\n';
    prefs_.putBytes("log", buf, n + add);
  }

 private:
  bool open(const char *name) {
    if (!store_ok_) return false;
    if (file_ && open_name_ == name) return true;
    if (file_) fclose(file_);
    char path[48];
    snprintf(path, sizeof path, "%s/%s", kMount, name);
    file_ = fopen(path, "rb");
    open_name_ = file_ ? name : nullptr;
    return file_ != nullptr;
  }

  Preferences prefs_;
  const esp_partition_t *app0_ = nullptr;
  bool nvs_ok_ = false, store_ok_ = false;
  FILE *file_ = nullptr;
  const char *open_name_ = nullptr;
  mbedtls_sha256_context sha_;
};

BoardPorts ports;

void drive_pins() {
  ota::Record r;
  if (!ports.load(r)) return;
  for (int8_t pin : r.pins_low)
    if (pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
    }
  for (int8_t pin : r.pins_high)
    if (pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
    }
}

}  // namespace

// Arduino marks this app valid at startup (the default verifyRollbackLater()),
// before setup(), so the bootloader never rolls back away from it.
void setup() {
  if (!ports.begin()) {
    for (;;) delay(1000);  // no app0 partition: not a board this was built for
  }
  drive_pins();
  if (ota::run_recovery(ports) == ota::Outcome::Restart) {
    ports.log("recovery: restarting into app0");
    esp_restart();
  }
  ports.log("recovery: halted");
}

void loop() { delay(1000); }
