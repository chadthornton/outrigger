#include "apps.h"
#include "crumbs.h"
#include "diag.h"
#if APP_LOADER

#include "loader_app.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>

#include "installer.h"
#include "midi_core.h"
#include "posix_storage.h"
#include "softkey.h"
#if APP_WEB_UI
#include "store_json.h"
#endif

using namespace fm1_loader;

extern char last_crash[3][40];  // app.cpp: the last abnormal reset, if any

#if LOADER_DIAG
#define DIAG(...) printf(__VA_ARGS__)
#else
#define DIAG(...) do {} while (0)
#endif

// A job runs in a boot of its own. The hold records it here (RTC memory, kept
// across esp_restart) and restarts the chip; the next boot starts the USB host
// straight in Load mode and runs it, then restarts into Play. The host is never
// restarted in place: in checkpoint 3 every USB failure (an ESP-IDF assert,
// devices that never enumerated, Wi-Fi gone) came after usb.end()/begin(), and
// the first job after a fresh boot always worked.
enum class JobKind : uint8_t { None, Backup, Install, Resume };
struct JobBoot {
  uint32_t magic;
  JobKind kind;
  uint8_t attempt;  // 2: the board restarted after the key because no ROM device showed up
  bool key_sent, fresh_ok, layout_ok, loader_running;
  char package[48], unit[40];
  // The job's result, for the Firmware page after the boot back to Play.
  bool has_result;
  uint32_t result_colour;
  char result_title[40], result_lines[kLoaderLines][40];
};
constexpr uint32_t kJobMagic = 0x10B5EED1;
RTC_NOINIT_ATTR JobBoot job_boot;

#if LOADER_SOAK
// Soak builds back up the FM-1 by themselves, job after job, to make the USB
// host crash on the FM-1's disconnect after the key happen often enough to study.
// Backups write nothing; each one sends the FM-1 back to its firmware.
struct Soak {
  uint32_t magic;
  uint32_t runs, ok, failed, crashes, failed_in_row;
};
constexpr uint32_t kSoakMagic = 0x50A4C0DE;
constexpr uint32_t kSoakRuns = 50;
RTC_NOINIT_ATTR Soak soak;
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
JobBoot active_job = {};  // the job this boot runs (kind None: a Play boot)

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
platform::Mode boot_mode = platform::Mode::Play;  // fixed for the whole boot
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
  char last_failure[40] = "";  // what the USB layer said about the last failed command
  bool reached_device() const override { return reached_; }
  bool reached_ = true;
  bool execute(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) override {
    last_failure[0] = 0;  // describes this command only
    reached_ = true;
    const uint8_t address = rom_address.load();
    if (!address) {
      reached_ = false;  // gone before it was sent: it did nothing
      note(cdb, "FM-1 gone from USB", 0, len);
      return false;
    }
    size_t actual = 0;
    if (!usb->mscRawCommand(address, cdb, uint8_t(cdb_len), dir == Dir::None ? nullptr : data,
                            dir == Dir::None ? 0 : len, dir != Dir::Out, actual, kScsiTimeoutMs)) {
      note(cdb, usb->lastErrorName(), actual, len);
      return false;
    }
    // jl-uboot-tool's SG_IO ignores a short answer to a 16-byte response or the
    // inquiry (the rest stays zero); flash reads must arrive whole.
    // A flash read (FD05) must arrive whole, whatever its size.
    if (dir != Dir::In || actual == len) return true;
    if (cdb[0] != (kReadFlash >> 8) || cdb[1] != (kReadFlash & 0xFF) ? len <= 36 : false) return true;
    note(cdb, "short read", actual, len);
    return false;
  }

 private:
  void note(const uint8_t *cdb, const char *why, size_t actual, size_t len) {
    snprintf(last_failure, sizeof last_failure, "USB: %02X%02X %s %u/%u", cdb[0], cdb[1], why,
             unsigned(actual), unsigned(len));
#if APP_WEB_UI
    // GET /log: also the address the first 4 argument bytes carry for flash
    // commands, and where in the transport mscRawCommand failed.
    EspUsbHost::MscRawResult r;
    if (rom_address.load()) r = usb->mscRawLastResult();
    static const char *const kStage[] = {"-", "no device", "CBW", "CSW", "CSW invalid", "CSW status"};
    DLOG_LOADER("scsi %02X%02X addr %02X%02X%02X%02X %s %u/%u: %s xfer %d data %d csw %u residue %lu", cdb[0],
                cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], why, unsigned(actual), unsigned(len),
                r.stage < 6 ? kStage[r.stage] : "?", r.transferStatus, r.dataStatus, r.cswStatus,
                (unsigned long)r.cswResidue);
#endif
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
  bool reboot_on_miss = false;  // first attempt: a ROM device that doesn't show up restarts the board
  bool wants_reboot = false;
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
    CRUMB_LOADER("sending key");
    key = enter_rom_boot(link, identity);
    CRUMB_LOADER("key done");
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
    while (!ready()) {
      if (millis() - start > timeout_ms) {
        const uint8_t a = rom_address.load();
        if (!a) why = "ROM device never appeared";
        else describe_refusal(a, "ROM device unusable");
        return false;
      }
      // It appeared but wasn't claimed (checkpoint 1, second run), or never
      // appeared (checkpoint 3): it was found the moment everything enumerated
      // afresh. Restart the board once; the FM-1 stays in ROM boot until a
      // power cycle, and the job carries on from waiting for it.
      const uint8_t a = rom_address.load();
      if (reboot_on_miss && ((a && millis() - start > 3000) || (!a && millis() - start > 6000))) {
        if (a) describe_refusal(a, "restarting the board; was");
        else why = "no ROM device seen: restarting the board";
        CRUMB_LOADER("reboot for ROM");
        DLOG_LOADER("ROM wait: %s", why);
        wants_reboot = true;
        return false;
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
  uint32_t now_ms() override { return millis(); }
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
    case Step::Check: return "Checking the FM-1...";
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
// The selected package by id (empty: the top row), so a re-sorted list keeps it.
std::string selected_id;
// What the selected install was previewed as: the hold agrees to exactly that
// package and that data decision.
std::string previewed_id;
DataDecision previewed;
// The last job left the FM-1 in ROM boot: stay in Load mode for the retry.
bool in_rom_boot = false;
// ...and stopped without a reset: the loader still runs in the FM-1's RAM.
bool loader_left_running = false;

// An install stopped part-way: the unit record says so (it survives a reboot of
// this board), and the FM-1 isn't back as a MIDI device (a stale record from an
// install that stopped before writing, say). The top row's hold then retries it
// instead of backing up.
bool resume_pending() {
  Unit u;
  return kWritesEnabled && fm1_midi_address.load() == 0 && library->unit(library->last_unit(), u) &&
         u.has_pending;
}
// What the top row was previewed as: the hold runs that, even if the FM-1 drops
// off USB while the host restarts in Load mode.
bool previewed_retry = false;

int selected_index() {
  const int sel = selected.load();
  return sel >= 0 && sel < int(item_ids.size()) ? sel : -1;
}

constexpr uint32_t kAct = 1, kSelect = 2, kPin = 4, kFm1Changed = 8;

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
  // Keep the selection on the same package when the order changes (a pin).
  int sel = -1;
  for (size_t i = 0; i < item_ids.size(); ++i)
    if (item_ids[i] == selected_id) sel = int(i);
  selected = sel;
  taskENTER_CRITICAL(&lock);
  memcpy(shown.items, items, sizeof items);
  shown.item_count = count;
  taskEXIT_CRITICAL(&lock);
}

// top: the top row's label when it isn't "Back up this FM-1" (the retry).
void set_preview(const char *l0, const char *l1, const char *action, bool can, const char *top = "") {
  taskENTER_CRITICAL(&lock);
  strlcpy(shown.top_label, top, sizeof shown.top_label);
  strlcpy(shown.preview[0], l0, sizeof shown.preview[0]);
  strlcpy(shown.preview[1], l1, sizeof shown.preview[1]);
  strlcpy(shown.action, action, sizeof shown.action);
  shown.can_act = can;
  shown.selected = int8_t(selected.load());
  taskEXIT_CRITICAL(&lock);
}

void refresh_preview() {
  const int sel = selected_index();
  const bool ready = store_ok && blob_ok;
  previewed_id.clear();
  previewed_retry = sel < 0 && resume_pending();
  if (previewed_retry) {
    // The top row's hold writes now: its label says so, not "Back up".
    Unit u;
    PackageInfo p;
    char top[40] = "Retry the install";
    if (library->unit(library->last_unit(), u) && library->package(u.pending.package_id, p))
      snprintf(top, sizeof top, "Retry the %s install", label_of(p).c_str());
    set_preview("An install stopped part-way.", "The FM-1 must still be in ROM boot.", "Hold: retry the install",
                ready && kWritesEnabled, top);
    return;
  }
  if (sel < 0) {
    set_preview("Reads the FM-1 twice.", "Writes nothing.", "Hold: back up FM-1", ready);
    return;
  }
  const InstallPreview p = preview_install(*library, library->last_unit(), item_ids[sel]);
  previewed = p.decision;
  previewed_id = item_ids[sel];
  char l0[40], l1[40], action[40];
  PackageInfo pkg;
  library->package(item_ids[sel], pkg);
  snprintf(action, sizeof action, "Hold: install %s", label_of(pkg).c_str());
  if (!p.ok) {
    set_preview(p.problem, "", action, false);
    return;
  }
  if (p.app_sectors == 0) snprintf(l0, sizeof l0, "Already installed: 0 sectors");
  else snprintf(l0, sizeof l0, "Writes %u of %u app sectors", unsigned(p.app_sectors), unsigned(kAppSectors));
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

void finish(Installer &inst, const Host &host, const UsbScsi &scsi, const char *done_title) {
  DLOG_LOADER("job end: %s; %s; %s; key %s, why '%s', danger %d",
              inst.current() == Step::Done ? done_title : failure_text(inst.failure()), inst.failure_detail().c_str(),
              scsi.last_failure, key_result_text(host.key), host.why, int(inst.danger()));
  if (inst.current() == Step::Done) {
    show(done_title, kGreen, 100);
    if (host.why[0]) show_line(1, host.why);  // a re-enumeration was needed: worth seeing
    return;
  }
  char buf[40];
  if (inst.failure() == Failure::KeyNotSent) snprintf(buf, sizeof buf, "No ROM boot: %s", key_result_text(host.key));
  else snprintf(buf, sizeof buf, "Stopped: %s", failure_text(inst.failure()));
  show(buf, kRed);
  // Line 2: which step and where; line 3: what USB said (or the FM-1's identity).
  if (!inst.failure_detail().empty()) show_line(2, inst.failure_detail().c_str());
  else if (host.why[0]) show_line(2, host.why);
  if (scsi.last_failure[0]) {
    show_line(3, scsi.last_failure);
  } else if (!host.identity.empty()) {
    snprintf(buf, sizeof buf, "FM-1 answered %s", host.identity.c_str());
    show_line(3, buf);
  }
  if (inst.danger()) {
    show_line(4, "DON'T UNPLUG the FM-1 or switch it off");
    show_line(5, "Hold to retry; Transporter if that fails");
  }
}

void run(Installer &inst) {
  Step logged = Step(-1);
  while (inst.step()) {
    if (inst.current() != logged) {
      logged = inst.current();
      CRUMB_LOADER(step_text(logged));
      DLOG_LOADER("step %d %s, %u of %u sectors", int(logged), step_text(logged), unsigned(inst.sectors_done()),
                  unsigned(inst.sectors_total()));
    }
    uint8_t percent = inst.sectors_total() ? uint8_t(inst.sectors_done() * 100 / inst.sectors_total()) : 0;
    show(step_text(inst.current()), kYellow, percent);
  }
}

void log_heap(const char *when) {
  DLOG_LOADER("heap %s: internal %u (largest %u), DMA %u (largest %u), PSRAM %u", when,
              unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
              unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
              unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA)),
              unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
              unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

const char *job_name(JobKind k) {
  switch (k) {
    case JobKind::Backup: return "backup";
    case JobKind::Install: return "install";
    case JobKind::Resume: return "resume";
    default: return "none";
  }
}

// Records the job and restarts the chip; the next boot runs it in Load mode.
[[noreturn]] void restart_for(const JobBoot &j, const char *why) {
  job_boot = j;
  job_boot.magic = kJobMagic;
  DLOG_LOADER("restarting the board: %s (%s, attempt %u)", why, job_name(j.kind), unsigned(j.attempt));
  show("Restarting in Load mode...", kYellow);
  vTaskDelay(pdMS_TO_TICKS(400));  // the screen and the log catch up
  esp_restart();
}

// The hold, on a Play boot (or a Load boot an install stopped in): what the
// preview showed becomes the job of the next boot.
void request_job() {
  JobBoot j = {};
  j.attempt = 1;
  const int sel = selected_index();
  if (sel < 0 && previewed_retry) {
    j.kind = JobKind::Resume;
    j.loader_running = loader_left_running;
  } else if (sel < 0) {
    j.kind = JobKind::Backup;
  } else {
    if (item_ids[sel] != previewed_id) {  // the hold agreed to what the preview showed
      show("Stopped: selection changed. Check it.", kRed);
      return;
    }
    j.kind = JobKind::Install;
    strlcpy(j.package, previewed_id.c_str(), sizeof j.package);
    strlcpy(j.unit, library->last_unit().c_str(), sizeof j.unit);
    j.fresh_ok = previewed.action == DataAction::FreshStart;
    j.layout_ok = previewed.layout_warning;
  }
  restart_for(j, "the hold");
}

// A Load boot: runs active_job, then restarts into Play unless an install
// stopped part-way (the FM-1 stays in ROM boot, and the retry is a hold away).
void run_job() {
  JobBoot &j = active_job;
  UsbScsi scsi;
  Host host;
  host.reboot_on_miss = j.attempt < 2 && j.kind != JobKind::Resume;
  Installer inst(scsi, host, *library, blob);
  InstallOptions o;
  o.confirmed = true;  // the 2-second hold on the Firmware page, before the restart
  o.rom_timeout_ms = kRomWaitMs;
  o.key_sent = j.key_sent;
  DLOG_LOADER("job start: %s %s, attempt %u%s", job_name(j.kind), j.package, unsigned(j.attempt),
              j.key_sent ? ", key already sent" : "");
  log_heap("job start");
  for (int i = 1; i < kLoaderLines; ++i) show_line(i, i == 1 ? blob_line : "");
  if (j.kind == JobKind::Resume) {
    // The FM-1 is still in ROM boot with an incomplete app: no key, straight to it.
    if (!host.wait_for_rom_device(kRomWaitMs)) {
      show("Stopped: the FM-1 isn't in ROM boot", kRed);
      if (host.why[0]) show_line(2, host.why);
    } else {
      o.loader_running = j.loader_running;
      inst.start_resume(o);
      run(inst);
      finish(inst, host, scsi, "Install finished.");
    }
  } else if (j.kind == JobKind::Backup) {
    inst.start_backup(o);
    run(inst);
    finish(inst, host, scsi, "Backed up. Nothing written.");
    if (inst.current() == Step::Done) report_backup(inst, host);
  } else {
    o.unit_id = j.unit;  // the unit the preview was for
    o.fresh_start_ok = j.fresh_ok;
    o.layout_change_ok = j.layout_ok;
    inst.start_install(j.package, o);
    run(inst);
    char buf[40];
    snprintf(buf, sizeof buf, "Installed: %u sectors written", unsigned(inst.sectors_done()));
    finish(inst, host, scsi, buf);
  }
  log_heap("job end");
#if LOADER_SOAK
  if (!(inst.failure() == Failure::NoRomDevice && host.wants_reboot)) {
    ++soak.runs;
    if (inst.current() == Step::Done) {
      ++soak.ok;
      soak.failed_in_row = 0;
    } else {
      ++soak.failed;
      ++soak.failed_in_row;
    }
    DLOG_LOADER("soak run %u: %s (ok %u, failed %u, crashes %u)", unsigned(soak.runs),
                inst.current() == Step::Done ? "ok" : failure_text(inst.failure()), unsigned(soak.ok),
                unsigned(soak.failed), unsigned(soak.crashes));
  }
#endif
  if (inst.failure() == Failure::NoRomDevice && host.wants_reboot) {
    JobBoot again = j;
    again.attempt = 2;
    again.key_sent = true;
    restart_for(again, "no ROM device after the key");
  }
  in_rom_boot = inst.danger();
  loader_left_running = inst.danger();
  if (in_rom_boot) {  // stay: the top row's hold is now the retry
    selected = -1;
    selected_id.clear();
    return;
  }
  // Back to Play on a fresh boot, with the result kept for the Firmware page.
  JobBoot done = {};
  done.has_result = true;
  taskENTER_CRITICAL(&lock);
  strlcpy(done.result_title, shown.title, sizeof done.result_title);
  done.result_colour = shown.title_colour;
  for (int i = 0; i < kLoaderLines; ++i) strlcpy(done.result_lines[i], shown.lines[i], sizeof done.result_lines[i]);
  taskEXIT_CRITICAL(&lock);
  job_boot = done;
  job_boot.magic = kJobMagic;
  DLOG_LOADER("job done: back to Play");
  vTaskDelay(pdMS_TO_TICKS(1500));  // the result stays on screen a moment
  esp_restart();
}

// The store line: free space and units, refreshed after each job.
void refresh_store_line() {
  if (!store_ok) return;
  snprintf(store_line, sizeof store_line, "Store %.1f of %.1f MB free, %u units",
           (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1048576.0, LittleFS.totalBytes() / 1048576.0,
           unsigned(library->unit_ids().size()));
  show_line(0, store_line);
}

// GET /store's body, built here because Library isn't thread-safe.
void publish_store() {
#if APP_WEB_UI
  if (store_ok) diag_publish_store(store_json(*library, *storage, LittleFS.totalBytes(), LittleFS.usedBytes()));
#endif
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
    publish_store();
    DIAG("diag: loader task idle, stack left %u\n", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
  log_heap(boot_mode == platform::Mode::Load ? "Load boot" : "Play boot");
  if (store_ok && blob_ok && active_job.kind != JobKind::None) {
    run_job();  // restarts the board unless an install stopped part-way
    refresh_store_line();
    refresh_items();
    refresh_preview();
    publish_store();
    busy = false;
  }
#if LOADER_SOAK
  if (soak.magic != kSoakMagic) soak = {kSoakMagic, 0, 0, 0, 0, 0};
  if (store_ok && blob_ok && boot_mode == platform::Mode::Play) {
    // The next backup, once the FM-1 is back (as MIDI, or still in ROM boot
    // after a crash: then the key is skipped and the job resets it home).
    wait_until(20000, [] { return fm1_midi_address.load() != 0 || rom_address.load() != 0; });
    // 45 s in Play: long enough for the Mac (tools/soak_watch.py) to read the log
    // tail over Wi-Fi, whose first answer after a boot can take 20 s.
    vTaskDelay(pdMS_TO_TICKS(45000));
    const bool in_rom = rom_address.load() != 0;
    if (soak.runs >= kSoakRuns || soak.failed_in_row >= 5) {
      DLOG_LOADER("soak finished: %u runs, ok %u, failed %u, crashes %u", unsigned(soak.runs), unsigned(soak.ok),
                  unsigned(soak.failed), unsigned(soak.crashes));
      show("Soak finished", kGreen);
    } else if (fm1_midi_address.load() || in_rom) {
      JobBoot j = {};
      j.kind = JobKind::Backup;
      j.attempt = 1;
      j.key_sent = in_rom;
      restart_for(j, in_rom ? "soak, FM-1 in ROM boot" : "soak");
    } else {
      show("Soak paused: no FM-1", kRed);
    }
  }
#endif
  for (;;) {
    uint32_t bits = 0;
    CRUMB_LOADER("idle");
    xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
    CRUMB_LOADER("woken");
    if (!store_ok) continue;
    if (bits & kSelect) {
      const int i = selected_index();
      selected_id = i < 0 ? std::string() : item_ids[i];
    }
    if (bits & kPin) {
      const int i = pin_request.exchange(-1);
      if (i >= 0 && i < int(item_ids.size())) {
        PackageInfo p;
        if (library->package(item_ids[i], p)) library->set_pinned(p.id, !p.pinned);
        refresh_items();
        publish_store();
      }
    }
    if (bits & (kSelect | kPin | kFm1Changed)) refresh_preview();
    if (!(bits & kAct)) continue;
    request_job();  // restarts the board, unless the selection changed
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
  // A job recorded before the restart runs now, once: it's cleared first, so a
  // crash during it (safe mode, then RST) doesn't run it again.
  const bool job_valid = job_boot.magic == kJobMagic;
  if (job_valid && job_boot.kind != JobKind::None && !last_crash[0][0]) {
    active_job = job_boot;
    boot_mode = platform::Mode::Load;
    busy = true;
    show("Load mode: starting the job...", kYellow);
  } else if (job_valid && job_boot.has_result) {  // the last job's result, after its restart
    show(job_boot.result_title, job_boot.result_colour);
    for (int i = 0; i < kLoaderLines; ++i) show_line(i, job_boot.result_lines[i]);
  }
  memset(&job_boot, 0, sizeof job_boot);
  if (last_crash[0][0]) {  // the board reset abnormally: say where
    for (int i = 0; i < 3; ++i) show_line(2 + i, last_crash[i]);
  }
  char mem[40];
  snprintf(mem, sizeof mem, "Free RAM %u KB internal, %u KB PSRAM",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  if (!shown.lines[5][0]) show_line(5, mem);
  xTaskCreatePinnedToCore(loader_task, "loader", 12288, nullptr, 2, &task, 1);
}

void loader_device_connected(const EspUsbHostDeviceInfo &d) {
  platform::DeviceDesc desc = {};
  desc.vid = d.vid;
  desc.pid = d.pid;
  if (platform::classify(desc) == platform::Kind::Fm1RomBoot) rom_address = d.address;
  else if (midi_core::device_role(d.vid, d.pid) == midi_core::Role::Target) fm1_midi_address = d.address;
  if (task) xTaskNotify(task, kFm1Changed, eSetBits);  // the top row may change (resume_pending)
}

void loader_device_disconnected(const EspUsbHostDeviceInfo &d) {
  uint8_t a = d.address;
  rom_address.compare_exchange_strong(a, 0);
  a = d.address;
  fm1_midi_address.compare_exchange_strong(a, 0);
  if (task) xTaskNotify(task, kFm1Changed, eSetBits);
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

platform::Mode loader_boot_mode() { return boot_mode; }

#if LOADER_SOAK
void loader_soak_crashed() {
  if (soak.magic == kSoakMagic) ++soak.crashes;
}
#endif

void loader_ui(UiLoader &out) {
  taskENTER_CRITICAL(&lock);
  out = shown;
  taskEXIT_CRITICAL(&lock);
  out.busy = busy;
}

#endif  // APP_LOADER
