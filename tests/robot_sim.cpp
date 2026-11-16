// The bench robot's core (firmware/bench_robot/robot_core.cpp) on the Mac, with
// a pretend P4 behind its USB port, for tools/robot.py --sim and
// tests/test_robot.py. Time is virtual: it moves only on "#sim advance <us>",
// so an hour of scenario runs in seconds and the same seed gives the same log
// every time.
//
// --robots N puts N robots (0..N-1) on the pretend P4's hub. Each has its own
// core and clock: robot k's clock reads the sim's time plus --offset-us k:US,
// running --drift-ppm k:PPM fast, so robot.py's clock model is exercised. A
// protocol line starting "@k " goes to robot k; a bare line goes to robot 0.
//
// The pretend P4 routes what a robot sends, the way a scenario's routes say:
//   --route R.C:R.C[,notes=LO-HI][,ch=1+2][,types=note+cc+clock+transport+other][,mapch=N][,tr=N]
// (from robot.cable to robot.cable; the filter and map as tools/robot.py's
// passes() and mapped()). The old --route 0:1 means 0.0:0.1, and with no
// --route at all robot 0's cable 0 comes back on its cable 1. Each delivery
// arrives --latency-us plus up to --jitter-us later, in order per destination.
// --p4-clock-bpm B --p4-clock-to R.C (repeatable) makes the P4 send its own
// MIDI clock from the start, each tick up to --p4-clock-jitter-us late.
// When a source unplugs, the P4 releases the notes it was holding on its
// behalf (a note-off for each, at the destination) unless --no-release.
// Faults for the checker's tests: --drop-every N, --dup-every N, --swap-every N
// (the Nth delivery trades places with the next), counted over all deliveries.
// An unplug drops what's in flight to that robot; the host "re-enumerates"
// --reenum-ms after the plug.
//
// stdin: protocol lines, or "#sim advance <us>" / "#sim now".
// stdout: the cores' replies, and "#sim t=<us>" for each #sim line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "robot_core.h"

namespace {

constexpr int kMaxRobots = 4;

struct Route {
  int from_robot = 0, from_cable = 0, to_robot = 0, to_cable = 1;
  int note_lo = -1, note_hi = -1;  // filter on note numbers (note messages only)
  unsigned ch_mask = 0;            // filter on channels 1-16 (bit ch-1); 0 = any
  unsigned type_mask = 0;          // filter on kinds (see kind_bit); 0 = any
  int map_ch = 0;                  // 1-16, 0 = keep
  int transpose = 0;
};

struct Options {
  int robots = 1;
  std::vector<Route> routes;
  int64_t latency_us = 1000, jitter_us = 0, reenum_us = 300000, start_us = 5000000;
  int64_t offset_us[kMaxRobots] = {}, drift_ppm[kMaxRobots] = {};
  long drop_every = 0, dup_every = 0, swap_every = 0;
  double clock_bpm = 0;
  int64_t clock_jitter_us = 0;
  std::vector<std::pair<int, int>> clock_to;
  bool release = true;
  unsigned seed = 1;
};

// The kinds, as tools/robot.py's kind_of().
enum Kind { kNote = 1, kCc = 2, kClock = 4, kTransport = 8, kOther = 16 };
unsigned kind_bit(const uint8_t *m) {
  uint8_t s = m[0];
  if (s < 0xF0) {
    switch (s >> 4) {
      case 0x8: case 0x9: case 0xA: return kNote;
      case 0xB: return kCc;
      default: return kOther;
    }
  }
  if (s == 0xF8) return kClock;
  if (s == 0xFA || s == 0xFB || s == 0xFC) return kTransport;
  return kOther;
}

struct Flight {
  int64_t at;  // sim time
  int robot;
  uint8_t pkt[4];
};

class Hub;

class SimPort : public robot::Port {
 public:
  SimPort(Hub &hub, int index) : hub_(hub), index_(index) {}
  robot::Core *core = nullptr;
  bool send(const uint8_t pkt[4]) override;
  bool mounted() override { return connected_ && mounted_; }
  void set_connected(bool on) override;
  void describe(char *out, size_t n) override {
    snprintf(out, n, "mac=02:00:00:00:00:%02x fw=0000000 sim=1", index_ + 1);
  }
  bool connected_ = true, mounted_ = true;
  int64_t mount_at_ = -1;  // sim time

 private:
  Hub &hub_;
  int index_;
};

class Hub {
 public:
  explicit Hub(const Options &o) : o_(o), rng_(o.seed) {
    for (int k = 0; k < o.robots; ++k) {
      ports_.emplace_back(new SimPort(*this, k));
      cores_.emplace_back(new robot::Core(*ports_[k], sched_[k], 2048, tx_[k], 4096, rx_[k], 4096));
      ports_[k]->core = cores_[k].get();
    }
    if (o.clock_bpm > 0) {
      clock_interval_ns_ = (int64_t)(60e9 / (o.clock_bpm * 24) + 0.5);
      next_tick_ = o.start_us;
    }
    now = o.start_us;
  }
  int64_t now = 0;
  int robots() const { return o_.robots; }
  robot::Core &core(int k) { return *cores_[k]; }

  // Robot k's clock at sim time t, and the first sim time its clock reads r or later.
  int64_t robot_time(int k, int64_t t) const {
    return t + o_.offset_us[k] + t * o_.drift_ppm[k] / 1000000;
  }
  int64_t sim_time(int k, int64_t r) const {
    if (r == INT64_MAX || r == INT64_MIN) return r;
    int64_t g = (int64_t)((double)(r - o_.offset_us[k]) * 1e6 / (1e6 + (double)o_.drift_ppm[k]));
    while (robot_time(k, g) < r) ++g;
    while (g > 0 && robot_time(k, g - 1) >= r) --g;
    return g;
  }

  void from_robot(int k, const uint8_t pkt[4]) {
    int cable = pkt[0] >> 4;
    const uint8_t *m = pkt + 1;
    for (const Route &r : o_.routes) {
      if (r.from_robot != k || r.from_cable != cable || !passes(r, m)) continue;
      uint8_t out[4] = {(uint8_t)(r.to_cable << 4 | (pkt[0] & 0x0F)), m[0], m[1], m[2]};
      map(r, out + 1);
      hold(k, r.to_robot, out);
      deliver(r.to_robot, out);
    }
  }

  void set_connected(int k, bool on) {
    SimPort &p = *ports_[k];
    if (on == p.connected_) return;
    p.connected_ = on;
    if (!on) {
      // What's in flight to it is gone; the P4 lets go of the notes it held for it.
      flights_.erase(std::remove_if(flights_.begin(), flights_.end(), [k](const Flight &f) { return f.robot == k; }),
                     flights_.end());
      for (auto it = held_.begin(); it != held_.end();) {
        int src = it->first[0], dst = it->first[1];
        if (dst == k) {
          it = held_.erase(it);
        } else if (src == k && o_.release) {
          uint8_t off[4] = {(uint8_t)(it->first[2] << 4 | 0x8), (uint8_t)(0x80 | it->first[3]),
                            (uint8_t)it->first[4], 0};
          for (int i = 0; i < it->second; ++i) push(dst, off, now + o_.latency_us);
          it = held_.erase(it);
        } else {
          ++it;
        }
      }
      if (p.mounted_) {
        p.mounted_ = false;
        cores_[k]->mount_changed(false, robot_time(k, now + 1000));
      }
      p.mount_at_ = -1;
    } else {
      p.mount_at_ = now + o_.reenum_us;
    }
  }

  int64_t next_event() const {
    int64_t t = INT64_MAX;
    for (const Flight &f : flights_) t = std::min(t, f.at);
    for (const auto &p : ports_)
      if (p->mount_at_ >= 0) t = std::min(t, p->mount_at_);
    if (clock_interval_ns_) t = std::min(t, tick_at());
    for (int k = 0; k < o_.robots; ++k) {
      int64_t d = cores_[k]->next_due();
      if (d == INT64_MIN) return INT64_MIN;
      t = std::min(t, sim_time(k, d));
    }
    return t;
  }

  void run_due() {
    for (int k = 0; k < o_.robots; ++k) {
      SimPort &p = *ports_[k];
      if (p.mount_at_ >= 0 && p.mount_at_ <= now) {
        p.mount_at_ = -1;
        p.mounted_ = true;
        cores_[k]->mount_changed(true, robot_time(k, now));
      }
    }
    while (clock_interval_ns_ && tick_at() <= now) {
      int64_t at = tick_at();
      ++ticks_;
      next_tick_ = o_.start_us + ticks_ * clock_interval_ns_ / 1000;
      tick_late_ = o_.clock_jitter_us ? (int64_t)(rng_() % (uint64_t)(o_.clock_jitter_us + 1)) : 0;
      uint8_t pkt[4] = {0x0F, 0xF8, 0, 0};
      for (auto &[r, c] : o_.clock_to) {
        if (r >= o_.robots || !ports_[r]->mounted()) continue;
        pkt[0] = (uint8_t)(c << 4 | 0x0F);
        push(r, pkt, at);
      }
    }
    std::stable_sort(flights_.begin(), flights_.end(), [](const Flight &a, const Flight &b) { return a.at < b.at; });
    size_t i = 0;
    for (; i < flights_.size() && flights_[i].at <= now; ++i) {
      const Flight &f = flights_[i];
      if (ports_[f.robot]->mounted()) cores_[f.robot]->received(f.pkt, robot_time(f.robot, f.at));
    }
    flights_.erase(flights_.begin(), flights_.begin() + (long)i);
  }

  void poll_all(robot::Out &out) {
    for (int k = 0; k < o_.robots; ++k) cores_[k]->poll(robot_time(k, now), out);
  }

 private:
  int64_t tick_at() const { return next_tick_ + tick_late_; }
  static bool passes(const Route &r, const uint8_t *m) {
    unsigned kb = kind_bit(m);
    if (r.type_mask && !(r.type_mask & kb)) return false;
    if (r.ch_mask && m[0] < 0xF0 && !(r.ch_mask & (1u << (m[0] & 0x0F)))) return false;
    if (r.note_lo >= 0 && kb == kNote && (m[1] < r.note_lo || m[1] > r.note_hi)) return false;
    return true;
  }
  static void map(const Route &r, uint8_t *m) {
    if (m[0] >= 0xF0) return;
    if (r.map_ch) m[0] = (uint8_t)((m[0] & 0xF0) | (r.map_ch - 1));
    if (r.transpose && kind_bit(m) == kNote) m[1] = (uint8_t)std::max(0, std::min(127, m[1] + r.transpose));
  }
  // Notes the P4 holds down at a destination on a source's behalf.
  void hold(int src, int dst, const uint8_t pkt[4]) {
    uint8_t s = pkt[1];
    if (s >= 0xA0) return;
    std::array<int, 5> key = {src, dst, pkt[0] >> 4, s & 0x0F, pkt[2]};
    if ((s >> 4) == 0x9 && pkt[3] > 0) {
      ++held_[key];
    } else {
      auto it = held_.find(key);
      if (it != held_.end() && --it->second <= 0) held_.erase(it);
    }
  }
  void deliver(int dst, const uint8_t pkt[4]) {
    ++delivered_;
    if (o_.drop_every && delivered_ % o_.drop_every == 0) return;
    int64_t j = o_.jitter_us ? (int64_t)(rng_() % (uint64_t)(o_.jitter_us + 1)) : 0;
    bool pushed = push(dst, pkt, now + o_.latency_us + j);
    if (pushed && o_.dup_every && delivered_ % o_.dup_every == 0) push(dst, pkt, flights_.back().at);
    if (o_.swap_every && delivered_ % o_.swap_every == 0) {
      swap_next_ = true;
    } else if (swap_next_ && flights_.size() >= 2) {
      std::swap(flights_[flights_.size() - 1].pkt, flights_[flights_.size() - 2].pkt);
      swap_next_ = false;
    }
  }
  bool push(int dst, const uint8_t pkt[4], int64_t at) {
    if (dst >= o_.robots || !ports_[dst]->connected_) return false;
    at = std::max(at, last_at_[dst]);  // in order, per destination
    last_at_[dst] = at;
    Flight f = {at, dst, {pkt[0], pkt[1], pkt[2], pkt[3]}};
    flights_.push_back(f);
    return true;
  }

  const Options &o_;
  std::mt19937_64 rng_;
  std::vector<std::unique_ptr<SimPort>> ports_;
  std::vector<std::unique_ptr<robot::Core>> cores_;
  robot::Event sched_[kMaxRobots][2048];
  robot::Record tx_[kMaxRobots][4096], rx_[kMaxRobots][4096];
  std::vector<Flight> flights_;
  std::map<std::array<int, 5>, int> held_;
  int64_t last_at_[kMaxRobots] = {};
  long delivered_ = 0;
  bool swap_next_ = false;
  int64_t clock_interval_ns_ = 0, next_tick_ = 0, ticks_ = 0, tick_late_ = 0;
};

bool SimPort::send(const uint8_t pkt[4]) {
  if (!connected_ || !mounted_) return false;
  hub_.from_robot(index_, pkt);
  return true;
}
void SimPort::set_connected(bool on) { hub_.set_connected(index_, on); }

class StdOut : public robot::Out {
 public:
  void line(const char *s) override {
    fputs(s, stdout);
    fputc('\n', stdout);
  }
};

bool arg_i64(int argc, char **argv, int *i, const char *name, int64_t *v) {
  if (strcmp(argv[*i], name) || *i + 1 >= argc) return false;
  *v = strtoll(argv[++*i], nullptr, 10);
  return true;
}

bool parse_route(const char *s, Route *r) {
  int a, b, c, d, n = 0;
  if (sscanf(s, "%d.%d:%d.%d%n", &a, &b, &c, &d, &n) == 4) {
    *r = Route{};
    r->from_robot = a, r->from_cable = b, r->to_robot = c, r->to_cable = d;
  } else if (sscanf(s, "%d:%d%n", &b, &d, &n) == 2) {
    *r = Route{};
    r->from_cable = b, r->to_cable = d;
  } else {
    return false;
  }
  std::string rest = s + n;
  size_t pos = 0;
  while (pos < rest.size()) {
    if (rest[pos] != ',') return false;
    size_t end = rest.find(',', pos + 1);
    std::string kv = rest.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
    pos = end == std::string::npos ? rest.size() : end;
    size_t eq = kv.find('=');
    if (eq == std::string::npos) return false;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "notes") {
      if (sscanf(v.c_str(), "%d-%d", &r->note_lo, &r->note_hi) != 2) return false;
    } else if (k == "ch" || k == "types") {
      size_t p = 0;
      while (p <= v.size()) {
        size_t q = v.find('+', p);
        std::string item = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (k == "ch") {
          r->ch_mask |= 1u << (atoi(item.c_str()) - 1);
        } else {
          static const std::map<std::string, unsigned> kinds = {
              {"note", kNote}, {"cc", kCc}, {"clock", kClock}, {"transport", kTransport}, {"other", kOther}};
          auto it = kinds.find(item);
          if (it == kinds.end()) return false;
          r->type_mask |= it->second;
        }
        if (q == std::string::npos) break;
        p = q + 1;
      }
    } else if (k == "mapch") {
      r->map_ch = atoi(v.c_str());
    } else if (k == "tr") {
      r->transpose = atoi(v.c_str());
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    int64_t v;
    if (!strcmp(argv[i], "--route") && i + 1 < argc) {
      Route r;
      if (!parse_route(argv[++i], &r)) {
        fprintf(stderr, "robot_sim: bad --route %s\n", argv[i]);
        return 2;
      }
      o.routes.push_back(r);
    } else if (!strcmp(argv[i], "--p4-clock-to") && i + 1 < argc) {
      int r, c;
      if (sscanf(argv[++i], "%d.%d", &r, &c) != 2) return 2;
      o.clock_to.push_back({r, c});
    } else if (!strcmp(argv[i], "--p4-clock-bpm") && i + 1 < argc) {
      o.clock_bpm = atof(argv[++i]);
    } else if ((!strcmp(argv[i], "--offset-us") || !strcmp(argv[i], "--drift-ppm")) && i + 1 < argc) {
      int k;
      long long x;
      bool off = !strcmp(argv[i], "--offset-us");
      if (sscanf(argv[++i], "%d:%lld", &k, &x) != 2 || k < 0 || k >= kMaxRobots) return 2;
      (off ? o.offset_us : o.drift_ppm)[k] = x;
    } else if (!strcmp(argv[i], "--no-release")) {
      o.release = false;
    } else if (arg_i64(argc, argv, &i, "--robots", &v)) {
      if (v < 1 || v > kMaxRobots) return 2;
      o.robots = (int)v;
    } else if (arg_i64(argc, argv, &i, "--latency-us", &o.latency_us) ||
               arg_i64(argc, argv, &i, "--jitter-us", &o.jitter_us) ||
               arg_i64(argc, argv, &i, "--p4-clock-jitter-us", &o.clock_jitter_us) ||
               arg_i64(argc, argv, &i, "--start-us", &o.start_us)) {
    } else if (arg_i64(argc, argv, &i, "--reenum-ms", &v)) {
      o.reenum_us = v * 1000;
    } else if (arg_i64(argc, argv, &i, "--drop-every", &v)) {
      o.drop_every = (long)v;
    } else if (arg_i64(argc, argv, &i, "--dup-every", &v)) {
      o.dup_every = (long)v;
    } else if (arg_i64(argc, argv, &i, "--swap-every", &v)) {
      o.swap_every = (long)v;
    } else if (arg_i64(argc, argv, &i, "--seed", &v)) {
      o.seed = (unsigned)v;
    } else {
      fprintf(stderr, "robot_sim: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (o.routes.empty()) o.routes.push_back(Route{});
  for (const Route &r : o.routes)
    if (r.from_robot >= o.robots || r.to_robot >= o.robots) {
      fprintf(stderr, "robot_sim: a route names a robot past --robots %d\n", o.robots);
      return 2;
    }
  static Hub *hub_mem = nullptr;
  hub_mem = new Hub(o);  // big: the robots' schedules and logs
  Hub &hub = *hub_mem;
  StdOut out;
  printf("# robot_sim ready robots=%d\n", o.robots);
  fflush(stdout);
  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    if (!strncmp(line, "#sim", 4)) {
      long long us = 0;
      if (sscanf(line, "#sim advance %lld", &us) == 1) {
        int64_t end = hub.now + us, last = INT64_MAX;
        for (;;) {
          int64_t t = hub.next_event();
          if (t == INT64_MIN) t = hub.now;  // a drain: run it now
          if (t > end) break;
          // Work due now that poll can't clear would spin: step a microsecond.
          hub.now = t > hub.now ? t : (t == last ? hub.now + 1 : hub.now);
          last = t;
          hub.run_due();
          hub.poll_all(out);
        }
        hub.now = end;
        hub.run_due();
        hub.poll_all(out);
      }
      printf("#sim t=%lld\n", (long long)hub.now);
    } else {
      int k = 0;
      char *p = line;
      if (line[0] == '@') {
        k = atoi(line + 1);
        p = strchr(line, ' ');
        p = p ? p + 1 : line + strlen(line);
        if (k < 0 || k >= hub.robots()) {
          printf("err robot\n");
          fflush(stdout);
          continue;
        }
      }
      robot::Core &core = hub.core(k);
      int64_t rt = hub.robot_time(k, hub.now);
      for (char *c = p; *c; ++c) core.feed(*c, rt, out);
      core.poll(rt, out);
      // A drain finishes before the next line, as it would between robot.py's reads.
      while (core.next_due() == INT64_MIN) core.poll(rt, out);
    }
    fflush(stdout);
  }
  return 0;
}
