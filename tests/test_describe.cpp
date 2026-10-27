// Host-side tests for lib/platform describe.h: configuration descriptor bytes
// to the broker's DeviceDesc. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "describe.h"

using namespace platform;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

// ---------- Descriptor builders ----------
static void config(std::vector<uint8_t> &d) { d = {9, 2, 0, 0, 0, 1, 0, 0x80, 50}; }

static void interface(std::vector<uint8_t> &d, uint8_t number, uint8_t alt, uint8_t endpoints, uint8_t cls,
                      uint8_t subclass, uint8_t protocol) {
  d.insert(d.end(), {9, 4, number, alt, endpoints, cls, subclass, protocol, 0});
  for (uint8_t i = 0; i < endpoints; ++i) d.insert(d.end(), {7, 5, static_cast<uint8_t>(0x81 + i), 2, 64, 0, 0});
}

static void class_specific(std::vector<uint8_t> &d) { d.insert(d.end(), {6, 0x24, 1, 0, 1, 0}); }

static void finish(std::vector<uint8_t> &d) {
  d[2] = d.size() & 0xFF;
  d[3] = d.size() >> 8;
}

// Skiff on Chad's unit: audio control, MIDI, audio streaming (alt 0 empty,
// alt 1 one iso endpoint), CDC control, CDC data.
static std::vector<uint8_t> skiff() {
  std::vector<uint8_t> d;
  config(d);
  interface(d, 0, 0, 0, 1, 1, 0);
  class_specific(d);
  interface(d, 1, 0, 2, 1, 3, 0);
  class_specific(d);
  interface(d, 2, 0, 0, 1, 2, 0);
  interface(d, 2, 1, 1, 1, 2, 0);
  class_specific(d);
  interface(d, 3, 0, 1, 2, 2, 1);
  interface(d, 4, 0, 2, 10, 0, 0);
  finish(d);
  return d;
}

// ---------- Tests ----------

static void test_skiff() {
  const std::vector<uint8_t> d = skiff();
  DeviceDesc out{};
  EXPECT(describe_device(5, 0x1209, 0x0001, false, "FEL", d.data(), d.size(), out), "parses");
  EXPECT(out.address == 5 && out.vid == 0x1209 && out.pid == 0x0001 && !out.is_hub, "identity copied");
  EXPECT(strcmp(out.tag, "FEL") == 0, "tag copied");
  EXPECT(out.interface_count == 5, "five interfaces, alternates merged");
  EXPECT(out.interfaces[1].cls == 1 && out.interfaces[1].subclass == 3 && out.interfaces[1].endpoints == 2,
         "MIDI with two endpoints");
  EXPECT(out.interfaces[2].number == 2 && out.interfaces[2].subclass == 2 && out.interfaces[2].endpoints == 1,
         "audio streaming costs its alt 1 endpoint");
  EXPECT(out.interfaces[4].cls == 10 && out.interfaces[4].endpoints == 2, "CDC data");
  EXPECT(classify(out) == Kind::Fm1, "the broker sees an FM-1");
}

static void test_feeds_the_broker() {
  const std::vector<uint8_t> d = skiff();
  DeviceDesc out{};
  describe_device(5, 0x1209, 0x0001, false, "FEL", d.data(), d.size(), out);
  Broker b(Mode::Play);
  b.attach(out);
  EXPECT(b.should_claim(5, 1) && !b.should_claim(5, 2) && !b.should_claim(5, 4), "only MIDI claimed in Play");
  EXPECT(b.channels_used() == 3, "EP0 + MIDI");
}

static void test_truncated() {
  std::vector<uint8_t> d = skiff();
  d.resize(d.size() - 3);  // the last endpoint descriptor runs past the end
  DeviceDesc out{};
  EXPECT(!describe_device(5, 0x1209, 0x0001, false, "FEL", d.data(), d.size(), out), "refused");
  EXPECT(out.interface_count == 5, "what was read is kept");

  const uint8_t zero_length[] = {9, 2, 11, 0, 1, 1, 0, 0x80, 50, 0, 4};
  EXPECT(!describe_device(5, 1, 1, false, "X", zero_length, sizeof zero_length, out), "zero length can't loop");
  EXPECT(!describe_device(5, 1, 1, false, "X", nullptr, 0, out), "no descriptor");
}

static void test_too_many_interfaces() {
  std::vector<uint8_t> d;
  config(d);
  for (uint8_t i = 0; i < 10; ++i) interface(d, i, 0, 1, 3, 0, 0);
  finish(d);
  DeviceDesc out{};
  EXPECT(describe_device(2, 1, 1, false, "KEY", d.data(), d.size(), out), "parses");
  EXPECT(out.interface_count == kMaxInterfaces, "capped at kMaxInterfaces");
}

static void test_hub_flag() {
  std::vector<uint8_t> d;
  config(d);
  interface(d, 0, 0, 1, 9, 0, 0);
  finish(d);
  DeviceDesc out{};
  describe_device(1, 0x05E3, 0x0610, true, "USB", d.data(), d.size(), out);
  EXPECT(out.is_hub && classify(out) == Kind::Hub, "hub");
}

int main() {
  test_skiff();
  test_feeds_the_broker();
  test_truncated();
  test_too_many_interfaces();
  test_hub_flag();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all describe tests passed\n");
  return 0;
}
