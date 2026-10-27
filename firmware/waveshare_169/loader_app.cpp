#include "apps.h"
#if APP_LOADER

#include "loader_app.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/queue.h>

#include <atomic>

#include "installer.h"
#include "posix_storage.h"
#include "softkey.h"

using namespace fm1_loader;

namespace {

constexpr const char *kStoreMount = "/store";
constexpr const char *kBlobKey = "loader/wl82loader.bin";
constexpr uint32_t kScsiTimeoutMs = 5000;
constexpr uint32_t kFm1AppearMs = 10000;  // the FM-1 re-enumerating after a host restart

// The unit recovered on 2026-10-25 (backups/fm1-transporter-dump-20261025-103326.bin
// in the recovery project). Checkpoint 1 passes when a backup of the same unit
// matches these.
constexpr const char *kDumpUnit = "69bb070aaa87bd10";
constexpr const char *kDumpHead = "d9f43191777656de01c7bed4f9e9ba2e34e94832c5d2ed02c3fc7ab7a6d9bd67";
constexpr const char *kDumpFactory = "70a8f5bea5097f4a0046682cdb5bf2e4ad6962901bd42debfe30c95a5ef3f3b0";

EspUsbHost *usb = nullptr;
bool (*loader_owns_midi)() = nullptr;

// Devices, written on the USB client task.
std::atomic<uint8_t> fm1_midi_address{0};
std::atomic<uint8_t> rom_address{0};

// Whole SysEx messages from the FM-1, for the identity handshake.
struct SysexMsg {
  uint8_t len;
  uint8_t data[63];
};
QueueHandle_t sysex_queue = nullptr;
SysexAssembler assembler;  // USB client task only

// What loop() shows, under lock.
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
UiLoader shown = {};
std::atomic<platform::Mode> wanted{platform::Mode::Play};
std::atomic<platform::Mode> running{platform::Mode::Play};
std::atomic<bool> busy{false};
TaskHandle_t task = nullptr;

// The store and the blob, set up once in loader_begin().
PosixStorage *storage = nullptr;
Library *library = nullptr;
std::vector<uint8_t> blob;
bool store_ok = false, blob_ok = false;
char store_line[40] = "", blob_line[40] = "";

void show(const char *title, uint32_t colour, uint8_t percent = 0) {
  taskENTER_CRITICAL(&lock);
  strlcpy(shown.title, title, sizeof shown.title);
  shown.title_colour = colour;
  shown.percent = percent;
  taskEXIT_CRITICAL(&lock);
}

void show_line(int i, const char *text) {
  taskENTER_CRITICAL(&lock);
  strlcpy(shown.lines[i], text, sizeof shown.lines[i]);
  taskEXIT_CRITICAL(&lock);
}

constexpr uint32_t kWhite = 0xF2F2F2, kYellow = 0xFFE14A, kGreen = 0x22DD44, kRed = 0xFF4A4A;

bool wait_until(uint32_t timeout_ms, bool (*ready)()) {
  const uint32_t start = millis();
  while (!ready()) {
    if (millis() - start > timeout_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

// ---------- The FM-1 as fm1_loader sees it ----------

class UsbScsi : public ScsiTransport {
 public:
  bool execute(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) override {
    const uint8_t address = rom_address.load();
    if (!address) return false;
    size_t actual = 0;
    if (!usb->mscRawCommand(address, cdb, uint8_t(cdb_len), dir == Dir::None ? nullptr : data,
                            dir == Dir::None ? 0 : len, dir != Dir::Out, actual, kScsiTimeoutMs))
      return false;
    // jl-uboot-tool's SG_IO ignores a short answer to a 16-byte response or the
    // inquiry (the rest stays zero); flash reads must arrive whole.
    return dir != Dir::In || actual == len || len <= 36;
  }
};

class UsbMidiLink : public MidiLink {
 public:
  bool send(const uint8_t *packets, size_t len) override {
    const uint8_t address = fm1_midi_address.load();
    return address && usb->midiSend(packets, len, address);
  }
  bool receive_sysex(std::vector<uint8_t> &msg, uint32_t timeout_ms) override {
    SysexMsg m;
    if (xQueueReceive(sysex_queue, &m, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    msg.assign(m.data, m.data + m.len);
    return true;
  }
  void sleep_ms(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
  void drain() override { xQueueReset(sysex_queue); }
};

class Host : public LoaderHost {
 public:
  std::string identity;
  KeyResult key = KeyResult::NoAnswer;
  const char *why = "";  // which check stopped the key, for the screen
  bool send_rom_boot_key() override {
    show("Asking the FM-1...", kYellow);
    // EspUsbHost reports a device as connected before its interfaces are
    // claimed: wait until its MIDI endpoints are up and the broker gives the
    // Loader the FM-1's MIDI (only in Load mode).
    if (!wait_until(kFm1AppearMs, [] { return fm1_midi_address.load() != 0; })) {
      key = KeyResult::NoAnswer;
      why = "no FM-1 on the hub";
      return false;
    }
    if (!wait_until(kFm1AppearMs, [] {
          const uint8_t a = fm1_midi_address.load();
          return a != 0 && usb->midiReady(a);
        })) {
      key = KeyResult::SendFailed;
      why = "FM-1 MIDI never ready";
      return false;
    }
    if (!wait_until(2000, [] { return loader_owns_midi && loader_owns_midi(); })) {
      key = KeyResult::SendFailed;
      why = "broker: Loader doesn't own FM-1 MIDI";
      return false;
    }
    UsbMidiLink link;
    key = enter_rom_boot(link, identity);
    if (key == KeyResult::SendFailed) why = "identity query not sent";
    return key == KeyResult::Sent;
  }
  bool wait_for_rom_device(uint32_t timeout_ms) override {
    show("Waiting for ROM boot...", kYellow);
    return wait_until(timeout_ms, [] {
      const uint8_t a = rom_address.load();
      return a != 0 && usb->mscReady(a);
    });
  }
  bool wait_for_midi_return(uint32_t timeout_ms) override {
    show("Restarting the FM-1...", kYellow);
    return wait_until(timeout_ms, [] { return fm1_midi_address.load() != 0; });
  }
};

// ---------- The job ----------

const char *step_text(Step s) {
  switch (s) {
    case Step::Confirm: case Step::SendKey: return "Asking the FM-1...";
    case Step::WaitRom: return "Waiting for ROM boot...";
    case Step::Identify: case Step::UploadLoader: case Step::ReadIds: return "Identifying...";
    case Step::Backup: return "Backing up (2 reads)...";
    case Step::Reset: case Step::WaitMidi: return "Restarting the FM-1...";
    default: return "Working...";
  }
}

void report_backup(Installer &inst, const Host &host) {
  const std::string &id = inst.unit().id;
  char buf[40];
  snprintf(buf, sizeof buf, "Unit %s%s", id.c_str(), id == kDumpUnit ? " (dump's)" : "");
  show_line(2, buf);
  snprintf(buf, sizeof buf, "Running %s", host.identity.empty() ? "?" : host.identity.c_str());
  show_line(3, buf);
  Hash head, factory;
  const bool have = library->region_sha256(inst.backup_id(), 0, kHeadEnd, head) &&
                    library->region_sha256(inst.backup_id(), kUnitSector, kFactoryEnd, factory);
  if (!have) {
    show_line(4, "Region hashes unavailable");
    return;
  }
  snprintf(buf, sizeof buf, "Head %.8s %s", hex(head).c_str(), hex(head) == kDumpHead ? "= dump" : "differs");
  show_line(4, buf);
  snprintf(buf, sizeof buf, "BT+voices %.8s %s", hex(factory).c_str(),
           hex(factory) == kDumpFactory ? "= dump" : "differs");
  show_line(5, buf);
}

void run_backup() {
  UsbScsi scsi;
  Host host;
  Installer inst(scsi, host, *library, blob);
  InstallOptions o;
  o.confirmed = true;  // the 2-second hold on the Firmware page
  o.rom_timeout_ms = kRomWaitMs;
  inst.start_backup(o);
  while (inst.step()) {
    uint8_t percent = inst.sectors_total() ? uint8_t(inst.sectors_done() * 100 / inst.sectors_total()) : 0;
    show(step_text(inst.current()), kYellow, percent);
  }
  if (inst.current() == Step::Done) {
    show("Backed up. Nothing written.", kGreen, 100);
    report_backup(inst, host);
    return;
  }
  char buf[40];
  if (inst.failure() == Failure::KeyNotSent) snprintf(buf, sizeof buf, "No ROM boot: %s", key_result_text(host.key));
  else snprintf(buf, sizeof buf, "Stopped: %s", failure_text(inst.failure()));
  show(buf, kRed);
  if (inst.failure() == Failure::KeyNotSent && host.why[0]) show_line(2, host.why);
  if (!host.identity.empty()) {
    snprintf(buf, sizeof buf, "FM-1 answered %s", host.identity.c_str());
    show_line(3, buf);
  }
  if (inst.danger()) show_line(5, "FM-1 left in ROM boot: power-cycle it");
}

// The store line: free space and units, refreshed after each job.
void refresh_store_line() {
  if (!store_ok) return;
  snprintf(store_line, sizeof store_line, "Store %.1f of %.1f MB free, %u units",
           (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1048576.0, LittleFS.totalBytes() / 1048576.0,
           unsigned(library->unit_ids().size()));
  show_line(0, store_line);
}

void loader_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    busy = true;
    for (int i = 2; i < kLoaderLines; ++i) show_line(i, "");
    show("Switching to Load mode...", kYellow);
    wanted = platform::Mode::Load;
    // loop() restarts the USB host in Load mode, then reports back.
    wait_until(5000, [] { return running.load() == platform::Mode::Load; });
    if (running.load() != platform::Mode::Load) show("Couldn't enter Load mode", kRed);
    else run_backup();
    wanted = platform::Mode::Play;
    wait_until(5000, [] { return running.load() == platform::Mode::Play; });
    refresh_store_line();
    busy = false;
  }
}

// The store's first check: mounted, rename-over works, and how full it is.
void open_store() {
  if (!LittleFS.begin(false, kStoreMount, 5, "store")) {  // never format: it holds backups
    strlcpy(store_line, "Store unreadable: make flash-store", sizeof store_line);
    return;
  }
  storage = new PosixStorage(kStoreMount);
  library = new Library(*storage);
  const uint8_t one[] = {1}, two[] = {2, 2};
  std::vector<uint8_t> back;
  store_ok = storage->write("t/selftest", one, 1) && storage->write("t/selftest", two, 2) &&
             storage->read("t/selftest", back) && back.size() == 2 && storage->remove("t/selftest");
  if (!store_ok) {
    strlcpy(store_line, "Store self-test failed", sizeof store_line);
    return;
  }
  refresh_store_line();
  blob_ok = storage->read(kBlobKey, blob) && hex(sha256(blob.data(), blob.size())) == kLoaderSha256;
  strlcpy(blob_line, blob_ok ? "Loader blob ok" : blob.empty() ? "Loader blob missing" : "Loader blob damaged",
          sizeof blob_line);
}

}  // namespace

void loader_begin(EspUsbHost &host, bool (*owns_fm1_midi)()) {
  usb = &host;
  loader_owns_midi = owns_fm1_midi;
  sysex_queue = xQueueCreate(8, sizeof(SysexMsg));
  open_store();
  show_line(0, store_line);
  show_line(1, blob_line);
  show(store_ok && blob_ok ? "Ready. Read-only build." : "Not ready", store_ok && blob_ok ? kWhite : kRed);
  xTaskCreatePinnedToCore(loader_task, "loader", 12288, nullptr, 2, &task, 1);
}

void loader_device_connected(const EspUsbHostDeviceInfo &d) {
  if (d.vid == 0x4C4A && d.pid == 0x8057) rom_address = d.address;
  else if ((d.vid == 0x4C4A && d.pid == 0xC755) || (d.vid == 0x1209 && d.pid == 0x0001))
    fm1_midi_address = d.address;
}

void loader_device_disconnected(const EspUsbHostDeviceInfo &d) {
  uint8_t a = d.address;
  rom_address.compare_exchange_strong(a, 0);
  a = d.address;
  fm1_midi_address.compare_exchange_strong(a, 0);
}

void loader_midi(const EspUsbHostMidiMessage &m) {
  if (m.address != fm1_midi_address.load() || !sysex_queue) return;
  std::vector<uint8_t> msg;
  if (!assembler.feed(m.codeIndex, m.status, m.data1, m.data2, msg) || msg.size() > sizeof SysexMsg::data) return;
  SysexMsg out;
  out.len = uint8_t(msg.size());
  memcpy(out.data, msg.data(), msg.size());
  xQueueSend(sysex_queue, &out, 0);
}

void loader_request_backup() {
  if (busy || !store_ok || !blob_ok || !task) return;
  busy = true;
  xTaskNotifyGive(task);
}

platform::Mode loader_wanted_mode() { return wanted.load(); }

void loader_mode_changed(platform::Mode mode) { running = mode; }

void loader_ui(UiLoader &out) {
  taskENTER_CRITICAL(&lock);
  out = shown;
  taskEXIT_CRITICAL(&lock);
  out.busy = busy;
  out.can_back_up = store_ok && blob_ok;
}

#endif  // APP_LOADER
