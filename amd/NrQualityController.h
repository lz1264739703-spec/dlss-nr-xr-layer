// Automatic quality-tier control for the neural pass.
//
// The per-frame network window is a moving part: its GPU cost grows roughly with the window area
// (the fit the probe produced is `net_gpu_ms ~= a + b * area`), so picking a scale is a matter of
// solving that fit backwards for the frame budget. Picking it *again every frame* is the hard part,
// and the answer is deliberately not "react to this frame": a controller that chases single samples
// flaps, and a flapping tier is worse than a slightly too small one.
//
// So there are two layers here:
//
//   * a pure solve, `suggestScaleForBudget`, which is the FrameTime(r) = base + k * r^2 inversion
//     (with a safety margin) snapped down to a discrete rung; and
//   * a stateful sampler, `observe`, which moves the rung only when a run of consecutive samples
//     agrees, so one late frame cannot move it -- and then not again until the frames in between
//     have had time to measure the rung it chose rather than the cost of changing to it.
//
// The second half of that last clause is what `moveDwellMs` is for, and it is not a refinement: a move
// is expensive enough on this hardware (a window resize rebuilds the runtime's working set, ~1.7 s)
// that a controller reacting in 1.5 s never takes a sample that describes a settled tier at all.
//
// Everything is deterministic: the caller supplies the timestamp, the module never reads a clock.
// That is what lets the self test reproduce exact streaks instead of waiting half a second per step.
//
// There is exactly one implementation of the anti-flap curve, and it is this one. The reference we
// took it from wrote the same logic into two files with two different thresholds, and the copies
// drifted; keeping a single sampler class and having every caller go through it is the whole point.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace amdnr {

// Every threshold, rung and interval the controller uses. Defaults are the shipped policy; a caller
// that wants a different ladder or a slower trigger passes an overridden copy to the constructor.
struct NrQualityControllerConfig {
    // The rungs the tier may sit on, descending. `1.0` is the full window and anything smaller is a
    // proportion of it. The list is normalised on construction: values are clamped into
    // [minScale, 1.0], `minScale` and `1.0` are forced in, and duplicates are removed.
    std::vector<double> steps{1.00, 0.90, 0.80, 0.75, 0.66, 0.50};

    // The lowest rung, and the one everything degenerate falls back to. It is a separate number from
    // `steps` because it is also the clamp the pure solve has to know about.
    double minScale{0.50};

    // How often `observe` is allowed to look, in milliseconds. Samples arriving sooner are counted as
    // throttled and leave the state untouched.
    double sampleIntervalMs{500.0};

    // How many consecutive over-budget samples it takes to drop a rung. One sample is never enough:
    // that is the whole anti-flap rule.
    int requiredStreak{3};

    // The same, for climbing back up. It is larger on purpose -- going up too eagerly is what makes
    // the tier oscillate, and the cost of staying low a moment longer is only sharpness.
    int requiredUpStreak{5};

    // How long the tier is frozen after it moves a rung, in milliseconds.
    //
    // This is the one number here that is set by what a move costs the caller rather than by what a
    // frame measures. A move resizes the neural window, and a window resize rebuilds the layer's
    // shared textures and the runtime's fp16 working set; measured on this machine that is 1.6 to
    // 1.7 seconds per change, and it is spent inside the pass, so it lands in the very signal this
    // controller reads. Without a dwell the sequence is fixed: move, take a 1.7 s sample, call it a
    // severe frame, move again -- and the tier walks its whole ladder in a few seconds, rebuilding
    // at every rung, then climbs back and does it again. The dwell is what makes the streak rules
    // above mean anything: it is the only window in which the samples describe the rung the tier is
    // actually sitting on. Samples inside it are counted as throttled.
    double moveDwellMs{5000.0};

    // The dead band around the budget, as a fraction. A sample has to beat `budget * (1 + tolerance)`
    // to count as over and fall below `budget * (1 - tolerance)` to count as under; inside the band
    // the controller does nothing and both runs reset.
    double tolerance{0.10};

    // Below this fraction of the budget the pass is treated as idle (a menu, a paused frame): the runs
    // are reset but the rung is not moved, so a still scene cannot creep the tier up and then be
    // caught out when motion resumes.
    double idleFraction{0.5};

    // A single frame above `budget * hardLimitFactor` is already a visible stutter, so it drops a rung
    // immediately, skipping the streak. This is the only path allowed to do that.
    double hardLimitFactor{2.0};
};

// What the controller has done so far. `streak` is the current run of consecutive over-budget samples
// (the run that would drop a rung if it reached `requiredStreak`).
struct NrQualityStats {
    uint64_t downgrades{0};
    uint64_t upgrades{0};
    uint64_t throttled{0};
    int streak{0};
};

// Owns one quality tier and the anti-flap curve that moves it. Header only, no third party types, no
// clock of its own.
class NrQualityController {
  public:
    explicit NrQualityController(double frameBudgetMs = 1000.0 / 90.0,
                                 const NrQualityControllerConfig& config = {})
        : m_config(config), m_budgetMs(frameBudgetMs) {
        normalizeConfig();
        m_steps = normalizeSteps(m_config.steps, m_config.minScale);
    }

    // The budget can move while the title runs (a refresh rate change), so this is a setter rather
    // than a constructor-only value.
    void setFrameBudget(double frameBudgetMs) {
        m_budgetMs = frameBudgetMs;
    }
    double frameBudget() const {
        return m_budgetMs;
    }

    // The rung currently in force. 1.0 is the full window.
    double currentScale() const {
        return m_steps[static_cast<std::size_t>(m_rung)];
    }

    // Feeds one per-frame sample. `networkMs` is this frame's measured network cost and `nowMs` is the
    // caller's timestamp -- the module never reads a clock, so the sequence is reproducible.
    void observe(double networkMs, double nowMs) {
        // Throttle first: a sample inside the interval is not a new observation at all.
        if (m_hasSample && (nowMs - m_lastSampleMs) < m_config.sampleIntervalMs) {
            m_stats.throttled += 1;
            return;
        }
        m_lastSampleMs = nowMs;
        m_hasSample = true;

        // And the dwell second, for the same reason and with a much longer interval: the frames right
        // after a move are the move's own rebuild, not a measurement of the rung now in force.
        if (m_hasMove && (nowMs - m_lastMoveMs) < m_config.moveDwellMs) {
            m_stats.throttled += 1;
            return;
        }

        // Hard protection. A frame this far past the budget is already stuttering, so it is the one
        // case that acts on a single sample; it also clears the runs because whatever they were
        // counting is no longer representative.
        if (networkMs > m_budgetMs * m_config.hardLimitFactor) {
            dropOneRung(nowMs);
            clearStreaks();
            return;
        }

        // Idle. Far below budget means the pass is barely running, which is not a signal to climb --
        // climbing here is exactly how a still scene ends up at full tier just before it moves.
        if (networkMs < m_budgetMs * m_config.idleFraction) {
            clearStreaks();
            return;
        }

        const double upper = m_budgetMs * (1.0 + m_config.tolerance);
        const double lower = m_budgetMs * (1.0 - m_config.tolerance);

        if (networkMs > upper) {
            m_downStreak += 1;
            m_upStreak = 0;
            if (m_downStreak >= m_config.requiredStreak) {
                dropOneRung(nowMs);
                clearStreaks();
            }
            return;
        }

        if (networkMs < lower) {
            m_upStreak += 1;
            m_downStreak = 0;
            if (m_upStreak >= m_config.requiredUpStreak) {
                raiseOneRung(nowMs);
                clearStreaks();
            }
            return;
        }

        // Inside the dead band: neither direction is clear, so neither run survives.
        clearStreaks();
    }

    // Everything back to the start: full rung, empty counters, no timestamp remembered.
    void reset() {
        m_rung = 0;
        m_hasSample = false;
        m_lastSampleMs = 0.0;
        m_hasMove = false;
        m_lastMoveMs = 0.0;
        clearStreaks();
        m_stats = NrQualityStats{};
    }

    NrQualityStats stats() const {
        NrQualityStats copy = m_stats;
        copy.streak = m_downStreak;
        return copy;
    }

    // The pure solve. From `budget = baseMs + k * r^2` it gives
    //   r = sqrt(max(0, (budget * (1 - safety) - baseMs) / k))
    // clamped to [minScale, 1.0] and snapped *down* to the nearest rung, so the chosen tier never
    // overshoots the budget it was solved for. Degenerate inputs (k <= 0, or a budget that does not
    // clear the fixed cost) have one defined answer: minScale.
    static double suggestScaleForBudget(double budgetMs,
                                        double baseMs,
                                        double k,
                                        double safety,
                                        const NrQualityControllerConfig& config = {}) {
        const double usable = budgetMs * (1.0 - safety) - baseMs;
        if (!std::isfinite(k) || k <= 0.0 || !std::isfinite(usable) || usable <= 0.0) {
            return config.minScale;
        }
        double r = std::sqrt(usable / k);
        if (!std::isfinite(r)) {
            return config.minScale;
        }
        r = std::clamp(r, config.minScale, 1.0);
        return snapDown(r, config);
    }

  private:
    // Descending, de-duplicated, and guaranteed to contain both `minScale` and `1.0`. Building it here
    // once means `snapDown` and the rung stepping share the same ladder definition.
    static std::vector<double> normalizeSteps(const std::vector<double>& steps, double minScale) {
        std::vector<double> out;
        out.reserve(steps.size() + 2);
        for (double s : steps) {
            if (!std::isfinite(s)) {
                continue;
            }
            out.push_back(std::clamp(s, minScale, 1.0));
        }
        out.push_back(minScale);
        out.push_back(1.0);
        std::sort(out.begin(), out.end(), std::greater<double>());
        out.erase(std::unique(out.begin(),
                              out.end(),
                              [](double a, double b) { return std::fabs(a - b) <= 1e-9; }),
                  out.end());
        return out;
    }

    // Largest rung not above `scale`. The ladder is descending, so the first entry that fits is it.
    static double snapDown(double scale, const NrQualityControllerConfig& config) {
        const std::vector<double> steps = normalizeSteps(config.steps, config.minScale);
        for (double s : steps) {
            if (scale >= s) {
                return s;
            }
        }
        return config.minScale;
    }

    void normalizeConfig() {
        if (!std::isfinite(m_config.minScale) || m_config.minScale <= 0.0) {
            m_config.minScale = 0.50;
        }
        m_config.minScale = std::min(m_config.minScale, 1.0);
        if (!std::isfinite(m_config.sampleIntervalMs) || m_config.sampleIntervalMs <= 0.0) {
            m_config.sampleIntervalMs = 500.0;
        }
        if (m_config.requiredStreak < 1) {
            m_config.requiredStreak = 1;
        }
        if (m_config.requiredUpStreak < 1) {
            m_config.requiredUpStreak = 1;
        }
        if (!std::isfinite(m_config.moveDwellMs) || m_config.moveDwellMs < 0.0) {
            m_config.moveDwellMs = 0.0;
        }
        if (!std::isfinite(m_config.tolerance) || m_config.tolerance < 0.0) {
            m_config.tolerance = 0.0;
        }
        if (!std::isfinite(m_config.idleFraction) || m_config.idleFraction < 0.0) {
            m_config.idleFraction = 0.0;
        }
        if (!std::isfinite(m_config.hardLimitFactor) || m_config.hardLimitFactor <= 0.0) {
            m_config.hardLimitFactor = 2.0;
        }
    }

    bool dropOneRung(double nowMs) {
        if (m_rung + 1 >= static_cast<int>(m_steps.size())) {
            return false;
        }
        m_rung += 1;
        m_stats.downgrades += 1;
        noteMove(nowMs);
        return true;
    }

    bool raiseOneRung(double nowMs) {
        if (m_rung <= 0) {
            return false;
        }
        m_rung -= 1;
        m_stats.upgrades += 1;
        noteMove(nowMs);
        return true;
    }

    // Only a move that happened starts a dwell; being already at the floor is not a move.
    void noteMove(double nowMs) {
        m_lastMoveMs = nowMs;
        m_hasMove = true;
    }

    void clearStreaks() {
        m_downStreak = 0;
        m_upStreak = 0;
    }

    NrQualityControllerConfig m_config;
    std::vector<double> m_steps;
    double m_budgetMs{1000.0 / 90.0};
    int m_rung{0};

    bool m_hasSample{false};
    double m_lastSampleMs{0.0};

    bool m_hasMove{false};
    double m_lastMoveMs{0.0};

    int m_downStreak{0};
    int m_upStreak{0};

    NrQualityStats m_stats;
};

}  // namespace amdnr
