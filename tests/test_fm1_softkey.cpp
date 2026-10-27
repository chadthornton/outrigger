// Host-side tests for the ROM-boot key entry, ported from the Unbricker's
// softkey: the identity handshake, then the key as one USB transfer. Vectors
// (tests/golden/crypto.txt) come from fm1_unbrick.py.
#include <string.h>

#include <deque>
#include <map>

#include "softkey.h"
#include "support/check.h"

using namespace fm1_loader;

int failures = 0;

static std::map<std::string, std::vector<uint8_t>> vectors() {
  std::map<std::string, std::vector<uint8_t>> out;
  for (const std::string &line : lines_of(read_text("tests/golden/crypto.txt"))) {
    size_t sp = line.rfind(' ');
    std::string key = line.substr(0, sp), h = line.substr(sp + 1);
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < h.size(); i += 2) bytes.push_back(uint8_t(strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
    out[key] = bytes;
  }
  return out;
}

static std::vector<uint8_t> packets_of(const std::vector<uint8_t> &msg) {
  std::vector<uint8_t> out;
  sysex_packets(msg.data(), msg.size(), out);
  return out;
}

static void test_packets() {
  auto v = vectors();
  // The key is one 8-byte transfer: 04 F0 22 24 07 35 7D F7.
  const std::vector<uint8_t> key = packets_of(v["softkey"]);
  CHECK_EQ(hex(key.data(), key.size()), std::string("04f02224" "07357df7"));
  CHECK(memcmp(key.data(), kKeyTransfer, 8) == 0);
  // The 10-byte identity query ends with a one-byte end packet (CIN 5).
  const std::vector<uint8_t> q = packets_of(v["hs_query"]);
  CHECK_EQ(hex(q.data(), q.size()), std::string("04f00032" "04450000" "0400407f" "05f70000"));
  // Two-byte end (CIN 6).
  const uint8_t two[] = {0xF0, 0x01, 0x02, 0x03, 0xF7};
  std::vector<uint8_t> out;
  sysex_packets(two, sizeof two, out);
  CHECK_EQ(hex(out.data(), out.size()), std::string("04f00102" "0603f700"));
}

static void test_reassembly() {
  auto v = vectors();
  const std::vector<uint8_t> &answer = v["identity FM-1_714"];
  std::vector<uint8_t> pk = packets_of(answer);
  SysexAssembler a;
  std::vector<uint8_t> msg;
  bool done = false;
  for (size_t i = 0; i < pk.size(); i += 4) {
    CHECK(!done);
    done = a.feed(pk[i] & 0x0F, pk[i + 1], pk[i + 2], pk[i + 3], msg);
  }
  CHECK(done && msg == answer);
  // A stray continuation without a start is dropped; a new start restarts.
  SysexAssembler b;
  CHECK(!b.feed(0x4, 0x10, 0x11, 0x12, msg));
  CHECK(!b.feed(0x4, 0xF0, 0x01, 0x02, msg));
  CHECK(!b.feed(0x4, 0xF0, 0x03, 0x04, msg));
  CHECK(b.feed(0x5, 0xF7, 0, 0, msg));
  CHECK_EQ(hex(msg.data(), msg.size()), std::string("f00304f7"));
  // Channel messages in between don't disturb it.
  CHECK(!b.feed(0x4, 0xF0, 0x05, 0x06, msg));
  CHECK(!b.feed(0x9, 0x90, 60, 100, msg));
  CHECK(b.feed(0x6, 0x07, 0xF7, 0, msg));
  CHECK_EQ(hex(msg.data(), msg.size()), std::string("f0050607f7"));
}

static void test_identity() {
  auto v = vectors();
  CHECK_EQ(parse_identity(v["identity FM-1_714"]), std::string("FM-1_714"));
  CHECK_EQ(parse_identity(v["identity FM-1_015"]), std::string("FM-1_015"));
  CHECK_EQ(parse_identity(v["identity ota-FM-1_015"]), std::string("ota-FM-1_015"));
  std::vector<uint8_t> bad = v["identity FM-1_714"];
  bad.pop_back();
  CHECK(parse_identity(bad).empty());
  CHECK(parse_identity(v["hs_query"]).empty());
}

struct FakeLink : MidiLink {
  std::vector<std::vector<uint8_t>> sent;
  std::deque<std::vector<uint8_t>> replies;  // one per query, empty = silence
  int fail_send_from = -1;                    // sends from this index on fail
  uint32_t slept = 0;
  int drains = 0;
  bool send(const uint8_t *packets, size_t len) override {
    if (fail_send_from >= 0 && int(sent.size()) >= fail_send_from) return false;
    sent.emplace_back(packets, packets + len);
    return true;
  }
  bool receive_sysex(std::vector<uint8_t> &msg, uint32_t) override {
    if (replies.empty()) return false;
    msg = replies.front();
    replies.pop_front();
    return !msg.empty();
  }
  void sleep_ms(uint32_t ms) override { slept += ms; }
  void drain() override { ++drains; }
  int keys() const {
    int n = 0;
    for (const auto &s : sent) n += s.size() == 8 && memcmp(s.data(), kKeyTransfer, 8) == 0;
    return n;
  }
};

static void test_key_entry() {
  auto v = vectors();
  const std::vector<uint8_t> query = packets_of(v["hs_query"]);
  {
    FakeLink l;
    l.replies.push_back(v["identity FM-1_714"]);
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::Sent);
    CHECK_EQ(id, std::string("FM-1_714"));
    CHECK_EQ(l.sent.size(), size_t(4));
    CHECK(l.sent[0] == query);
    CHECK_EQ(l.keys(), 3);
    CHECK_EQ(l.slept, uint32_t(40));  // 20 ms between the three sends
    CHECK_EQ(l.drains, 1);
  }
  {
    FakeLink l;  // silence: three queries, never a key
    l.replies = {{}, {}, {}};
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::NoAnswer);
    CHECK_EQ(l.sent.size(), size_t(3));
    CHECK_EQ(l.keys(), 0);
  }
  {
    FakeLink l;  // answers on the third try
    l.replies = {{}, {}, v["identity FM-1_015"]};
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::Sent);
    CHECK_EQ(id, std::string("FM-1_015"));
    CHECK_EQ(l.keys(), 3);
  }
  {
    FakeLink l;  // mid-update: refused
    l.replies.push_back(v["identity ota-FM-1_015"]);
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::UpdateMode);
    CHECK_EQ(l.keys(), 0);
  }
  {
    FakeLink l;  // something else answers
    std::vector<uint8_t> other = v["identity FM-1_714"];
    l.replies.push_back(other);
    l.replies.front()[10] ^= 0x01;  // corrupt the identity text
    std::string id;
    KeyResult r = enter_rom_boot(l, id);
    CHECK(r == KeyResult::NotFm1 || r == KeyResult::NoAnswer);
    CHECK_EQ(l.keys(), 0);
  }
  {
    FakeLink l;  // the unit leaves the bus after the first key: still sent
    l.replies.push_back(v["identity FM-1_714"]);
    l.fail_send_from = 2;
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::Sent);
    CHECK_EQ(l.keys(), 1);
  }
  {
    FakeLink l;  // the first key can't be sent
    l.replies.push_back(v["identity FM-1_714"]);
    l.fail_send_from = 1;
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::SendFailed);
  }
  {
    FakeLink l;  // the query itself can't be sent
    l.fail_send_from = 0;
    std::string id;
    CHECK(enter_rom_boot(l, id) == KeyResult::SendFailed);
    CHECK_EQ(l.keys(), 0);
  }
}

int main() {
  test_packets();
  test_reassembly();
  test_identity();
  test_key_entry();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader softkey tests passed\n");
  return 0;
}
