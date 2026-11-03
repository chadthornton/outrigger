// Host-side test for lib/board/src/profile_link: the Computer-mode commands
// that read, store and forget the board's profile ('O' 'P' frames).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "profile_link.h"

static int failures = 0;
#define EXPECT(cond, ...)  \
  do {                     \
    if (!(cond)) {         \
      printf(__VA_ARGS__); \
      printf("\n");        \
      ++failures;          \
    }                      \
  } while (0)

static std::vector<uint8_t> from_hex(const std::string &h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
  return out;
}

// The first blob of a named case in tests/boards/blobs.tsv.
static std::vector<uint8_t> case_blob(const char *name) {
  std::ifstream in("tests/boards/blobs.tsv");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream f(line);
    std::string n, hex;
    std::getline(f, n, '\t');
    std::getline(f, hex, '\t');
    if (n == name) return from_hex(hex);
  }
  return {};
}

struct Board {
  std::vector<uint8_t> stored;  // what the settings hold
  bool quad = false;
  ProfileLink link;
  Board()
      : link({[this](uint8_t *out, size_t max) -> size_t {
                if (stored.empty() || stored.size() > max) return 0;
                memcpy(out, stored.data(), stored.size());
                return stored.size();
              },
              [this](const uint8_t *data, size_t len) {
                stored.assign(data, data + len);
                return true;
              },
              [this] { stored.clear(); }, false}) {}
  // Sends one request; returns the reply frames' bytes.
  std::vector<uint8_t> ask(uint8_t type, const std::vector<uint8_t> &payload = {}) {
    const std::vector<uint8_t> frame = profile_frame(type, payload.data(), payload.size());
    std::vector<uint8_t> out;
    link.feed(frame.data(), frame.size(), out);
    return out;
  }
};

// A reply's type and payload, from the one frame in it.
static bool reply(const std::vector<uint8_t> &bytes, uint8_t &type, std::vector<uint8_t> &payload) {
  ProfileFrameReader r;
  for (uint8_t b : bytes)
    if (r.feed(b)) {
      type = r.type();
      payload = r.payload();
      return true;
    }
  return false;
}

int main() {
  const std::vector<uint8_t> good = case_blob("waveshare_35"), usb = case_blob("usb pin"),
                             damaged = case_blob("damaged");
  EXPECT(!good.empty() && !usb.empty() && !damaged.empty(), "blobs.tsv lacks a case this test needs");
  uint8_t t = 0;
  std::vector<uint8_t> p;

  {
    Board b;
    EXPECT(reply(b.ask('G'), t, p) && t == 'g' && p.empty(), "get with nothing stored: an empty 'g'");
    EXPECT(reply(b.ask('S', good), t, p) && t == 's' && b.stored == good, "set a good profile: stored, 's'");
    EXPECT(reply(b.ask('G'), t, p) && t == 'g' && p == good, "get: the stored bytes back");
    EXPECT(reply(b.ask('F'), t, p) && t == 'f' && b.stored.empty(), "forget: cleared, 'f'");
  }
  {
    Board b;
    b.stored = good;
    EXPECT(reply(b.ask('S', usb), t, p) && t == 'e' && !p.empty() && p[0] == uint8_t(BlobError::PinUsb),
           "a profile breaking a rule: 'e' with its code");
    EXPECT(p.size() > 1 && std::string(p.begin() + 1, p.end()).find("GPIO19") != std::string::npos,
           "the refusal says why: %s", std::string(p.begin() + (p.empty() ? 0 : 1), p.end()).c_str());
    EXPECT(b.stored == good, "a refused profile leaves the stored one");
    EXPECT(reply(b.ask('S', damaged), t, p) && t == 'e' && p[0] == uint8_t(BlobError::Crc), "damaged: 'e' crc");
  }
  {
    Board b;
    b.link = ProfileLink({[](uint8_t *, size_t) -> size_t { return 0; }, [](const uint8_t *, size_t) { return true; },
                          [] {}, true});  // a quad-PSRAM build
    EXPECT(reply(b.ask('S', good), t, p) && t == 'e' && p[0] == uint8_t(ProfileLink::kPsramMismatch),
           "an octal profile on a quad build is refused");
  }
  {
    Board b;
    EXPECT(!b.link.restart_due(), "no restart before it's asked for");
    EXPECT(reply(b.ask('B'), t, p) && t == 'b' && b.link.restart_due(), "back to normal start: 'b', then the restart");
    EXPECT(reply(b.ask('Q'), t, p) && t == 'e', "an unknown request: 'e'");
  }
  {
    // Bytes around frames (the board's log, another protocol's 'O' 'R' frames) are skipped.
    Board b;
    std::vector<uint8_t> noise = {'h', 'i', '\n', 'O', 'R', 'H', 0, 0, 0x12, 0x34, 'O'};
    const std::vector<uint8_t> frame = profile_frame('G', nullptr, 0);
    noise.insert(noise.end(), frame.begin(), frame.end());
    std::vector<uint8_t> out;
    b.link.feed(noise.data(), noise.size(), out);
    EXPECT(reply(out, t, p) && t == 'g', "a frame after noise still gets its reply");
  }
  if (failures) return 1;
  printf("profile_link: get, set, forget and restart, and refusals with a reason\n");
  return 0;
}
