// Adaptive display rate: the pure decision rule (no windows, no clock, no threads), so it can be unit-tested.
//
// The interface thread measures what the live updates cost it (drawing the position bar and time, the statistics
// block, the rest) over a short window of time. This rule turns those measurements into a "tier": how much of the
// live display to give up so that drawing never takes more than a set share of the interface thread's time, and so
// that drawing backs off when the playback worker is behind. Tiers, cheapest visible loss first:
//   0  everything at the screen's rate (only values that changed are redrawn: always on, not a tier)
//   1  position bar and time at full rate; the statistics block is drawn half on one update, half on the next
//   2+ the update rate is lowered as well: 60, 30, 15, 10 per second (a rate not below the screen's own is skipped)
// The ladder only ever goes one rung per decision. Stepping down needs the cost to be over the budget for two windows
// in a row (or far over it once); stepping up needs the cost PREDICTED at the rung above (from what was measured at
// this one) to be well under the budget for several windows in a row, so it never flickers between two tiers.
// Nothing here needs more than Windows 98 SE / XP: plain integer and double arithmetic.
#pragma once

#include <cstdint>

namespace midibn {
namespace app {

struct DisplayLadder {
    enum { MaxTiers = 8 };
    int count = 0;
    int rate_hz[MaxTiers] = {};       // update rate cap of the tier; 0 = the screen's own rate
    bool half_statistics[MaxTiers] = {};
};

// The ladder for a screen/timer rate of nativeHz (<= 0 is read as 60).
inline DisplayLadder make_display_ladder(int nativeHz)
{
    if (nativeHz <= 0) nativeHz = 60;
    DisplayLadder l;
    l.count = 0;
    l.rate_hz[l.count] = 0; l.half_statistics[l.count] = false; ++l.count;
    l.rate_hz[l.count] = 0; l.half_statistics[l.count] = true; ++l.count;
    static const int rates[4] = { 60, 30, 15, 10 };
    for (int i = 0; i < 4; ++i)
        if (rates[i] * 10 < nativeHz * 9) {   // a rung must be clearly below the screen's rate (59.94 Hz is "60")
            l.rate_hz[l.count] = rates[i];
            l.half_statistics[l.count] = true;
            ++l.count;
        }
    return l;
}

// Rate (updates per second) that a tier delivers when the screen runs at nativeHz.
inline int display_tier_rate(const DisplayLadder& l, int tier, int nativeHz)
{
    if (nativeHz <= 0) nativeHz = 60;
    int cap = l.rate_hz[tier];
    return cap > 0 && cap < nativeHz ? cap : nativeHz;
}

struct DisplayTierParams {
    double budget = 0.08;          // share of the interface thread's time that drawing may take
    double release = 0.6;          // step up only when the predicted share at the rung above is below budget * release
    double far_over = 2.0;         // a share above budget * far_over steps down at once (one window)
    int over_windows = 2;          // windows over the budget in a row that step down
    int calm_windows = 6;          // windows in a row (predicted under budget * release) that step up
    double statistics_half = 0.5;  // share of the statistics cost a half-statistics update is assumed to keep
    int64_t behind_lag_us = 20000; // playback "behind" when its current lag is at least this
    bool behind_backoff = true;    // drawing backs off while playback is behind (the player enables it only with 1-2
                                   // processors: with more, a late output is the device's limit, not processor time)
};

// The budget (share of the interface thread's time) for a PC with `processors` logical processors the player may use.
// Drawing competes with playback only when processors are scarce: with 1 processor everything shares it (8 %); with 2
// the playback worker and the interface thread take one each and the driver/synthesizer needs room (12 %); with 3 or
// more drawing has a processor of its own and only the cost of a very slow display path (a quarter of the thread) is
// worth reacting to. Measured on a modern 12-processor PC: 60 updates per second take 11 %, 144 take 22 %.
inline double display_budget_for_processors(int processors)
{
    return processors <= 1 ? 0.08 : processors == 2 ? 0.12 : 0.25;
}

// What the interface thread measured over one window.
struct DisplaySample {
    int64_t window_us = 0;     // length of the window
    int updates = 0;           // live updates in it
    int64_t moving_us = 0;     // time spent on the update and everything but the statistics block (tick, bar, time, rest)
    int64_t statistics_us = 0; // time spent drawing the statistics block (at the current tier)
    bool behind = false;       // the playback worker is behind
};

struct DisplayTierState {
    int tier = 0;
    int over = 0;              // consecutive windows over the budget
    int calm = 0;              // consecutive windows calm enough to step up
};

// Share of the interface thread the updates took in the window.
inline double display_busy_share(const DisplaySample& s)
{
    return s.window_us > 0 ? static_cast<double>(s.moving_us + s.statistics_us) / static_cast<double>(s.window_us) : 0.0;
}

// Predicted share if the same window had run at `toTier` (measured at `fromTier`).
inline double display_predicted_share(const DisplayLadder& l, int nativeHz, int fromTier, int toTier, const DisplaySample& s,
    const DisplayTierParams& p)
{
    if (s.updates <= 0) return 0.0;
    const double perMoving = static_cast<double>(s.moving_us) / s.updates;
    const double perStatistics = static_cast<double>(s.statistics_us) / s.updates;
    const double fullStatistics = l.half_statistics[fromTier] ? perStatistics / p.statistics_half : perStatistics;
    const double target = l.half_statistics[toTier] ? fullStatistics * p.statistics_half : fullStatistics;
    return display_tier_rate(l, toTier, nativeHz) * (perMoving + target) / 1000000.0;
}

inline DisplayTierState next_display_tier(const DisplayLadder& l, int nativeHz, DisplayTierState st, const DisplaySample& s,
    const DisplayTierParams& p)
{
    const int last = l.count - 1;
    if (st.tier > last) st.tier = last;
    if (st.tier < 0) st.tier = 0;
    if (s.window_us <= 0) return st;
    const double share = display_busy_share(s);
    bool down = false;
    if (s.behind) down = true;                                   // playback comes first
    else if (s.updates > 0 && share > p.budget * p.far_over) down = true;
    else if (s.updates > 0 && share > p.budget) down = (++st.over >= p.over_windows);
    else st.over = 0;
    if (down) {
        if (st.tier < last) ++st.tier;
        st.over = 0;
        st.calm = 0;
        return st;
    }
    if (s.behind || share > p.budget) { st.calm = 0; return st; }
    if (st.tier > 0 && s.updates > 0) {
        if (display_predicted_share(l, nativeHz, st.tier, st.tier - 1, s, p) <= p.budget * p.release) {
            if (++st.calm >= p.calm_windows) {
                --st.tier;
                st.calm = 0;
                st.over = 0;
            }
        }
        else st.calm = 0;
    }
    return st;
}

} // namespace app
} // namespace midibn
