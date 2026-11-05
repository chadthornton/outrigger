// Host-side tests for lib/ota: the update record, the recovery app's copy and
// rollback, and Outrigger's boot-time handling, run against a model of the
// board and its bootloader (support/ota_sim.h) with a power cut at every step.
// Run with `make test`.
#include <stdio.h>
#include <string.h>

#include "ota.h"
#include "support/check.h"
#include "support/ota_sim.h"

using namespace ota_sim;

int failures = 0;

// Three builds: what the board runs, the update, and a second update. Sizes are
// odd on purpose (the last sector is partly used).
static Build make_build(const char *name, size_t size, bool confirms = true, bool ota_aware = true) {
  Build b;
  b.name = name;
  b.bytes = synth(name, size);
  b.confirms = confirms;
  b.ota_aware = ota_aware;
  return b;
}

static Board board_running(const Build &old, std::vector<Build> builds) {
  Board s;
  s.builds = builds;
  s.install(old);
  return s;
}

static const Build *running(Board &s) { return s.build_in_app0(); }

// The board is back to normal: app0 boots, confirmed, on its own.
static bool settled_in(Board &s, const Build &b) {
  const int a = s.active();
  return running(s) && running(s)->name == b.name && a >= 0 && (s.od[a].seq - 1) % 2 == 0 &&
         s.od[a].state != kNew && s.od[a].state != kPendingVerify;
}

static void test_record_round_trip() {
  ota::Record r = ota::staged(make_build("n", 5000).image(), make_build("p", 7000).image(), "abc1234");
  r.attempts = 2;
  r.pins_high[0] = 15;
  r.pins_low[1] = 45;
  uint8_t blob[ota::kRecordBytes];
  ota::encode(r, blob);
  ota::Record back;
  CHECK(ota::decode(blob, sizeof blob, back));
  CHECK(back.state == ota::State::Staged);
  CHECK_EQ(int(back.attempts), 2);
  CHECK_EQ(int(back.pins_high[0]), 15);
  CHECK_EQ(int(back.pins_high[1]), -1);
  CHECK_EQ(int(back.pins_low[1]), 45);
  CHECK(ota::same_image(back.next, r.next));
  CHECK(ota::same_image(back.prev, r.prev));
  CHECK_EQ(std::string(back.label), std::string("abc1234"));
  // Any flipped byte, a short blob or a foreign format is no record at all.
  for (size_t i = 0; i < sizeof blob; ++i) {
    uint8_t bad[ota::kRecordBytes];
    memcpy(bad, blob, sizeof bad);
    bad[i] ^= 0x10;
    if (ota::decode(bad, sizeof bad, back)) {
      printf("FAIL decode accepted a flip at byte %zu\n", i);
      ++failures;
    }
  }
  CHECK(!ota::decode(blob, sizeof blob - 1, back));
  // A label longer than the field is cut, still terminated.
  ota::Record l = ota::staged(r.next, r.prev, "0123456789abcdefXYZ");
  CHECK_EQ(strlen(l.label), sizeof l.label - 1);
}

static void test_update_without_a_cut() {
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33);
  Board s = board_running(old, {old, next});
  CHECK(stage(s, next, old));
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, next));
  ota::Record r = s.record();
  CHECK(r.state == ota::State::Idle);
  CHECK(r.result == ota::Result::Updated);
  CHECK_EQ(int(r.attempts), 1);
  CHECK(s.files.empty());
  CHECK_EQ(s.boots.size(), size_t(2));  // recovery, then next
  // And it stays put.
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, next));
  CHECK_EQ(s.boots.back(), std::string("next"));
}

static void test_never_confirming_build_rolls_back_after_three_boots() {
  Build old = make_build("old", 9 * 4096 + 100), bad = make_build("bad", 11 * 4096 + 1, false);
  Board s = board_running(old, {old, bad});
  CHECK(stage(s, bad, old));
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));
  int bad_boots = 0;
  for (const std::string &b : s.boots) bad_boots += b == "bad";
  CHECK_EQ(bad_boots, ota::kMaxTrials);
  ota::Record r = s.record();
  CHECK(r.state == ota::State::Idle);
  CHECK(r.result == ota::Result::RolledBack);
  CHECK(s.files.empty());
}

static void test_rollback_to_a_build_without_lib_ota() {
  // Today's boards run builds from before updates: Arduino confirms them itself.
  Build old = make_build("old", 9 * 4096 + 100, true, false), bad = make_build("bad", 6 * 4096, false);
  Board s = board_running(old, {old, bad});
  CHECK(stage(s, bad, old));
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));
  // The old build doesn't read the record; the recovery app left it RolledBack.
  CHECK(s.record().state == ota::State::RolledBack);
}

static void test_damaged_next_file_never_touches_app0() {
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33);
  Board s = board_running(old, {old, next});
  CHECK(stage(s, next, old));
  s.files[ota::kNextFile][1234] ^= 1;
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));
  CHECK_EQ(s.app0_erases, 0L);
  CHECK(s.record().result == ota::Result::RolledBack);
}

static void test_bad_flash_write_is_copied_again() {
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33);
  Board s = board_running(old, {old, next});
  CHECK(stage(s, next, old));
  s.flip_on_write = 3;
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, next));
  CHECK_EQ(s.app0_erases, 2L);
}

static void test_no_recovery_app_refuses_to_start() {
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33);
  Board s = board_running(old, {old, next});
  s.recovery_installed = false;
  CHECK(!stage(s, next, old));
  CHECK(s.record().result == ota::Result::Abandoned);
  CHECK(s.files.empty());
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));
}

static void test_usb_install_clears_a_stale_record() {
  // An update was mid-trial when someone flashed a third build over USB: the
  // installer rewrites otadata (boot_app0), so app0 boots, and the record
  // must not send the board back to the recovery app or claim an update.
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33, false),
        usb = make_build("usb", 8 * 4096 + 5);
  Board s = board_running(old, {old, next, usb});
  CHECK(stage(s, next, old));
  CHECK(boot_once(s) == End::Restart);  // recovery copies next
  CHECK(s.record().state == ota::State::Trial);
  s.install(usb);
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, usb));
  ota::Record r = s.record();
  CHECK(r.state == ota::State::Idle);
  CHECK(r.result == ota::Result::Abandoned);
  CHECK(s.files.empty());
}

static void test_recovery_with_nothing_pending_boots_app0() {
  Build old = make_build("old", 9 * 4096 + 100);
  Board s = board_running(old, {old});
  CHECK(s.set_boot(1));  // something pointed otadata at the recovery app
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));
}

static void test_damaged_record_is_no_record() {
  Build old = make_build("old", 9 * 4096 + 100), next = make_build("next", 7 * 4096 + 33);
  Board s = board_running(old, {old, next});
  CHECK(stage(s, next, old));
  s.rec_blob[5] ^= 0xFF;
  CHECK(run(s) == End::Settled);
  CHECK(settled_in(s, old));  // nothing copied: a record it can't read starts nothing
}

static void test_plan_boot() {
  ota::Record none;
  ota::BootPlan p = ota::plan_boot(false, none, false, false, false);
  CHECK(!p.trial && !p.write && p.remove_files);

  ota::Record r = ota::staged(make_build("n", 5000).image(), make_build("p", 7000).image(), "n");
  // Staged, yet the old image runs: the recovery app never got to it.
  p = ota::plan_boot(true, r, false, false, true);
  CHECK(!p.trial && p.write && p.remove_files);
  CHECK(p.rec.state == ota::State::Idle && p.rec.result == ota::Result::Abandoned);

  r.state = ota::State::Trial;
  r.attempts = 1;
  p = ota::plan_boot(true, r, true, true, false);
  CHECK(p.trial && !p.write && !p.remove_files);
  // A cut between the confirm and the record's save: the bootloader already has it VALID.
  p = ota::plan_boot(true, r, false, true, false);
  CHECK(!p.trial && p.write && p.remove_files);
  CHECK(p.rec.state == ota::State::Idle && p.rec.result == ota::Result::Updated);

  r.state = ota::State::RolledBack;
  p = ota::plan_boot(true, r, false, false, true);
  CHECK(p.write && p.remove_files && p.rec.state == ota::State::Idle && p.rec.result == ota::Result::RolledBack);

  // A finished record stays as it is, keeping its result for /status.
  ota::Record done = ota::confirmed(r);
  p = ota::plan_boot(true, done, false, false, false);
  CHECK(!p.write && p.remove_files && !p.trial);
}

// Every single power cut, from the first byte of the download's save to the
// confirm. The board must end in a confirmed build: the new one when it works
// and the cut came after the update was handed to the recovery app, otherwise
// the old one; and the record must say which.
static int abandoned_cuts = 0;

static void cut_everywhere(bool next_confirms, bool old_aware) {
  Build old = make_build("old", 9 * 4096 + 100, true, old_aware),
        next = make_build("next", 7 * 4096 + 33, next_confirms);
  long total, staged_steps;
  {
    Board s = board_running(old, {old, next});
    CHECK(stage(s, next, old));
    staged_steps = s.steps;
    CHECK(run(s) == End::Settled);
    total = s.steps;
  }
  CHECK(total > 20);
  for (long cut = 0; cut < total; ++cut) {
    Board s = board_running(old, {old, next});
    s.cut_at = cut;
    bool handed_over = false;
    try {
      handed_over = stage(s, next, old);
      if (handed_over) run(s);  // a cut inside makes run() go on as after a restart
    } catch (PowerCut &) {
    }
    s.cut_at = -1;
    const End e = run(s);
    // The two otadata writes as the recovery app first starts (the bootloader's
    // NEW to PENDING_VERIFY, then its own confirm) can't be torn without the
    // bootloader going back to the old build: that update is abandoned, never lost
    // halfway, and the old build says so.
    const bool handover_window = cut >= staged_steps && cut < staged_steps + 2;
    const bool abandoned = handover_window && settled_in(s, old);
    if (abandoned) {
      ++abandoned_cuts;
      if (old_aware && s.record().result != ota::Result::Abandoned) {
        printf("FAIL cut at step %ld: the old build runs but the record says %s\n", cut,
               ota::result_name(s.record().result));
        ++failures;
      }
      continue;
    }
    const Build &want = (cut >= staged_steps && next_confirms) ? next : old;
    if (e != End::Settled || !settled_in(s, want)) {
      printf("FAIL cut at step %ld/%ld (confirms=%d, old aware=%d): ended %d in %s\n", cut, total, next_confirms,
             old_aware, int(e), running(s) ? running(s)->name.c_str() : "nothing valid");
      ++failures;
      continue;
    }
    ota::Record r = s.record();
    if (!old_aware && want.name == "old") continue;  // a build from before updates never reads the record
    const bool ok = want.name == "next" ? r.result == ota::Result::Updated && r.state == ota::State::Idle
                                        : r.state == ota::State::Idle || !s.have_rec;
    if (!ok || !s.files.empty()) {
      printf("FAIL cut at step %ld: record %s/%s, %zu files left\n", cut, ota::state_name(r.state),
             ota::result_name(r.result), s.files.size());
      ++failures;
    }
  }
}

// Two cuts: one anywhere, then a second anywhere in the boots that follow.
static void cut_twice(bool next_confirms) {
  Build old = make_build("old", 5 * 4096 + 100), next = make_build("next", 4 * 4096 + 33, next_confirms);
  long total;
  {
    Board s = board_running(old, {old, next});
    stage(s, next, old);
    run(s);
    total = s.steps;
  }
  for (long first = 0; first < total; ++first)
    for (long second = first + 1; second < total + 8; ++second) {
      Board s = board_running(old, {old, next});
      s.cut_at = first;
      try {
        if (stage(s, next, old)) run(s);
      } catch (PowerCut &) {
      }
      s.cut_at = second;
      run(s);
      s.cut_at = -1;
      const End e = run(s);
      if (e != End::Settled || !(settled_in(s, next) || settled_in(s, old)) ||
          (!next_confirms && settled_in(s, next))) {
        printf("FAIL cuts at %ld and %ld (confirms=%d): ended %d in %s\n", first, second, next_confirms, int(e),
               running(s) ? running(s)->name.c_str() : "nothing valid");
        ++failures;
        continue;
      }
      // The record names what runs, and nothing is left in the store.
      const ota::Record r = s.record();
      const bool says = settled_in(s, next) ? r.state == ota::State::Idle && r.result == ota::Result::Updated
                                            : r.state == ota::State::Idle && r.result != ota::Result::Updated;
      if (!says || !s.files.empty()) {
        printf("FAIL cuts at %ld and %ld: record %s/%s, %zu files left\n", first, second, ota::state_name(r.state),
               ota::result_name(r.result), s.files.size());
        ++failures;
      }
    }
}

int main() {
  test_record_round_trip();
  test_update_without_a_cut();
  test_never_confirming_build_rolls_back_after_three_boots();
  test_rollback_to_a_build_without_lib_ota();
  test_damaged_next_file_never_touches_app0();
  test_bad_flash_write_is_copied_again();
  test_no_recovery_app_refuses_to_start();
  test_usb_install_clears_a_stale_record();
  test_recovery_with_nothing_pending_boots_app0();
  test_damaged_record_is_no_record();
  test_plan_boot();
  cut_everywhere(true, true);
  cut_everywhere(false, true);
  cut_everywhere(true, false);
  cut_everywhere(false, false);
  CHECK(abandoned_cuts <= 4 * 2);
  cut_twice(true);
  cut_twice(false);
  if (failures) {
    printf("%d failures\n", failures);
    return 1;
  }
  printf("ota: all passed\n");
  return 0;
}
