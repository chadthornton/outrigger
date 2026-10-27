#include "apps.h"
#include "crumbs.h"
#if APP_LOADER

#include "loader_app.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>

#include "installer.h"
#include "midi_core.h"
#include "posix_storage.h"
#include "softkey.h"

using namespace fm1_loader;

extern char last_crash[3][40];  // app.cpp: the last abnormal reset, if any

#if LOADER_DIAG
#define DIAG(...) printf(__VA_ARGS__)
#else
#define DIAG(...) do {} while (0)
#endif

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
LoaderCore core = {};
std::atomic<bool> restart_requested{false};

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

template <class Ready>
bool wait_until(uint32_t timeout_ms, Ready ready) {
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
    // A flash read (FD05) must arrive whole, whatever its size.
    if (dir != Dir::In || actual == len) return true;
    return cdb[0] != (kReadFlash >> 8) || cdb[1] != (kReadFlash & 0xFF) ? len <= 36 : false;
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
    if (rom_address.load()) {  // left there by an earlier job or the FM-1's own hotkey
      key = KeyResult::SendFailed;
      why = "FM-1 already in ROM boot: power-cycle it";
      return false;
    }
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
    if (!wait_until(2000, [] { return core.owns_fm1_midi && core.owns_fm1_midi(); })) {
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
    auto ready = [] {
      const uint8_t a = rom_address.load();
      return a != 0 && usb->mscReady(a);
    };
    const uint32_t start = millis();
    bool restarted = false;
    while (!ready()) {
      if (millis() - start > timeout_ms) {
        const uint8_t a = rom_address.load();
        if (!a) why = "ROM device never appeared";
        else describe_refusal(a, "ROM device unusable");
        return false;
      }
      // It appeared but wasn't claimed (checkpoint 1, second run): the broker
      // may still count the FM-1 that just left. Restart the host once so
      // everything enumerates afresh; the ROM stays in ROM boot until a power cycle.
      const uint8_t a = rom_address.load();
      if (a && !restarted && millis() - start > 3000) {
        describe_refusal(a, "restarted host; was");
        restarted = true;
        show("Re-enumerating...", kYellow);
        restart_requested = true;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
  }
  void describe_refusal(uint8_t address, const char *prefix) {
    char reason[34] = "";
    if (core.refusal) core.refusal(address, reason, sizeof reason);
    snprintf(why_buf, sizeof why_buf, "%s: %s", prefix, reason[0] ? reason : "not claimed");
    why = why_buf;
  }
  char why_buf[64] = "";
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
    case Step::ResumeRead: return "Reading the FM-1...";
    case Step::Plan: return "Planning...";
    case Step::WriteApp: return "Writing firmware...";
    case Step::WriteData: return "Writing data...";
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

// The list the page shows (loader task only): package ids in row order.
std::vector<std::string> item_ids;
std::atomic<int> selected{-1}, pin_request{-1};
// What the selected install was previewed as: the hold agrees to exactly that.
DataDecision previewed;
// An install stopped with the app area incomplete: the hold retries it (resume).
bool resume_ready = false;
bool loader_left_running = false;

constexpr uint32_t kAct = 1, kSelect = 2, kPin = 4;

std::string label_of(const PackageInfo &p) {
  std::string name = p.name.empty() ? p.id : p.name;
  return p.version.empty() ? name : name + " " + p.version;
}

void refresh_items() {
  std::vector<PackageInfo> pkgs;
  for (const std::string &id : library->package_ids()) {
    PackageInfo p;
    if (library->package(id, p)) pkgs.push_back(p);
  }
  std::sort(pkgs.begin(), pkgs.end(), [](const PackageInfo &a, const PackageInfo &b) {
    return a.pinned != b.pinned ? a.pinned : label_of(a) < label_of(b);
  });
  // What the FM-1 seen last runs, from its latest backup.
  Firmware running;
  Unit u;
  FlashHashes flash;
  if (library->unit(library->last_unit(), u) && library->backup(u.latest_backup, flash))
    running = library->identify_app(std::vector<Hash>(flash.begin() + kAppStart / kSector,
                                                      flash.begin() + kAppEnd / kSector));
  // Built here, then copied under the lock: nothing inside a critical section
  // may allocate (ESP-IDF aborts, and the board reboots in a loop).
  UiLoaderItem items[kLoaderItems] = {};
  uint8_t count = 0;
  item_ids.clear();
  for (const PackageInfo &p : pkgs) {
    if (count == kLoaderItems) break;
    UiLoaderItem &it = items[count++];
    strlcpy(it.label, label_of(p).c_str(), sizeof it.label);
    strlcpy(it.note, running.known() && running.app_sha256 == p.app_sha256 ? "running" : p.identity.c_str(),
            sizeof it.note);
    it.pinned = p.pinned;
    item_ids.push_back(p.id);
  }
  taskENTER_CRITICAL(&lock);
  memcpy(shown.items, items, sizeof items);
  shown.item_count = count;
  taskEXIT_CRITICAL(&lock);
}

void set_preview(const char *l0, const char *l1, const char *action, bool can) {
  taskENTER_CRITICAL(&lock);
  strlcpy(shown.preview[0], l0, sizeof shown.preview[0]);
  strlcpy(shown.preview[1], l1, sizeof shown.preview[1]);
  strlcpy(shown.action, action, sizeof shown.action);
  shown.can_act = can;
  shown.selected = int8_t(selected.load());
  taskEXIT_CRITICAL(&lock);
}

void refresh_preview() {
  const int sel = selected.load();
  const bool ready = store_ok && blob_ok;
  if (resume_ready) {
    set_preview("An install stopped part-way.", "The FM-1 must still be in ROM boot.", "Hold: retry the install",
                ready && kWritesEnabled);
    return;
  }
  if (sel < 0 || sel >= int(item_ids.size())) {
    set_preview("Reads the FM-1 twice.", "Writes nothing.", "Hold: back up FM-1", ready);
    return;
  }
  const InstallPreview p = preview_install(*library, library->last_unit(), item_ids[sel]);
  previewed = p.decision;
  char l0[40], l1[40], action[40];
  PackageInfo pkg;
  library->package(item_ids[sel], pkg);
  snprintf(action, sizeof action, "Hold: install %s", label_of(pkg).c_str());
  if (!p.ok) {
    set_preview(p.problem, "", action, false);
    return;
  }
  if (p.app_sectors == 0) snprintf(l0, sizeof l0, "Already installed: 0 sectors");
  else snprintf(l0, sizeof l0, "Writes %u of 143 app sectors", unsigned(p.app_sectors));
  switch (p.decision.action) {
    case DataAction::Keep:
      strlcpy(l1, p.decision.stock_unmapped ? "Keeps old data: may confuse stock" : "Keeps songs and settings",
              sizeof l1);
      break;
    case DataAction::Restore:
      snprintf(l1, sizeof l1, "Restores its saves (%u sectors)", unsigned(p.data_sectors));
      break;
    case DataAction::FreshStart:
      snprintf(l1, sizeof l1, "Fresh start: erases %u (saved)", unsigned(p.data_sectors));
      break;
  }
  if (!kWritesEnabled) strlcpy(action, "Read-only build", sizeof action);
  set_preview(l0, l1, action, ready && kWritesEnabled);
}

void finish(Installer &inst, const Host &host, const char *done_title) {
  if (inst.current() == Step::Done) {
    show(done_title, kGreen, 100);
    if (host.why[0]) show_line(1, host.why);  // a re-enumeration was needed: worth seeing
    return;
  }
  char buf[40];
  if (inst.failure() == Failure::KeyNotSent) snprintf(buf, sizeof buf, "No ROM boot: %s", key_result_text(host.key));
  else snprintf(buf, sizeof buf, "Stopped: %s", failure_text(inst.failure()));
  show(buf, kRed);
  if (host.why[0]) show_line(2, host.why);
  if (!host.identity.empty()) {
    snprintf(buf, sizeof buf, "FM-1 answered %s", host.identity.c_str());
    show_line(3, buf);
  }
  if (inst.danger()) {
    show_line(4, "DON'T UNPLUG the FM-1 or switch it off");
    show_line(5, "Hold to retry; Transporter if that fails");
  }
}

void run(Installer &inst) {
  while (inst.step()) {
    uint8_t percent = inst.sectors_total() ? uint8_t(inst.sectors_done() * 100 / inst.sectors_total()) : 0;
    show(step_text(inst.current()), kYellow, percent);
  }
}

void run_job() {
  UsbScsi scsi;
  Host host;
  Installer inst(scsi, host, *library, blob);
  InstallOptions o;
  o.confirmed = true;  // the 2-second hold on the Firmware page
  o.rom_timeout_ms = kRomWaitMs;
  const int sel = selected.load();
  if (resume_ready) {
    // The FM-1 is still in ROM boot with an incomplete app: no key, straight to it.
    if (!host.wait_for_rom_device(kRomWaitMs)) {
      show("Stopped: the FM-1 isn't in ROM boot", kRed);
      if (host.why[0]) show_line(2, host.why);
      return;
    }
    o.loader_running = loader_left_running;
    inst.start_resume(o);
    run(inst);
    finish(inst, host, "Install finished.");
  } else if (sel < 0 || sel >= int(item_ids.size())) {
    inst.start_backup(o);
    run(inst);
    finish(inst, host, "Backed up. Nothing written.");
    if (inst.current() == Step::Done) report_backup(inst, host);
  } else {
    // The hold agreed to what the preview showed, the data decision included.
    o.fresh_start_ok = previewed.action == DataAction::FreshStart;
    o.layout_change_ok = previewed.layout_warning;
    inst.start_install(item_ids[sel], o);
    run(inst);
    char buf[40];
    snprintf(buf, sizeof buf, "Installed: %u sectors written", unsigned(inst.sectors_done()));
    finish(inst, host, buf);
  }
  resume_ready = inst.danger();
  // Stopped part-way without a reset: the loader still runs in the FM-1's RAM.
  loader_left_running = inst.danger();
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
  CRUMB_LOADER("start");
  if (store_ok) {
    DIAG("diag: refresh_items\n");
    CRUMB_LOADER("refresh_items");
    refresh_items();
    CRUMB_LOADER("refresh_preview");
    DIAG("diag: refresh_preview (%u items)\n", unsigned(item_ids.size()));
    refresh_preview();
    DIAG("diag: loader task idle, stack left %u\n", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
  for (;;) {
    uint32_t bits = 0;
    CRUMB_LOADER("idle");
    xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
    CRUMB_LOADER("woken");
    if (!store_ok) continue;
    if (bits & kPin) {
      const int i = pin_request.exchange(-1);
      if (i >= 0 && i < int(item_ids.size())) {
        PackageInfo p;
        if (library->package(item_ids[i], p)) library->set_pinned(p.id, !p.pinned);
        refresh_items();
      }
    }
    if (bits & (kSelect | kPin)) refresh_preview();
    if (!(bits & kAct)) continue;
    busy = true;
    for (int i = 1; i < kLoaderLines; ++i) show_line(i, i == 1 ? blob_line : "");
    show("Switching to Load mode...", kYellow);
    wanted = platform::Mode::Load;
    // loop() restarts the USB host in Load mode, then reports back.
    wait_until(5000, [] { return running.load() == platform::Mode::Load; });
    if (running.load() != platform::Mode::Load) show("Couldn't enter Load mode", kRed);
    else run_job();
    // After an install stopped part-way, stay in Load mode: the ROM device stays
    // claimed and the loader in its RAM isn't disturbed by a re-enumeration.
    const platform::Mode next = resume_ready ? platform::Mode::Load : platform::Mode::Play;
    wanted = next;
    wait_until(5000, [next] { return running.load() == next; });
    refresh_store_line();
    refresh_items();
    refresh_preview();
    busy = false;
  }
}

// The store's first check: mounted, rename-over works, and how full it is.
void open_store() {
  DIAG("diag: mounting the store\n");
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
  DIAG("diag: store self-test ok\n");
  refresh_store_line();
  DIAG("diag: store line: %s\n", store_line);
  blob_ok = storage->read(kBlobKey, blob) && hex(sha256(blob.data(), blob.size())) == kLoaderSha256;
  strlcpy(blob_line, blob_ok ? "Loader blob ok" : blob.empty() ? "Loader blob missing" : "Loader blob damaged",
          sizeof blob_line);
}

}  // namespace

void loader_begin(EspUsbHost &host, const LoaderCore &c) {
  usb = &host;
  core = c;
  sysex_queue = xQueueCreate(8, sizeof(SysexMsg));
  shown.selected = -1;
  open_store();
  show_line(0, store_line);
  show_line(1, blob_line);
  const bool ready = store_ok && blob_ok;
  show(!ready ? "Not ready" : kWritesEnabled ? "Ready. Writes are on." : "Ready. Read-only build.",
       !ready ? kRed : kWritesEnabled ? kYellow : kWhite);
  if (last_crash[0][0]) {  // the board reset abnormally: say where
    for (int i = 0; i < 3; ++i) show_line(2 + i, last_crash[i]);
  }
  char mem[40];
  snprintf(mem, sizeof mem, "Free RAM %u KB internal, %u KB PSRAM",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  show_line(5, mem);
  xTaskCreatePinnedToCore(loader_task, "loader", 12288, nullptr, 2, &task, 1);
}

void loader_device_connected(const EspUsbHostDeviceInfo &d) {
  platform::DeviceDesc desc = {};
  desc.vid = d.vid;
  desc.pid = d.pid;
  if (platform::classify(desc) == platform::Kind::Fm1RomBoot) rom_address = d.address;
  else if (midi_core::device_role(d.vid, d.pid) == midi_core::Role::Target) fm1_midi_address = d.address;
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

void loader_select(int index) {
  if (busy || !task) return;
  selected = index;
  xTaskNotify(task, kSelect, eSetBits);
}

void loader_pin(int index) {
  if (busy || !task) return;
  pin_request = index;
  xTaskNotify(task, kPin, eSetBits);
}

void loader_act() {
  if (busy || !store_ok || !blob_ok || !task) return;
  taskENTER_CRITICAL(&lock);
  const bool can = shown.can_act;
  taskEXIT_CRITICAL(&lock);
  if (!can) return;
  busy = true;
  xTaskNotify(task, kAct, eSetBits);
}

platform::Mode loader_wanted_mode() { return wanted.load(); }

bool loader_wants_restart() { return restart_requested.exchange(false); }

void loader_mode_changed(platform::Mode mode) { running = mode; }

void loader_ui(UiLoader &out) {
  taskENTER_CRITICAL(&lock);
  out = shown;
  taskEXIT_CRITICAL(&lock);
  out.busy = busy;
}

#endif  // APP_LOADER
