// The bench robot's core (firmware/bench_robot/robot_core.cpp) on the Mac, with
// a pretend P4 behind its USB port, for tools/robot.py --sim and
// tests/test_robot.py. Time is virtual: it moves only on "#sim advance <us>",
// so a ten-second scenario runs in milliseconds and the same seed gives the
// same log every time.
//
// The pretend P4 routes each packet sent on one cable back to the robot on
// another (--route 0:1), after --latency-us plus up to --jitter-us, in order.
// Faults for the checker's tests: --drop-every N, --dup-every N, --swap-every N
// (the Nth delivery trades places with the next). An unplug drops what's in
// flight; the host "re-enumerates" --reenum-ms after the plug.
//
// stdin: protocol lines, or "#sim advance <us>" / "#sim now".
// stdout: the core's replies, and "#sim t=<us>" for each #sim line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "robot_core.h"

namespace {

struct Flight {
  int64_t at;
  uint8_t pkt[4];
};

struct Options {
  int route_from = 0, route_to = 1;
  int64_t latency_us = 1000, jitter_us = 0, reenum_us = 300000, start_us = 5000000;
  long drop_every = 0, dup_every = 0, swap_every = 0;
  unsigned seed = 1;
};

class SimPort : public robot::Port {
 public:
  explicit SimPort(const Options &o) : o_(o), rng_(o.seed) {}
  int64_t now = 0;
  robot::Core *core = nullptr;

  bool send(const uint8_t pkt[4]) override {
    if (!connected_ || !mounted_) return false;
    int cable = pkt[0] >> 4;
    if (cable != o_.route_from) return true;  // the pretend P4 has no route for it
    ++delivered_;
    if (o_.drop_every && delivered_ % o_.drop_every == 0) return true;
    int64_t j = o_.jitter_us ? (int64_t)(rng_() % (uint64_t)(o_.jitter_us + 1)) : 0;
    int64_t at = std::max(now + o_.latency_us + j, last_at_);
    last_at_ = at;
    Flight f = {at, {(uint8_t)(o_.route_to << 4 | (pkt[0] & 0x0F)), pkt[1], pkt[2], pkt[3]}};
    flights_.push_back(f);
    if (o_.dup_every && delivered_ % o_.dup_every == 0) flights_.push_back(f);
    if (o_.swap_every && delivered_ % o_.swap_every == 0) swap_next_ = true;
    else if (swap_next_ && flights_.size() >= 2) {
      std::swap(flights_[flights_.size() - 1].pkt, flights_[flights_.size() - 2].pkt);
      swap_next_ = false;
    }
    return true;
  }
  bool mounted() override { return connected_ && mounted_; }
  void set_connected(bool on) override {
    if (on == connected_) return;
    connected_ = on;
    if (!on) {
      flights_.clear();
      if (mounted_) {
        mounted_ = false;
        core->mount_changed(false, now + 1000);
      }
      mount_at_ = -1;
    } else {
      mount_at_ = now + o_.reenum_us;
    }
  }
  void describe(char *out, size_t n) override { snprintf(out, n, "mac=02:00:00:00:00:01 fw=0000000 sim=1"); }

  int64_t next_event() const {
    int64_t t = INT64_MAX;
    if (!flights_.empty()) t = flights_.front().at;
    if (mount_at_ >= 0 && mount_at_ < t) t = mount_at_;
    return t;
  }
  void run_due() {
    if (mount_at_ >= 0 && mount_at_ <= now) {
      mount_at_ = -1;
      mounted_ = true;
      core->mount_changed(true, now);
    }
    size_t i = 0;
    for (; i < flights_.size() && flights_[i].at <= now; ++i) core->received(flights_[i].pkt, flights_[i].at);
    flights_.erase(flights_.begin(), flights_.begin() + (long)i);
  }

 private:
  const Options &o_;
  std::mt19937_64 rng_;
  std::vector<Flight> flights_;
  bool connected_ = true, mounted_ = true, swap_next_ = false;
  int64_t mount_at_ = -1, last_at_ = 0;
  long delivered_ = 0;
};

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

}  // namespace

int main(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    int64_t v;
    if (!strcmp(argv[i], "--route") && i + 1 < argc) {
      if (sscanf(argv[++i], "%d:%d", &o.route_from, &o.route_to) != 2) return 2;
    } else if (arg_i64(argc, argv, &i, "--latency-us", &o.latency_us) ||
               arg_i64(argc, argv, &i, "--jitter-us", &o.jitter_us) ||
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
  static robot::Event sched[2048];
  static robot::Record tx[4096], rx[4096];
  SimPort port(o);
  robot::Core core(port, sched, 2048, tx, 4096, rx, 4096);
  port.core = &core;
  port.now = o.start_us;
  StdOut out;
  printf("# robot_sim ready\n");
  fflush(stdout);
  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    if (!strncmp(line, "#sim", 4)) {
      long long us = 0;
      if (sscanf(line, "#sim advance %lld", &us) == 1) {
        int64_t end = port.now + us, last = INT64_MAX;
        for (;;) {
          int64_t t = std::min(core.next_due(), port.next_event());
          if (t == INT64_MIN) t = port.now;  // a drain: run it now
          if (t > end) break;
          // Work due now that poll can't clear would spin: step a microsecond.
          port.now = t > port.now ? t : (t == last ? port.now + 1 : port.now);
          last = t;
          port.run_due();
          core.poll(port.now, out);
        }
        port.now = end;
        port.run_due();
        core.poll(port.now, out);
      }
      printf("#sim t=%lld\n", (long long)port.now);
    } else {
      for (char *c = line; *c; ++c) core.feed(*c, port.now, out);
      core.poll(port.now, out);
      // A drain finishes before the next line, as it would between robot.py's reads.
      while (core.next_due() == INT64_MIN) core.poll(port.now, out);
    }
    fflush(stdout);
  }
  return 0;
}
