// Host-side tests for fm1_loader's Receiver: the Computer mode protocol that
// takes a Firmware Hub package from the browser and stores it with
// Library::add_package. The page's side runs against the same code through
// build/receive_pipe (tests/installer/add.test.mjs).
#include <string.h>

#include <map>

#include "library.h"
#include "receive.h"
#include "support/check.h"

using namespace fm1_loader;

int failures = 0;

using Bytes = std::vector<uint8_t>;

struct Reply {
  uint8_t type = 0;
  Bytes payload;
};

// Every complete frame in the board's output, in order; junk between them is
// skipped, as the page skips the board's log lines.
static std::vector<Reply> replies(const Bytes &out) {
  std::vector<Reply> got;
  FrameReader r;
  for (uint8_t b : out)
    if (r.feed(b)) got.push_back({r.type(), r.payload()});
  return got;
}

static uint32_t u32(const Bytes &p, size_t at) {
  return p[at] | p[at + 1] << 8 | p[at + 2] << 16 | uint32_t(p[at + 3]) << 24;
}

static void put32(Bytes &p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p.push_back(uint8_t(v >> (8 * i)));
}

static std::string text(const Bytes &p, size_t at) { return std::string(p.begin() + at, p.end()); }

struct Board {
  MemoryStorage storage;
  Library lib{storage};
  uint64_t free_bytes = 8u << 20;
  Receiver rx{lib, [this] { return free_bytes; }, "abc1234"};
  uint32_t now = 1000;
  std::vector<ReceiveStatus> statuses;
  Board() {
    rx.on_status([this](const ReceiveStatus &s) { statuses.push_back(s); });
  }

  std::vector<Reply> send(uint8_t type, const Bytes &payload = {}) {
    Bytes in = encode_frame(type, payload.data(), payload.size()), out;
    rx.feed(in.data(), in.size(), now, out);
    return replies(out);
  }
  // One request, one reply.
  Reply ask(uint8_t type, const Bytes &payload = {}) {
    std::vector<Reply> r = send(type, payload);
    CHECK_EQ(r.size(), size_t(1));
    return r.empty() ? Reply() : r[0];
  }
};

static Bytes begin_payload(const Bytes &file, const std::string &project, const std::string &version,
                           const std::string &name) {
  Bytes p;
  put32(p, uint32_t(file.size()));
  const Hash h = sha256(file.data(), file.size());
  p.insert(p.end(), h.begin(), h.end());
  for (const std::string &s : {project, version, name}) {
    p.insert(p.end(), s.begin(), s.end());
    p.push_back(0);
  }
  return p;
}

// Sends a whole package the way the page does: begin, then data frames from
// wherever the board's ack says it is. Returns the last reply.
static Reply send_package(Board &b, const Bytes &file, const std::string &project, const std::string &version,
                          const std::string &name, size_t chunk = kDataMax) {
  Reply r = b.ask('A', begin_payload(file, project, version, name));
  while (r.type == 'k') {
    const uint32_t at = u32(r.payload, 0);
    Bytes p;
    put32(p, at);
    const size_t n = std::min(chunk, file.size() - at);
    p.insert(p.end(), file.begin() + at, file.begin() + at + n);
    r = b.ask('D', p);
  }
  return r;
}

static Bytes fixture(const char *name) { return read_file(std::string("tests/fixtures/") + name); }

static void test_hello_on_an_empty_store() {
  Board b;
  Reply r = b.ask('H');
  CHECK_EQ(r.type, uint8_t('h'));
  CHECK_EQ(r.payload.size() >= 7, true);
  CHECK_EQ(r.payload[0], kReceiveProtocol);
  CHECK_EQ(r.payload[1], uint8_t(0));  // packages stored
  CHECK_EQ(r.payload[2], uint8_t(kMaxPackages));
  CHECK_EQ(u32(r.payload, 3), uint32_t(8u << 20));
  CHECK_EQ(text(r.payload, 7), std::string("abc1234"));
}

static void test_adds_a_hub_package() {
  Board b;
  const Bytes file = fixture("felucca-1.8.0.fwsc");
  Reply r = send_package(b, file, "felucca", "1.8.0", "Felucca");
  CHECK_EQ(r.type, uint8_t('d'));
  CHECK_EQ(r.payload, Bytes{0});
  // Named as tools/store_add.py names catalog packages, so a Mac add and a
  // browser add of one file are the same package.
  PackageInfo p;
  CHECK(b.lib.package("felucca-1.8.0", p));
  CHECK_EQ(p.name, std::string("Felucca"));
  CHECK_EQ(p.version, std::string("1.8.0"));
  CHECK_EQ(p.project, std::string("felucca"));
  CHECK_EQ(p.identity, std::string("FM-1_918"));
  CHECK(p.file_sha256 == sha256(file.data(), file.size()));
  CHECK_EQ(b.ask('H').payload[1], uint8_t(1));
  CHECK(!b.statuses.empty() && b.statuses.back().state == ReceiveStatus::State::Added);
  CHECK(!b.statuses.empty() && b.statuses.back().label == "Felucca 1.8.0");
  // The list names it with its file hash, so the page can mark it as there.
  Reply l = b.ask('L', {0});
  CHECK_EQ(l.type, uint8_t('l'));
  CHECK_EQ(l.payload.size(), size_t(1 + 32 + 13));
  CHECK(Bytes(l.payload.begin() + 1, l.payload.begin() + 33) == Bytes(p.file_sha256.begin(), p.file_sha256.end()));
  CHECK_EQ(text(l.payload, 33), std::string("felucca-1.8.0"));
  CHECK_EQ(b.ask('L', {1}).type, uint8_t('e'));
}

static ReceiveError error_of(const Reply &r) {
  CHECK_EQ(r.type, uint8_t('e'));
  return r.payload.empty() ? ReceiveError::None : ReceiveError(r.payload[0]);
}

// Every key and value in the store.
static std::map<std::string, Bytes> snapshot(MemoryStorage &st) {
  std::map<std::string, Bytes> out;
  for (const std::string &k : st.list("")) CHECK(st.read(k, out[k]));
  return out;
}

static void test_refuses_before_any_data() {
  const Bytes file = fixture("felucca-1.8.0.fwsc");
  {  // Built on Melodee, unreviewed: it may write the unit-ID sector (decide_data blocks it too).
    Board b;
    CHECK(error_of(b.ask('A', begin_payload(file, "orange-cat", "1", "Orange Cat"))) == ReceiveError::Blocked);
    CHECK(error_of(b.ask('D', {0, 0, 0, 0, 1})) == ReceiveError::NotReceiving);
    CHECK(b.lib.package_ids().empty());
  }
  {  // Melodee itself is riskier, not blocked: the board takes it (and warns at install).
    Board b;
    const Reply r = b.ask('A', begin_payload(file, "melodee", "1.6.0", "Melodee"));
    CHECK(r.type != uint8_t('e'));
  }
  {  // Files of the tester's own aren't taken yet: only catalog packages.
    Board b;
    CHECK(error_of(b.ask('A', begin_payload(file, "", "", "mine"))) == ReceiveError::NoProject);
  }
  {  // A name that isn't a store key.
    Board b;
    CHECK(error_of(b.ask('A', begin_payload(file, "fe/lucca", "1.8.0", "Felucca"))) == ReceiveError::BadRequest);
  }
  {
    Board b;
    CHECK(error_of(b.ask('A', begin_payload(Bytes(), "felucca", "1.8.0", "Felucca"))) == ReceiveError::TooBig);
    CHECK(error_of(b.ask('A', begin_payload(Bytes(kPackageMax + 1), "felucca", "1.8.0", "Felucca"))) ==
          ReceiveError::TooBig);
  }
  {  // Room for the package and a whole backup after it, or no add.
    Board b;
    b.free_bytes = kBackupReserve + kPackageRoom - 1;
    CHECK(error_of(b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca"))) == ReceiveError::StoreFull);
    // The board's screen says so too, not just the page.
    CHECK(!b.statuses.empty() && b.statuses.back().state == ReceiveStatus::State::Failed);
    b.free_bytes = kBackupReserve + kPackageRoom;
    CHECK_EQ(b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca")).type, uint8_t('k'));
  }
}

static void test_already_stored_and_name_taken() {
  Board b;
  const Bytes felucca = fixture("felucca-1.8.0.fwsc"), skiff = fixture("skiff-2.1.1.fwsc");
  CHECK(b.lib.add_package("felucca-1.8.0", felucca.data(), felucca.size(), nullptr, "Felucca", "1.8.0", "felucca") ==
        PackageError::None);
  const auto before = snapshot(b.storage);
  // The same file: nothing to send, nothing written.
  Reply r = b.ask('A', begin_payload(felucca, "felucca", "1.8.0", "Felucca"));
  CHECK_EQ(r.type, uint8_t('d'));
  CHECK_EQ(r.payload, Bytes{1});
  // The same file the Mac stored under another name is there too.
  CHECK_EQ(b.ask('A', begin_payload(felucca, "felucca", "1.8", "Felucca")).payload, Bytes{1});
  // Another file under a stored name is never written over it.
  CHECK(error_of(b.ask('A', begin_payload(skiff, "felucca", "1.8.0", "Felucca"))) == ReceiveError::IdTaken);
  CHECK(snapshot(b.storage) == before);
}

static void test_refuses_past_the_firmware_page_rows() {
  Board b;
  const Bytes felucca = fixture("felucca-1.8.0.fwsc"), skiff = fixture("skiff-2.1.1.fwsc");
  for (size_t i = 0; i < kMaxPackages; ++i)
    CHECK(b.lib.add_package("p" + std::to_string(i), felucca.data(), felucca.size()) == PackageError::None);
  CHECK(error_of(b.ask('A', begin_payload(skiff, "skiff", "2.1.1", "Skiff"))) == ReceiveError::TooMany);
}

static void test_damaged_or_foreign_files_are_not_stored() {
  const Bytes file = fixture("skiff-2.1.1.fwsc");
  {  // The bytes that arrive aren't the ones the page checked.
    Board b;
    Bytes damaged = file;
    damaged[5000] ^= 1;
    Reply r = b.ask('A', begin_payload(file, "skiff", "2.1.1", "Skiff"));
    for (uint32_t at = 0; r.type == 'k' && at < damaged.size(); at = u32(r.payload, 0)) {
      Bytes p;
      put32(p, at);
      p.insert(p.end(), damaged.begin() + at, damaged.begin() + std::min(damaged.size(), at + kDataMax));
      r = b.ask('D', p);
    }
    CHECK(error_of(r) == ReceiveError::HashMismatch);
    CHECK(b.lib.package_ids().empty());
    CHECK(b.statuses.back().state == ReceiveStatus::State::Failed);
  }
  {  // Intact, but not an FM-1 package.
    Board b;
    Bytes junk(600000);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = uint8_t(i * 31 + (i >> 9));
    CHECK(error_of(send_package(b, junk, "skiff", "2.1.1", "Skiff")) == ReceiveError::BadPackage);
    CHECK(b.lib.package_ids().empty());
    CHECK_EQ(b.lib.sectors().count(), size_t(0));
  }
}

static void test_only_adds() {
  // A store with a backup and another package: the add writes only the new
  // package's manifest and sectors, and every existing key stays byte for byte.
  Board b;
  const Bytes felucca = fixture("felucca-1.8.0.fwsc"), skiff = fixture("skiff-2.1.1.fwsc");
  CHECK(b.lib.add_package("felucca-1.8.0", felucca.data(), felucca.size(), nullptr, "Felucca", "1.8.0", "felucca") ==
        PackageError::None);
  Bytes img(kFlashSize, 0xFF);
  for (size_t i = 0; i < kAppLen; ++i) img[kAppStart + i] = uint8_t(i * 7);
  for (size_t i = 0; i < kSector; ++i) img[kUnitSector + i] = uint8_t(i + 3);
  FlashHashes flash(kSectors);
  for (uint32_t i = 0; i < kSectors; ++i) CHECK(b.lib.sectors().put(img.data() + i * kSector, flash[i]));
  Unit u;
  u.id = unit_id_of(img.data() + kUnitSector);
  std::string backup_id;
  CHECK(b.lib.add_backup(u, flash, backup_id));
  const auto before = snapshot(b.storage);
  CHECK_EQ(send_package(b, skiff, "skiff", "2.1.1", "Skiff").type, uint8_t('d'));
  const auto after = snapshot(b.storage);
  for (const auto &kv : before) CHECK(after.count(kv.first) && after.at(kv.first) == kv.second);
  for (const auto &kv : after)
    if (!before.count(kv.first)) CHECK(kv.first == "m/pkg/skiff-2.1.1" || kv.first.compare(0, 2, "s/") == 0);
}

static void test_store_failure_mid_add() {
  // The store refuses a write partway through (it filled up, or the flash
  // failed), then the power goes: what was there still reads, and a later
  // add of the same file works.
  Board b;
  const Bytes felucca = fixture("felucca-1.8.0.fwsc"), skiff = fixture("skiff-2.1.1.fwsc");
  CHECK(b.lib.add_package("felucca-1.8.0", felucca.data(), felucca.size(), nullptr, "Felucca", "1.8.0", "felucca") ==
        PackageError::None);
  b.storage.fail_writes_after = 20;
  CHECK(error_of(send_package(b, skiff, "skiff", "2.1.1", "Skiff")) == ReceiveError::StoreFailed);
  b.storage.lose_unsynced();
  b.storage.fail_writes_after = -1;
  PackageInfo p;
  CHECK(b.lib.package("felucca-1.8.0", p));
  CHECK(!b.lib.package("skiff-2.1.1", p));
  CHECK_EQ(send_package(b, skiff, "skiff", "2.1.1", "Skiff").type, uint8_t('d'));
  CHECK(b.lib.package("skiff-2.1.1", p));
}

static void test_transport() {
  const Bytes file = fixture("felucca-1.8.0.fwsc");
  {  // The board's log lines between frames are skipped.
    Board b;
    Bytes in = {'I', ' ', '(', '1', ')', ' ', 'O', 'R', 'x', '\n'}, out;
    const Bytes f = encode_frame('H', nullptr, 0);
    in.insert(in.end(), f.begin(), f.end());
    b.rx.feed(in.data(), in.size(), b.now, out);
    CHECK_EQ(replies(out).size(), size_t(1));
  }
  {  // A frame whose CRC fails gets no reply: the page resends.
    Board b;
    Bytes f = encode_frame('H', nullptr, 0), out;
    f[2] = 'L';
    b.rx.feed(f.data(), f.size(), b.now, out);
    CHECK(out.empty());
  }
  {  // Pieces in any size, and a repeated frame, land once.
    Board b;
    Reply r = b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca"));
    Bytes p;
    put32(p, 0);
    p.insert(p.end(), file.begin(), file.begin() + 100);
    CHECK_EQ(u32(b.ask('D', p).payload, 0), uint32_t(100));
    CHECK_EQ(u32(b.ask('D', p).payload, 0), uint32_t(100));
    CHECK_EQ(send_package(b, file, "felucca", "1.8.0", "Felucca", 777).type, uint8_t('d'));
  }
  {  // The last reply was lost: the repeated last frame gets it again.
    Board b;
    Reply r = b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca"));
    Bytes last;
    while (r.type == 'k') {
      const uint32_t at = u32(r.payload, 0);
      last.clear();
      put32(last, at);
      last.insert(last.end(), file.begin() + at, file.begin() + std::min(file.size(), at + kDataMax));
      r = b.ask('D', last);
    }
    CHECK_EQ(r.type, uint8_t('d'));
    Reply again = b.ask('D', last);
    CHECK_EQ(again.type, uint8_t('d'));
    CHECK_EQ(again.payload, Bytes{0});
  }
  {  // A transfer that goes quiet is dropped.
    Board b;
    CHECK_EQ(b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca")).type, uint8_t('k'));
    b.rx.tick(b.now + kIdleMs + 1);
    CHECK(b.statuses.back().state == ReceiveStatus::State::Failed);
    CHECK(error_of(b.ask('D', {0, 0, 0, 0, 1})) == ReceiveError::NotReceiving);
  }
  {  // Abort.
    Board b;
    CHECK_EQ(b.ask('A', begin_payload(file, "felucca", "1.8.0", "Felucca")).type, uint8_t('k'));
    CHECK_EQ(b.ask('X').type, uint8_t('k'));
    CHECK(error_of(b.ask('D', {0, 0, 0, 0, 1})) == ReceiveError::NotReceiving);
  }
}

int main() {
  test_hello_on_an_empty_store();
  test_adds_a_hub_package();
  test_refuses_before_any_data();
  test_already_stored_and_name_taken();
  test_refuses_past_the_firmware_page_rows();
  test_damaged_or_foreign_files_are_not_stored();
  test_only_adds();
  test_store_failure_mid_add();
  test_transport();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("test_fm1_receive: ok\n");
  return 0;
}
