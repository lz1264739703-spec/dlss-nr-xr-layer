// Self test for NrQualityController.
//
// Runs every rule the controller promises, deterministically: all timestamps are fed in by hand, so
// the streaks are exact rather than timing-dependent. Prints one line per check and a summary, and
// returns non-zero if anything failed.

#include "NrQualityController.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

void check(bool ok, const std::string& name) {
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", name.c_str());
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}

// The shipped policy, with the move dwell switched off. Every test that is about a streak, a rung or
// the floor needs this: with the dwell in force the tier may not move twice inside five seconds, and
// these tests move it every half second on purpose. The dwell itself is tested on the shipped policy.
amdnr::NrQualityControllerConfig noDwellConfig() {
    amdnr::NrQualityControllerConfig cfg;
    cfg.moveDwellMs = 0.0;
    return cfg;
}

// Rule 2: two over-budget samples are not enough, the third is.
void testDowngradeStreak() {
    amdnr::NrQualityController c(10.0);  // budget 10 ms -> over band starts above 11 ms
    c.observe(12.0, 0.0);
    c.observe(12.0, 500.0);
    check(near(c.currentScale(), 1.00), "downgrade: two over-budget samples leave the tier alone");
    check(c.stats().streak == 2, "downgrade: the run counts to two");

    c.observe(12.0, 1000.0);
    check(near(c.currentScale(), 0.90), "downgrade: the third over-budget sample drops one rung");
    check(c.stats().streak == 0, "downgrade: the run clears after acting");
}

// Rule 2: sitting exactly on the budget is inside the dead band, so nothing happens.
void testExactlyAtBudget() {
    amdnr::NrQualityController c(10.0);
    for (int i = 0; i < 10; ++i) {
        c.observe(10.0, i * 500.0);
    }
    check(near(c.currentScale(), 1.00), "at-budget: exactly on budget never drops");
    check(c.stats().downgrades == 0, "at-budget: no downgrade was recorded");
}

// Rule 2: climbing needs the (larger) up streak, and only then moves one rung.
void testUpgradeStreak() {
    amdnr::NrQualityController c(10.0, noDwellConfig());
    // Drop to 0.90 first, so there is somewhere to climb back to.
    c.observe(12.0, 0.0);
    c.observe(12.0, 500.0);
    c.observe(12.0, 1000.0);
    check(near(c.currentScale(), 0.90), "upgrade: setup dropped to 0.90");

    double t = 1500.0;
    for (int i = 0; i < 4; ++i) {
        c.observe(8.0, t);  // 8 ms is below the 9 ms under band
        t += 500.0;
    }
    check(near(c.currentScale(), 0.90), "upgrade: four under-budget samples do not climb");

    c.observe(8.0, t);
    check(near(c.currentScale(), 1.00), "upgrade: the fifth under-budget sample climbs one rung");
}

// Rule 4: one severe frame drops immediately, no streak required, and clears the run.
void testHardLimit() {
    amdnr::NrQualityController c(10.0);
    c.observe(12.0, 0.0);
    check(near(c.currentScale(), 1.00), "hard-limit: one over-budget sample leaves the tier alone");

    c.observe(25.0, 500.0);  // above budget * 2.0
    check(near(c.currentScale(), 0.90), "hard-limit: a single severe frame drops one rung at once");
    check(c.stats().streak == 0, "hard-limit: the run is cleared by the drop");
}

// Rule 2: samples inside the interval are skipped, and the skip is counted.
void testThrottle() {
    amdnr::NrQualityController c(10.0);
    c.observe(10.0, 0.0);
    c.observe(10.0, 100.0);
    c.observe(10.0, 200.0);
    check(c.stats().throttled == 2, "throttle: samples inside the interval are skipped");

    c.observe(10.0, 500.0);
    check(c.stats().throttled == 2, "throttle: the sample on the interval boundary is taken");
}

// Rule 6: the ladder stops at minScale.
void testFloor() {
    amdnr::NrQualityController c(10.0, noDwellConfig());
    double t = 0.0;
    for (int i = 0; i < 8; ++i) {
        c.observe(30.0, t);  // severe every time
        t += 500.0;
    }
    check(near(c.currentScale(), 0.50), "floor: repeated severe frames reach the floor");
    check(c.stats().downgrades == 5, "floor: exactly five rungs to reach 0.50");

    c.observe(30.0, t);
    check(near(c.currentScale(), 0.50), "floor: the tier cannot drop below minScale");
    check(c.stats().downgrades == 5, "floor: no extra downgrade is recorded at the floor");
}

// Rule 1: the geometric solve and its degenerate inputs.
void testGeometry() {
    amdnr::NrQualityControllerConfig cfg;

    // r^2 = (10 * 0.9 - 2) / 8.641975 = 0.81 -> r = 0.90 exactly.
    const double solved = amdnr::NrQualityController::suggestScaleForBudget(10.0, 2.0, 8.641975, 0.10, cfg);
    check(near(solved, 0.90, 0.02), "geometry: solves r = sqrt((budget*(1-safety)-base)/k)");

    // r^2 = (10 * 0.9 - 2) / 10 = 0.7 -> r = 0.8367, which snaps down to 0.80.
    const double snapped = amdnr::NrQualityController::suggestScaleForBudget(10.0, 2.0, 10.0, 0.10, cfg);
    check(near(snapped, 0.80), "geometry: a value between rungs snaps down");

    const double degenerateK = amdnr::NrQualityController::suggestScaleForBudget(10.0, 2.0, 0.0, 0.10, cfg);
    check(near(degenerateK, cfg.minScale), "geometry: k <= 0 falls back to minScale");

    const double degenerateBudget =
        amdnr::NrQualityController::suggestScaleForBudget(2.0, 2.0, 8.0, 0.10, cfg);
    check(near(degenerateBudget, cfg.minScale), "geometry: budget <= base falls back to minScale");
}

// Rule 3: an idle sample resets the run and never moves the tier.
void testIdle() {
    amdnr::NrQualityController c(10.0, noDwellConfig());
    c.observe(25.0, 0.0);  // hard-limit drop to 0.90
    check(near(c.currentScale(), 0.90), "idle: setup dropped to 0.90");

    c.observe(12.0, 500.0);  // over budget, run = 1
    c.observe(4.0, 1000.0);  // below budget * 0.5 -> idle
    c.observe(4.0, 1500.0);
    c.observe(4.0, 2000.0);

    check(near(c.currentScale(), 0.90), "idle: a still scene never climbs");
    check(c.stats().streak == 0, "idle: the over-budget run is reset");
}

// Rule 5: reset() restores the initial state.
void testReset() {
    amdnr::NrQualityController c(10.0, noDwellConfig());
    c.observe(12.0, 0.0);
    c.observe(25.0, 500.0);
    check(near(c.currentScale(), 0.90), "reset: setup moved off full scale");

    c.reset();
    const amdnr::NrQualityStats s = c.stats();
    check(near(c.currentScale(), 1.00), "reset: the tier returns to full scale");
    check(s.downgrades == 0 && s.upgrades == 0 && s.throttled == 0 && s.streak == 0,
          "reset: every counter clears");
}

// The shipped budget against the cost measured on this machine.
//
// The layer's own log for the last real session has the two eye passes at about 12.8 ms each, so
// about 25.6 ms a frame, against a 90 Hz period of 11.11 ms. The budget the layer ships with is
// three quarters of that period -- about 8.33 ms -- and 25.6 ms is well past twice it, so every
// sample takes the hard-limit path and the tier has to walk all the way to the floor and stay.
//
// What the dwell decides is how long that walk takes. A rung change resizes the neural window, and a
// resize makes the runtime rebuild its fp16 working set: the passes around one measured 1.6 to 1.7 s
// in that same log, which is a hundred and ninety times the budget they are being judged against. So
// the shipped policy is one rung per dwell -- five seconds -- and not one rung per 500 ms sample.
//
// The second of those is the shipped behaviour until this dwell existed, and the same log shows what
// it cost: 54 window rebuilds in nine minutes, and the stalls they carry are 92 of those minutes'
// seconds. The tier was not converging on a rung, it was spending its time arriving at rungs.
void testShippedBudgetAgainstMeasuredCost() {
    constexpr double budget = 1000.0 / 90.0 * 0.75;  // about 8.33 ms
    constexpr double dwellMs = 5000.0;               // the shipped moveDwellMs
    amdnr::NrQualityController c(budget);

    // Two seconds of severe frames, sampled every half second: the first sample moves the rung and the
    // rest land inside its dwell.
    double t = 0.0;
    for (int i = 0; i < 5; ++i) {
        c.observe(25.6, t);
        t += 500.0;
    }
    check(near(c.currentScale(), 0.90), "shipped budget: two seconds of severe frames buys one rung");
    check(c.stats().downgrades == 1, "shipped budget: one rung per dwell, not one per sample");

    // The floor is five rungs, so it is five dwells away, not five samples.
    t = dwellMs;
    for (int i = 0; i < 5; ++i) {
        c.observe(25.6, t);
        t += dwellMs;
    }
    check(near(c.currentScale(), 0.50), "shipped budget: a 25.6 ms frame walks the tier to the floor");

    c.observe(25.6, t);
    check(near(c.currentScale(), 0.50), "shipped budget: the floor holds under a still-too-large frame");
}

// A move starts a dwell, and a sample inside it is not an observation at all.
//
// This is the rule that keeps the controller from measuring its own rebuild. The frames right after a
// move cost the rebuild: on this machine they are the 1.6 to 1.7 s passes in the log, and without the
// dwell they are also the most severe samples the controller will ever see, so it drops a rung for
// them -- and the drop costs another second and a half, and produces another severe sample.
void testMoveDwell() {
    amdnr::NrQualityController c(10.0);  // the shipped dwell, five seconds
    // The first move: the third over-budget sample, at t = 1000.
    c.observe(12.0, 0.0);
    c.observe(12.0, 500.0);
    c.observe(12.0, 1000.0);
    check(near(c.currentScale(), 0.90), "dwell: setup dropped one rung");

    // The rebuild frames. Far past the hard limit, so without the dwell each one would drop a rung.
    double t = 1500.0;
    for (int i = 0; i < 6; ++i) {
        c.observe(1700.0, t);
        t += 600.0;
    }
    check(near(c.currentScale(), 0.90), "dwell: a run of rebuild frames cannot move the tier");
    check(c.stats().downgrades == 1, "dwell: and none of them is counted as a downgrade");

    // Past the dwell the ordinary rules are back, so the tier does keep moving -- at the dwell's
    // spacing rather than at the sample interval's.
    t = 1000.0 + 5000.0;  // exactly where the shipped five second dwell ends
    c.observe(12.0, t);
    c.observe(12.0, t + 500.0);
    c.observe(12.0, t + 1000.0);
    check(near(c.currentScale(), 0.80), "dwell: past the dwell the streak rules apply again");
}

// The budget is a fraction of the interval the runtime reports, so the same frame cost has to be
// judged differently at 72 Hz and at 90 Hz.
//
// This is the reason the interval is taken from xrWaitFrame instead of a 90 written into a constant.
// VR misses its interval as a cliff, not as a slope: a tier solved against the wrong interval is
// either trimmed for a budget nobody is enforcing, or spared one that is. 10.0 ms a frame sits
// inside three quarters of a 72 Hz period (10.42 ms) and past three quarters of a 90 Hz one
// (8.33 ms), so that one cost has to hold the top rung on the first and walk down on the second.
void testBudgetFollowsTheHeadset() {
    constexpr double kFraction = 0.75;
    constexpr double kFrameMs = 10.0;
    const double at72 = (1000.0 / 72.0) * kFraction;  // 10.42 ms
    const double at90 = (1000.0 / 90.0) * kFraction;  //  8.33 ms

    amdnr::NrQualityController c(at72);
    double t = 0.0;
    for (int i = 0; i < 8; ++i) {
        c.observe(kFrameMs, t);
        t += 500.0;
    }
    check(near(c.currentScale(), 1.00), "budget: a 10 ms frame holds the top rung on a 72 Hz budget");
    check(c.stats().downgrades == 0, "budget: and nothing was dropped");

    c.setFrameBudget(at90);
    for (int i = 0; i < 3; ++i) {
        c.observe(kFrameMs, t);
        t += 500.0;
    }
    check(c.currentScale() < 1.00, "budget: the same frame walks the tier down once the budget is 90 Hz");
}

}  // namespace

int main() {
    testDowngradeStreak();
    testExactlyAtBudget();
    testUpgradeStreak();
    testHardLimit();
    testThrottle();
    testFloor();
    testGeometry();
    testIdle();
    testReset();
    testShippedBudgetAgainstMeasuredCost();
    testMoveDwell();
    testBudgetFollowsTheHeadset();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("SOME FAILED\n");
    return 1;
}
