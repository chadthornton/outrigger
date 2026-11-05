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
#include "loader_messages.h"
#include "midi_core.h"
#include "posix_storage.h"
#include "softkey.h"
#if APP_WEB_UI
#include "store_json.h"
#endif

using namespace fm1_loader;
using loader_messages::Chain;
using loader_messages::JobKind;
using loader_messages::KeyStop;

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
struct JobBoot {
  uint32_t magic;
  JobKind kind;
  uint8_t attempt;  // 2, 3: the board restarted after a key because no ROM device showed up
  bool key_sent, fresh_ok, layout_ok, loader_running;
  char package[48], unit[40];
  // A switch that found the FM-1 changed: its backup, then its install
  // (switch_chain.h). Each stage is its own boot; the package, unit and the
  // preview's consent (fresh_ok, layout_ok) carry through.
  Chain chain;
  char stale_backup[12];  // the backup the FM-1 had changed since ("b50")
};
// Changed with JobBoot's layout: a record left by an older build is never
// read as a job.
constexpr uint32_t kJobMagic = 0x10B5EED3;
constexpr uint8_t kKeyTries = 3;  // boots that look for ROM boot, each after a key
RTC_NOINIT_ATTR JobBoot job_boot;

// The job's result, for the result page after the boot back to Play.
struct JobResult {
  uint32_t magic;
  char package[48];  // the switch's package: its plan is previewed again, for the result's hold
  UiResult result;
};
// With the struct's size in it: a build whose UiResult differs never reads an
// older build's record.
constexpr uint32_t kResultMagic = 0x2E5017ED ^ uint32_t(sizeof(UiResult));
RTC_NOINIT_ATTR JobResult job_result;

#if LOADER_SOAK
// Soak builds back up the FM-1 by themselves, job after job, to make the USB
// host crash on the FM-1's disconnect after the key happen often enough to study.
// Backups write nothing; each one sends the FM-1 back to its firmware.
struct Soak {
  uint32_t magic;
  uint32_t runs, ok, failed, crashes, failed_in_row;
  uint32_t deferrals;  // disconnects whose endpoints waited for IDF (EspUsbHost::disconnectDeferrals)
};
constexpr uint32_t kSoakMagic = 0x50A4C0E0;  // changed per soak build: counts start over
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
constexpr const char *kDumpHead = "d9f43191777656de01c7bed4f9e9ba2e34e94832c5d2ed02c3fc7ab7a6d9bd67";
constexpr const char *kDumpFactory = "70a8f5bea5097f4a0046682cdb5bf2e4ad6962901bd42debfe30c95a5ef3f3b0";

EspUsbHost *usb = nullptr;
LoaderCore core = {};
JobBoot active_job = {};  // the job this boot runs (kind None: a Play boot)

// Devices, written on the USB client task.
std::atomic<uint8_t> fm1_midi_address{0};
std::atomic<bool> fm1_switchable{false};  // the key has been tried on its firmware (can_switch_firmware)
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
  // BENCH builds only: a Backup job may send the key to an Open FM-1, to find out
  // whether Open takes it (docs/open-firmware.md, the probe). A backup writes nothing.
  bool untested_key_ok = false;
  bool wants_reboot = false;
  std::string identity;
  KeyResult key = KeyResult::NoAnswer;
  const char *why = "";  // which check stopped the key, for the log and /status
  KeyStop stop = KeyStop::None;  // the same, for the result page
  bool send_rom_boot_key() override {
    show("Asking the FM-1...", kYellow);
    if (rom_address.load()) {  // left there by an earlier job or the FM-1's own hotkey
      key = KeyResult::SendFailed;
      why = "FM-1 already in ROM boot: power-cycle it";
      stop = KeyStop::AlreadyInRom;
      return false;
    }
    // EspUsbHost reports a device as connected before its interfaces are
    // claimed: wait until its MIDI endpoints are up and the broker gives the
    // Loader the FM-1's MIDI (only in Load mode).
    if (!wait_until(kFm1AppearMs, [] { return fm1_midi_address.load() != 0; })) {
      key = KeyResult::NoAnswer;
      why = "no FM-1 on the hub";
      stop = KeyStop::NoFm1;
      return false;
    }
    if (!wait_until(kFm1AppearMs, [] {
          const uint8_t a = fm1_midi_address.load();
          return a != 0 && usb->midiReady(a);
        })) {
      key = KeyResult::SendFailed;
      why = "FM-1 MIDI never ready";
      stop = KeyStop::NotReady;
      return false;
    }
    if (!wait_until(2000, [] { return core.owns_fm1_midi && core.owns_fm1_midi(); })) {
      key = KeyResult::SendFailed;
      why = "broker: Loader doesn't own FM-1 MIDI";
      stop = KeyStop::NotReady;
      return false;
    }
    // Checked last, just before the key, for the FM-1 that's on the hub now.
    if (!fm1_switchable.load() && untested_key_ok) DLOG_LOADER("bench: key sent to an untested FM-1 (backup only)");
    if (!fm1_switchable.load() && !untested_key_ok) {
      key = KeyResult::NotFm1;
      why = "M-VAVE Open firmware: key untested";
      stop = KeyStop::Unsupported;
      return false;
    }
    UsbMidiLink link;
    CRUMB_LOADER("sending key");
    key = enter_rom_boot(link, identity);
    CRUMB_LOADER("key done");
    if (key != KeyResult::Sent) stop = KeyStop::Answer;
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
  snprintf(buf, sizeof buf, "Unit %s", id.c_str());
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
std::atomic<int> selected{-1};
// The hold that just completed: what its label named, and for a switch its row.
std::atomic<uint8_t> act_kind{uint8_t(UiAct::None)};
std::atomic<int> act_index{-1};
// The selected package by id (empty: none), so a re-sorted list keeps it.
std::string selected_id;
// What the selected switch was previewed as: the hold agrees to exactly that
// package and that data decision.
std::string previewed_id;
DataDecision previewed;
// The last job left the FM-1 in ROM boot: stay in Load mode for the retry.
bool in_rom_boot = false;
// ...and stopped without a reset: the loader still runs in the FM-1's RAM.
bool loader_left_running = false;
uint32_t result_seq = 0;  // numbers each result, so the UI opens a new one once

// An install stopped part-way: the unit record says so (it survives a reboot of
// this board), and the FM-1 isn't back as a MIDI device (a stale record from an
// install that stopped before writing, say). Only the retry runs then.
bool resume_pending() {
  Unit u;
  return kWritesEnabled && fm1_midi_address.load() == 0 && library->unit(library->last_unit(), u) &&
         u.has_pending;
}
// What the last preview found: the retry's hold runs only while it's true, even
// if the FM-1 drops off USB while the host restarts in Load mode.
bool previewed_retry = false;

int selected_index() {
  const int sel = selected.load();
  return sel >= 0 && sel < int(item_ids.size()) ? sel : -1;
}

constexpr uint32_t kAct = 1, kSelect = 2, kFm1Changed = 8;

std::string label_of(const PackageInfo &p) {
  std::string name = p.name.empty() ? p.id : p.name;
  return p.version.empty() ? name : name + " " + p.version;
}

// The FM-1 the screen talks about (the unit backed up last): its record, and
// what its latest backup runs.
struct Known {
  bool has_unit = false;
  Unit unit;
  Firmware running;
  std::string running_label;  // the package it matches, or ""
  char latest[12] = "", first[12] = "";
};
Known known_unit() {
  Known k;
  FlashHashes flash;
  k.has_unit = library->unit(library->last_unit(), k.unit) && !k.unit.latest_backup.empty();
  if (!k.has_unit) return k;
  loader_messages::backup_name(k.unit.latest_backup.c_str(), k.latest, sizeof k.latest);
  loader_messages::backup_name(k.unit.baseline_backup.c_str(), k.first, sizeof k.first);
  if (library->backup(k.unit.latest_backup, flash) && flash.size() == kSectors)
    k.running = library->identify_app(std::vector<Hash>(flash.begin() + kAppStart / kSector,
                                                        flash.begin() + kAppEnd / kSector));
  if (k.running.known())
    for (const std::string &id : library->package_ids()) {
      PackageInfo p;
      if (library->package(id, p) && p.app_sha256 == k.running.app_sha256) k.running_label = label_of(p);
    }
  return k;
}

std::string package_label(const std::string &id) {
  PackageInfo p;
  return library->package(id, p) ? label_of(p) : id;
}

void refresh_items() {
  const Known k = known_unit();
  const std::vector<SaveState> saves = k.has_unit ? library->saves_of(k.unit.id) : std::vector<SaveState>();
  std::vector<PackageInfo> pkgs;
  for (const std::string &id : library->package_ids()) {
    PackageInfo p;
    if (library->package(id, p)) pkgs.push_back(p);
  }
  // The running one first, then by name.
  auto running = [&](const PackageInfo &p) { return k.running.known() && k.running.app_sha256 == p.app_sha256; };
  std::sort(pkgs.begin(), pkgs.end(), [&](const PackageInfo &a, const PackageInfo &b) {
    return running(a) != running(b) ? running(a) : label_of(a) < label_of(b);
  });
  // Built here, then copied under the lock: nothing inside a critical section
  // may allocate (ESP-IDF aborts, and the board reboots in a loop).
  UiLoaderItem items[kLoaderItems] = {};
  uint8_t count = 0;
  item_ids.clear();
  for (const PackageInfo &p : pkgs) {
    if (count == kLoaderItems) break;
    UiLoaderItem &it = items[count++];
    strlcpy(it.label, label_of(p).c_str(), sizeof it.label);
    // "your songs": a save of this unit for this firmware, which a switch brings back.
    bool songs = false;
    for (const SaveState &s : saves) songs |= s.identity == p.identity && s.app_sha256 == p.app_sha256;
    it.note = running(p) ? UiNote::Running : songs ? UiNote::YourSongs : UiNote::None;
    item_ids.push_back(p.id);
  }
  // Keep the selection on the same package when the order changes.
  int sel = -1;
  for (size_t i = 0; i < item_ids.size(); ++i)
    if (item_ids[i] == selected_id) sel = int(i);
  selected = sel;
  char running_label[28], backups[40];
  strlcpy(running_label, k.running_label.c_str(), sizeof running_label);
  if (k.has_unit) snprintf(backups, sizeof backups, "Latest %s, first %s", k.latest, k.first);
  else strlcpy(backups, "No backup yet", sizeof backups);
  taskENTER_CRITICAL(&lock);
  memcpy(shown.items, items, sizeof items);
  shown.item_count = count;
  memcpy(shown.running, running_label, sizeof running_label);
  memcpy(shown.backups_note, backups, sizeof backups);
  taskEXIT_CRITICAL(&lock);
}

// A result for the result page: numbered, so the UI opens it once.
void set_result(UiResult &r) {
  r.seq = ++result_seq;
  taskENTER_CRITICAL(&lock);
  shown.result = r;
  taskEXIT_CRITICAL(&lock);
}

void refresh_preview() {
  const bool ready = store_ok && blob_ok;
  const Known k = known_unit();
  previewed_retry = resume_pending();
  previewed_id.clear();

  // What stands in the list's way: a store or loader file that's bad, or an
  // install the FM-1 is still in ROM boot for.
  UiResult blocked = {};
  std::string pending_label;
  if (previewed_retry) pending_label = package_label(k.unit.pending.package_id);
  // An unfinished install comes first: its card says not to unplug the FM-1,
  // even when the retry can't run (can_retry is off then).
  if (previewed_retry) loader_messages::describe_unfinished(pending_label.c_str(), k.latest, blocked);
  else if (!ready) loader_messages::describe_not_ready(store_ok, blob_ok, blob.empty(), !store_ok ? store_line : blob_line, blocked);

  UiPlan plan = {};
  plan.item = -1;
  const int sel = selected_index();
  if (sel >= 0) {
    const std::string target = package_label(item_ids[sel]);
    loader_messages::PlanInput in = {};
    in.target = target.c_str();
    in.running = k.running_label.c_str();
    in.backup = k.latest;
    in.has_unit = k.has_unit;
    in.ready = ready;
    in.writes_enabled = kWritesEnabled;
    in.unfinished = previewed_retry;
    in.preview = preview_install(*library, library->last_unit(), item_ids[sel]);
    previewed = in.preview.decision;
    previewed_id = item_ids[sel];
    plan.item = int8_t(sel);
    loader_messages::describe_plan(in, plan);
  }
  taskENTER_CRITICAL(&lock);
  shown.blocked = blocked;
  shown.unfinished = previewed_retry;
  shown.plan = plan;
  shown.can_back_up = ready && !previewed_retry;
  shown.can_retry = ready && kWritesEnabled && previewed_retry;
  taskEXIT_CRITICAL(&lock);
}

// The job page: the job's steps, from where the Installer is.
void show_job(const JobBoot &job, Step step, const char *firmware, const char *backup, uint32_t done, uint32_t total,
              bool danger) {
  loader_messages::JobInput in = {job.kind, step, firmware, backup, done, total, danger};
  in.chain = job.chain;
  in.stale_backup = job.stale_backup;
  UiJob j;
  loader_messages::describe_job(in, j);
  taskENTER_CRITICAL(&lock);
  shown.job = j;
  taskEXIT_CRITICAL(&lock);
}

// What a job is about, for its page and its result.
struct JobNames {
  std::string firmware;  // the switch's or the retry's package label
  char backup[12] = "";  // the unit's latest backup
};
JobNames job_names(const JobBoot &j) {
  JobNames n;
  if (!store_ok) return n;
  const Known k = known_unit();
  strlcpy(n.backup, k.latest, sizeof n.backup);
  if (j.package[0]) n.firmware = package_label(j.package);  // a switch, or its chained backup
  else if (j.kind == JobKind::Resume && k.unit.has_pending) n.firmware = package_label(k.unit.pending.package_id);
  return n;
}

// The stage's result, unless the switch's chain carries on (switch_chain.h):
// then there's no result yet, only the next stage.
bool chain_goes_on(const Installer &inst, const JobBoot &job) {
  return loader_messages::next_after(job.kind, job.chain, inst.current() == Step::Done, inst.failure(), inst.danger())
      .restart;
}

void finish(Installer &inst, const Host &host, const UsbScsi &scsi, const char *done_title, const JobBoot &job,
            const JobNames &names, bool wrote) {
  const JobKind kind = job.kind;
  DLOG_LOADER("job end: %s; %s; %s; key %s, why '%s', danger %d",
              inst.current() == Step::Done ? done_title : failure_text(inst.failure()), inst.failure_detail().c_str(),
              scsi.last_failure, key_result_text(host.key), host.why, int(inst.danger()));
  // The Web UI's /status keeps today's lines.
  char buf[40];
  if (inst.current() == Step::Done) {
    show(done_title, kGreen, 100);
    if (host.why[0]) show_line(1, host.why);  // a re-enumeration was needed: worth seeing
  } else {
    if (inst.failure() == Failure::KeyNotSent)
      snprintf(buf, sizeof buf, "No ROM boot: %s",
               host.stop == KeyStop::Unsupported ? host.why : key_result_text(host.key));
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
      show_line(4, "DON'T unplug or turn off the FM-1");
      show_line(5, "If the retry fails: Transporter");
    }
  }

  // The result page (loader_messages.cpp's table).
  if (chain_goes_on(inst, job)) return;
  char detail[96] = "";
  if (!inst.failure_detail().empty()) {
    snprintf(detail, sizeof detail, "%s%s%s", inst.failure_detail().c_str(), scsi.last_failure[0] ? "; " : "",
             scsi.last_failure);
  } else if (host.why[0]) {
    strlcpy(detail, host.why, sizeof detail);
  } else if (scsi.last_failure[0]) {
    strlcpy(detail, scsi.last_failure, sizeof detail);
  }
  Unit u;
  const bool pending = library->unit(library->last_unit(), u) && u.has_pending;
  const Known k = known_unit();
  loader_messages::JobOutcome o = {};
  o.kind = kind;
  o.done = inst.current() == Step::Done;
  o.failure = inst.failure();
  o.key_stop = host.stop;
  o.key = host.key;
  o.danger = inst.danger();
  o.wrote = wrote;
  o.pending = pending;
  o.firmware = names.firmware.c_str();
  o.backup = k.latest[0] ? k.latest : names.backup;
  o.detail = detail;
  o.sectors = inst.sectors_done();
  o.data = inst.decision().action;
  o.chain = job.chain;
  o.stale_backup = job.stale_backup;
  UiResult r = {};
  loader_messages::describe_result(o, r);
  set_result(r);
}

// Runs the Installer to its end, keeping the job page current. wrote: a write
// step was reached (sectors_done() also counts reads, so it can't tell).
void run(Installer &inst, const JobBoot &job, const JobNames &names, bool &wrote) {
  Step logged = Step(-1);
  while (inst.step()) {
    if (inst.current() != logged) {
      logged = inst.current();
      CRUMB_LOADER(step_text(logged));
      DLOG_LOADER("step %d %s, %u of %u sectors", int(logged), step_text(logged), unsigned(inst.sectors_done()),
                  unsigned(inst.sectors_total()));
      if (logged == Step::WriteApp) {  // the plan is final here, for an install and a resume
        const DataDecision &d = inst.decision();
        DLOG_LOADER("data: %s%s, %u sectors to write",
                    d.action == DataAction::Restore      ? "restore"
                    : d.action == DataAction::FreshStart ? "fresh"
                                                         : "keep",
                    d.clean_slate ? " on a clean slate" : "", unsigned(inst.sectors_total()));
      }
    }
    if (inst.current() == Step::WriteApp || inst.current() == Step::WriteData) wrote = true;
    uint8_t percent = inst.sectors_total() ? uint8_t(inst.sectors_done() * 100 / inst.sectors_total()) : 0;
    show(step_text(inst.current()), kYellow, percent);
    show_job(job, inst.current(), names.firmware.c_str(), names.backup, inst.sectors_done(), inst.sectors_total(),
             inst.danger());
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

// A hold whose page no longer matches what the Loader previewed: nothing runs.
void refuse(const char *why) {
  DLOG_LOADER("hold refused: %s", why);
  show("Stopped: the page changed. Check it.", kRed);
  UiResult r = {};
  loader_messages::describe_refused(r);
  set_result(r);
}

// The hold, on a Play boot (or a Load boot an install stopped in): what the
// preview showed becomes the job of the next boot. The hold says what it ran
// (its label's kind, and for a switch its row); anything that doesn't match
// the last preview is refused.
void request_job(UiAct act, int index) {
  JobBoot j = {};
  j.attempt = 1;
  const int sel = selected_index();
  const char *why = "";
  j.kind = loader_messages::job_for_hold(
      {act, index, sel, sel >= 0 && item_ids[sel] == previewed_id, resume_pending(), previewed_retry}, &why);
  if (j.kind == JobKind::None) return refuse(why);
  if (j.kind == JobKind::Resume) {
    j.loader_running = loader_left_running;
  } else if (j.kind == JobKind::Install) {
    // The hold agreed to what the preview showed.
    strlcpy(j.package, previewed_id.c_str(), sizeof j.package);
    strlcpy(j.unit, library->last_unit().c_str(), sizeof j.unit);
    j.fresh_ok = needs_erase_ok(previewed);
    j.layout_ok = previewed.layout_warning;
  }
  const JobNames names = job_names(j);
  show_job(j, Step::Idle, names.firmware.c_str(), names.backup, 0, 0, false);
  restart_for(j, "the hold");
}

// A Load boot: runs active_job, then restarts into Play unless an install
// stopped part-way (the FM-1 stays in ROM boot, and the retry is a hold away).
void run_job() {
  JobBoot &j = active_job;
  UsbScsi scsi;
  Host host;
  host.reboot_on_miss = j.attempt < kKeyTries && j.kind != JobKind::Resume;
#if BENCH
  host.untested_key_ok = j.kind == JobKind::Backup;
#endif
  Installer inst(scsi, host, *library, blob);
  InstallOptions o;
  o.confirmed = true;  // the 2-second hold on the Firmware page, before the restart
  o.rom_timeout_ms = kRomWaitMs;
  o.key_sent = j.key_sent;
  if (j.key_sent) {
    // A retry after a restart: the FM-1 is in ROM boot, or it took the key but
    // restarted into its own firmware (the soak saw 2 in 7). Then it gets the key again.
    wait_until(10000, [] { return rom_address.load() != 0 || fm1_midi_address.load() != 0; });
    if (!rom_address.load() && fm1_midi_address.load()) {
      DLOG_LOADER("retry: the FM-1 is back in its firmware, sending the key again");
      o.key_sent = false;
    }
  }
  DLOG_LOADER("job start: %s %s, attempt %u%s", job_name(j.kind), j.package, unsigned(j.attempt),
              j.key_sent ? ", key already sent" : "");
  log_heap("job start");
  for (int i = 1; i < kLoaderLines; ++i) show_line(i, i == 1 ? blob_line : "");
  const JobNames names = job_names(j);
  bool wrote = false;
  show_job(j, Step::Confirm, names.firmware.c_str(), names.backup, 0, 0, false);
  if (j.kind == JobKind::Resume) {
    // The FM-1 is still in ROM boot with an incomplete app: no key, straight to it.
    if (!host.wait_for_rom_device(kRomWaitMs)) {
      show("Stopped: the FM-1 isn't in ROM boot", kRed);
      if (host.why[0]) show_line(2, host.why);
      UiResult r = {};
      loader_messages::describe_result({JobKind::Resume, false, Failure::NoRomDevice, KeyStop::None,
                                        KeyResult::Sent, false, false, true, names.firmware.c_str(), names.backup,
                                        host.why, 0, DataAction::Keep},
                                       r);
      set_result(r);
    } else {
      o.loader_running = j.loader_running;
      inst.start_resume(o);
      run(inst, j, names, wrote);
      finish(inst, host, scsi, "Install finished.", j, names, wrote);
    }
  } else if (j.kind == JobKind::Backup) {
    inst.start_backup(o);
    run(inst, j, names, wrote);
    finish(inst, host, scsi, "Backed up. Nothing written.", j, names, wrote);
    if (inst.current() == Step::Done) report_backup(inst, host);
  } else {
    o.unit_id = j.unit;  // the unit the preview was for
    o.fresh_start_ok = j.fresh_ok;
    o.layout_change_ok = j.layout_ok;
    inst.start_install(j.package, o);
    run(inst, j, names, wrote);
    char buf[40];
    snprintf(buf, sizeof buf, "Installed: %u sectors written", unsigned(inst.sectors_done()));
    finish(inst, host, scsi, buf, j, names, wrote);
  }
  log_heap("job end");
  DLOG_LOADER("loader task stack left %u", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
#if LOADER_SOAK
  // This boot's count: each job runs in its own boot, so the RTC record sums them.
  if (usb) soak.deferrals += usb->disconnectDeferrals();
  if (!(inst.failure() == Failure::NoRomDevice && host.wants_reboot)) {
    ++soak.runs;
    if (inst.current() == Step::Done) {
      ++soak.ok;
      soak.failed_in_row = 0;
    } else {
      ++soak.failed;
      ++soak.failed_in_row;
    }
    DLOG_LOADER("soak run %u: %s (ok %u, failed %u, crashes %u, deferrals %u)", unsigned(soak.runs),
                inst.current() == Step::Done ? "ok" : failure_text(inst.failure()), unsigned(soak.ok),
                unsigned(soak.failed), unsigned(soak.crashes), unsigned(soak.deferrals));
  }
#endif
  if (inst.failure() == Failure::NoRomDevice && host.wants_reboot) {
    JobBoot again = j;
    again.attempt = uint8_t(j.attempt + 1);
    again.key_sent = true;  // the next boot checks whether the FM-1 is in ROM boot
    restart_for(again, "no ROM device after the key");
  }
  // A switch whose FM-1 changed since its backup (or had none): back up, then
  // install against that backup, each in a boot of its own. Nothing was erased.
  if (chain_goes_on(inst, j)) {
    const loader_messages::NextJob next =
        loader_messages::next_after(j.kind, j.chain, inst.current() == Step::Done, inst.failure(), inst.danger());
    JobBoot then = j;
    then.kind = next.kind;
    then.chain = next.chain;
    then.attempt = 1;
    then.key_sent = false;
    if (next.chain == Chain::Backup) strlcpy(then.stale_backup, names.backup, sizeof then.stale_backup);
    restart_for(then, next.chain == Chain::Backup ? "switch: the FM-1 changed, backing up first"
                                                  : "switch: backed up, installing");
  }
  in_rom_boot = inst.danger();
  loader_left_running = inst.danger();
  if (in_rom_boot) {  // stay: the result page's hold is now the retry
    selected = -1;
    selected_id.clear();
    return;
  }
  // Back to Play on a fresh boot, with the result kept for the result page.
  JobResult done = {};
  done.magic = kResultMagic;
  strlcpy(done.package, j.package, sizeof done.package);  // a switch's, or its chained backup's
  taskENTER_CRITICAL(&lock);
  done.result = shown.result;
  taskEXIT_CRITICAL(&lock);
  job_result = done;
  job_boot = {};
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
  if (soak.magic != kSoakMagic) soak = {kSoakMagic, 0, 0, 0, 0, 0, 0};
  if (store_ok && blob_ok && boot_mode == platform::Mode::Play) {
    // The next backup, once the FM-1 is back (as MIDI, or still in ROM boot
    // after a crash: then the key is skipped and the job resets it home).
    wait_until(20000, [] { return fm1_midi_address.load() != 0 || rom_address.load() != 0; });
    // 45 s in Play: long enough for the Mac (tools/soak_watch.py) to read the log
    // tail over Wi-Fi, whose first answer after a boot can take 20 s.
    vTaskDelay(pdMS_TO_TICKS(45000));
    const bool in_rom = rom_address.load() != 0;
    if (soak.runs >= kSoakRuns || soak.failed_in_row >= 5) {
      DLOG_LOADER("soak finished: %u runs, ok %u, failed %u, crashes %u, deferrals %u", unsigned(soak.runs),
                  unsigned(soak.ok), unsigned(soak.failed), unsigned(soak.crashes), unsigned(soak.deferrals));
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
    if (bits & (kSelect | kFm1Changed)) refresh_preview();
    if (!(bits & kAct)) continue;
    // Restarts the board, unless the hold no longer matches the preview.
    request_job(UiAct(act_kind.exchange(uint8_t(UiAct::None))), act_index.load());
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

const char *loader_boot_title() {
  if (last_crash[0][0]) return nullptr;  // safe mode: the crash screen
  if (job_boot.magic == kJobMagic && job_boot.kind != JobKind::None)
    return job_boot.kind == JobKind::Backup ? "Backing up" : "Switching firmware";
  if (job_result.magic == kResultMagic) return "Finishing up";
  return nullptr;
}

void loader_begin(EspUsbHost &host, const LoaderCore &c) {
  usb = &host;
  core = c;
  sysex_queue = xQueueCreate(8, sizeof(SysexMsg));
  shown.plan.item = -1;
  open_store();
  // The FM-1 page's reason when the store can't be read: refresh_preview, which
  // needs the store, keeps it current otherwise.
  if (!store_ok || !blob_ok)
    loader_messages::describe_not_ready(store_ok, blob_ok, blob.empty(), !store_ok ? store_line : blob_line, shown.blocked);
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
    const JobNames names = job_names(active_job);
    show_job(active_job, Step::Confirm, names.firmware.c_str(), names.backup, 0, 0, false);
  } else if (job_result.magic == kResultMagic && !last_crash[0][0]) {
    // The last job's result, after its restart: the UI opens it. A failed
    // switch's plan is previewed again, so the result's hold can run it.
    result_seq = 1;
    UiResult &r = job_result.result;
    r.headline[sizeof r.headline - 1] = r.safety_text[sizeof r.safety_text - 1] = r.why[sizeof r.why - 1] = 0;
    r.todo[sizeof r.todo - 1] = r.detail[sizeof r.detail - 1] = r.fix_label[sizeof r.fix_label - 1] = 0;
    job_result.package[sizeof job_result.package - 1] = 0;
    shown.result = r;
    shown.result.seq = result_seq;
    show(shown.result.headline, shown.result.ok ? kGreen : kRed);
    if (shown.result.fix == UiAct::Switch) selected_id = job_result.package;
  }
  memset(&job_boot, 0, sizeof job_boot);
  memset(&job_result, 0, sizeof job_result);
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
  else if (midi_core::device_role(d.vid, d.pid) == midi_core::Role::Target) {
    fm1_switchable = midi_core::can_switch_firmware(d.vid, d.pid);  // before the address the key waits on
    fm1_midi_address = d.address;
  }
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

void loader_act(UiAct act, int index) {
  if (busy || !store_ok || !blob_ok || !task) return;
  taskENTER_CRITICAL(&lock);
  const bool can = act == UiAct::Switch   ? shown.plan.can_act && shown.plan.item == index
                   : act == UiAct::Backup ? shown.can_back_up
                   : act == UiAct::Retry  ? shown.can_retry
                                          : false;
  taskEXIT_CRITICAL(&lock);
  if (!can) return;
  act_kind = uint8_t(act);
  act_index = index;
  // The job page opens now, before the loader task names the job's steps.
  taskENTER_CRITICAL(&lock);
  shown.job = UiJob{};
  strlcpy(shown.job.title, "Starting", sizeof shown.job.title);
  taskEXIT_CRITICAL(&lock);
  busy = true;
  xTaskNotify(task, kAct, eSetBits);
}

platform::Mode loader_boot_mode() { return boot_mode; }

bool loader_busy() { return busy.load(); }

#if LOADER_SOAK
void loader_soak_crashed() {
  if (soak.magic == kSoakMagic) ++soak.crashes;
}
#endif

void loader_ui(UiLoader &out) {
  taskENTER_CRITICAL(&lock);
  out = shown;
  taskEXIT_CRITICAL(&lock);
  out.job.active = busy;  // a job's boot, or the hold's restart into one
}

#endif  // APP_LOADER
