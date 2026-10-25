#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "device_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace midibn {
namespace tools {

namespace {

bool write_all(FILE* f, const void* data, size_t bytes) { return bytes == 0 || std::fwrite(data, 1, bytes, f) == bytes; }

double percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return 0.0;
    double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(rank));
    size_t hi = std::min(sorted.size() - 1, lo + 1);
    double frac = rank - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

std::string num(double v, int digits = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", digits, v);
    return buffer;
}

void add(std::vector<std::string>& out, const std::string& key, const std::string& value) { out.push_back(key + "=" + value); }
void addn(std::vector<std::string>& out, const std::string& key, double value, int digits = 3) { add(out, key, num(value, digits)); }
void addi(std::vector<std::string>& out, const std::string& key, long long value) { add(out, key, std::to_string(value)); }

} // namespace

bool write_sample_file(const std::string& path, const SampleRun& run)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    SampleHeader header = run.header;
    header.sample_count = static_cast<int64_t>(run.samples.size());
    bool ok = write_all(f, &header, sizeof header) && write_all(f, run.samples.data(), run.samples.size() * sizeof(SampleRecord));
    return std::fclose(f) == 0 && ok;
}

bool read_sample_file(const std::string& path, SampleRun& run, std::string& error)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    bool ok = std::fread(&run.header, 1, sizeof run.header, f) == sizeof run.header;
    if (ok && std::memcmp(run.header.magic, "MBDEVS01", 8) != 0) { error = "bad magic in " + path; std::fclose(f); return false; }
    if (ok) {
        run.samples.resize(static_cast<size_t>(run.header.sample_count));
        ok = std::fread(run.samples.data(), sizeof(SampleRecord), run.samples.size(), f) == run.samples.size();
    }
    std::fclose(f);
    if (!ok) error = "short read in " + path;
    return ok;
}

std::vector<std::string> analyze_samples(const SampleRun& run, const std::string& timeline_csv_path)
{
    std::vector<std::string> out;
    const std::vector<SampleRecord>& s = run.samples;
    const size_t n = s.size();
    const double tickNs = 1e9 / static_cast<double>(run.header.frequency);
    addi(out, "sample_every", run.header.sample_every);
    addi(out, "samples", static_cast<long long>(n));
    addi(out, "total_sends", run.header.total_sends);
    addi(out, "expected_events", run.header.expected_events);
    addn(out, "qpc_cost_ns", run.header.qpc_cost_ns, 1);
    if (n < 4) { add(out, "analysis", "too few samples"); return out; }

    std::vector<double> t(n);      // send time since engine start, microseconds
    std::vector<double> late(n);   // microseconds
    for (size_t i = 0; i < n; ++i) {
        t[i] = static_cast<double>(s[i].before_rel_ticks - run.header.epoch_rel_ticks) * tickNs / 1000.0;   // since the engine's own time zero
        late[i] = t[i] - static_cast<double>(s[i].intended_us);
    }
    const double spanUs = t[n - 1] - t[0];
    const long long kSpan = s[n - 1].k - s[0].k;
    addn(out, "first_send_ms", t[0] / 1000.0);
    addn(out, "last_send_ms", t[n - 1] / 1000.0);
    addn(out, "span_ms", spanUs / 1000.0);
    addn(out, "first_late_us", late[0], 1);
    addn(out, "throughput_meps", spanUs > 0 ? static_cast<double>(kSpan) / spanUs : 0.0, 4);   // events per microsecond = millions per second

    // ---- lateness distribution (actual send time minus intended time) ----
    {
        std::vector<double> v(late);
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        addn(out, "late_us_min", v.front(), 1);
        addn(out, "late_us_mean", sum / static_cast<double>(n), 1);
        addn(out, "late_us_p50", percentile(v, 50), 1);
        addn(out, "late_us_p90", percentile(v, 90), 1);
        addn(out, "late_us_p99", percentile(v, 99), 1);
        addn(out, "late_us_p999", percentile(v, 99.9), 1);
        addn(out, "late_us_max", v.back(), 1);
        const double limits[] = { 100.0, 1000.0, 5000.0, 10000.0, 20000.0 };
        const char* names[] = { "late_over_100us_pct", "late_over_1ms_pct", "late_over_5ms_pct", "late_over_10ms_pct", "late_over_20ms_pct" };
        for (int i = 0; i < 5; ++i) {
            size_t c = static_cast<size_t>(v.end() - std::upper_bound(v.begin(), v.end(), limits[i]));
            addn(out, names[i], 100.0 * static_cast<double>(c) / static_cast<double>(n), 3);
        }
    }

    // ---- time inside the output call (timer-read cost removed) ----
    {
        std::vector<double> d(n);
        double sum = 0;
        for (size_t i = 0; i < n; ++i) {
            d[i] = static_cast<double>(s[i].duration_ticks) * tickNs;
            sum += d[i];
        }
        // The timer ticks every 100 ns, so a single duration is 0, 100, 200 ... ns; only the MEAN over many
        // samples is meaningful at sub-tick scale (the sample positions dither against the tick). The tail
        // figures below are for calls of 1 microsecond and longer.
        const double mean = std::max(0.0, sum / static_cast<double>(n) - run.header.qpc_cost_ns);
        size_t gt1 = 0, gt10 = 0, gt100 = 0;
        double mx = 0;
        for (double x : d) { if (x > 1000.0) ++gt1; if (x > 10000.0) ++gt10; if (x > 100000.0) ++gt100; mx = std::max(mx, x); }
        addn(out, "send_ns_mean", mean, 1);
        addn(out, "send_call_gt_1us_pct", 100.0 * static_cast<double>(gt1) / static_cast<double>(n), 3);
        addn(out, "send_call_gt_10us_pct", 100.0 * static_cast<double>(gt10) / static_cast<double>(n), 3);
        addn(out, "send_call_gt_100us_pct", 100.0 * static_cast<double>(gt100) / static_cast<double>(n), 3);
        addn(out, "send_ns_max", mx, 0);
        const double estTotalMs = mean * static_cast<double>(kSpan + 1) / 1e6;
        addn(out, "send_est_total_ms", estTotalMs, 1);
        addn(out, "send_fraction_of_span_pct", spanUs > 0 ? 100.0 * estTotalMs * 1000.0 / spanUs : 0.0, 1);
        addn(out, "send_per_event_ns_from_span", kSpan > 0 ? spanUs * 1000.0 / static_cast<double>(kSpan) : 0.0, 1);
    }

    // ---- delivery intervals: time per sample_every events ----
    std::vector<double> dt;     // microseconds per full interval
    std::vector<double> dlate;  // change in lateness between consecutive samples, microseconds
    for (size_t i = 0; i + 1 < n; ++i) {
        dlate.push_back(late[i + 1] - late[i]);
        if (s[i + 1].k - s[i].k == static_cast<long long>(run.header.sample_every)) dt.push_back(t[i + 1] - t[i]);
    }
    if (!dt.empty()) {
        std::vector<double> v(dt);
        std::sort(v.begin(), v.end());
        const double med = percentile(v, 50);
        addn(out, "interval_us_p50", med, 2);
        addn(out, "interval_us_p99", percentile(v, 99), 2);
        addn(out, "interval_us_p999", percentile(v, 99.9), 2);
        addn(out, "interval_us_max", v.back(), 1);
        size_t fast = 0, slow = 0, huge = 0;
        for (double x : dt) { if (x < med * 0.5) ++fast; if (x > med * 2.0) ++slow; if (x > 250.0) ++huge; }
        addn(out, "interval_fast_lt_half_median_pct", 100.0 * static_cast<double>(fast) / static_cast<double>(dt.size()), 3);
        addn(out, "interval_slow_gt_2x_median_pct", 100.0 * static_cast<double>(slow) / static_cast<double>(dt.size()), 3);
        addn(out, "interval_gt_250us_pct", 100.0 * static_cast<double>(huge) / static_cast<double>(dt.size()), 3);
    }

    // ---- stalls: the send schedule slips by more than a threshold between two consecutive samples ----
    {
        const double th = 250.0;
        long long c250 = 0, c1000 = 0, c5000 = 0, drops250 = 0;
        double maxJump = 0, totalJump = 0;
        std::vector<double> onsets;   // send time of the sample that follows the jump
        std::vector<double> jumps;
        for (size_t i = 0; i < dlate.size(); ++i) {
            const double d = dlate[i];
            if (d > th) {
                ++c250; totalJump += d; onsets.push_back(t[i + 1]); jumps.push_back(d);
                if (d > 1000.0) ++c1000;
                if (d > 5000.0) ++c5000;
                maxJump = std::max(maxJump, d);
            }
            if (d < -th) ++drops250;
        }
        addi(out, "stall_count_gt_250us", c250);
        addi(out, "stall_count_gt_1ms", c1000);
        addi(out, "stall_count_gt_5ms", c5000);
        addn(out, "stall_max_us", maxJump, 1);
        addn(out, "stall_total_ms", totalJump / 1000.0, 2);
        addi(out, "lateness_drop_count_gt_250us", drops250);
        if (!jumps.empty()) {
            std::vector<double> v(jumps);
            std::sort(v.begin(), v.end());
            addn(out, "stall_size_us_p50", percentile(v, 50), 1);
            addn(out, "stall_size_us_p90", percentile(v, 90), 1);
        }
        if (onsets.size() >= 3) {
            std::vector<double> gaps;
            for (size_t i = 0; i + 1 < onsets.size(); ++i) gaps.push_back((onsets[i + 1] - onsets[i]) / 1000.0);   // ms
            std::vector<double> v(gaps);
            std::sort(v.begin(), v.end());
            addn(out, "stall_spacing_ms_p10", percentile(v, 10), 2);
            addn(out, "stall_spacing_ms_p50", percentile(v, 50), 2);
            addn(out, "stall_spacing_ms_p90", percentile(v, 90), 2);
            // Most common spacing, in 0.5 ms bins up to 40 ms.
            std::map<int, int> histogram;
            for (double g : gaps) if (g < 40.0) ++histogram[static_cast<int>(g * 2.0)];
            int bestBin = -1, bestCount = 0;
            for (const auto& kv : histogram) if (kv.second > bestCount) { bestCount = kv.second; bestBin = kv.first; }
            if (bestBin >= 0) {
                addn(out, "stall_spacing_mode_ms", (bestBin + 0.5) * 0.5, 2);
                addn(out, "stall_spacing_mode_share_pct", 100.0 * bestCount / static_cast<double>(gaps.size()), 1);
            }
        }
    }

    // ---- lateness per millisecond of send time, and its autocorrelation ----
    {
        const size_t bins = static_cast<size_t>(spanUs / 1000.0) + 1;
        std::vector<double> sum(bins, 0.0), mx(bins, -1e18);
        std::vector<int> cnt(bins, 0);
        for (size_t i = 0; i < n; ++i) {
            size_t b = std::min(bins - 1, static_cast<size_t>((t[i] - t[0]) / 1000.0));
            sum[b] += late[i]; ++cnt[b]; mx[b] = std::max(mx[b], late[i]);
        }
        std::vector<double> mean(bins, 0.0);
        double prev = late[0];
        for (size_t b = 0; b < bins; ++b) { if (cnt[b] > 0) prev = sum[b] / cnt[b]; mean[b] = prev; }
        if (!timeline_csv_path.empty()) {
            FILE* f = std::fopen(timeline_csv_path.c_str(), "wb");
            if (f) {
                std::fputs("ms,samples,mean_late_us,max_late_us\n", f);
                for (size_t b = 0; b < bins; ++b)
                    std::fprintf(f, "%zu,%d,%.1f,%.1f\n", b, cnt[b], mean[b], cnt[b] ? mx[b] : mean[b]);
                std::fclose(f);
            }
        }
        double avg = 0;
        for (double v : mean) avg += v;
        avg /= static_cast<double>(bins);
        double var = 0;
        for (double v : mean) var += (v - avg) * (v - avg);
        double lo = *std::min_element(mean.begin(), mean.end());
        double hi = *std::max_element(mean.begin(), mean.end());
        addn(out, "late_ms_bin_mean_range_us", hi - lo, 1);
        if (bins > 60 && var > 0 && hi - lo > 20.0) {
            double bestR = -2; int bestLag = 0;
            double r1 = 0, r16 = 0;
            for (int lag = 1; lag <= 40; ++lag) {
                double acc = 0;
                for (size_t b = 0; b + static_cast<size_t>(lag) < bins; ++b) acc += (mean[b] - avg) * (mean[b + static_cast<size_t>(lag)] - avg);
                const double r = acc / var;
                if (lag == 1) r1 = r;
                if (lag == 16) r16 = r;
                if (lag >= 2 && r > bestR) { bestR = r; bestLag = lag; }
            }
            addn(out, "acf_late_1ms_lag1", r1, 3);
            addn(out, "acf_late_1ms_lag16", r16, 3);
            addi(out, "acf_late_1ms_best_lag_ms", bestLag);
            addn(out, "acf_late_1ms_best_r", bestR, 3);
        }
        else add(out, "acf_late_1ms", "flat (lateness range under 20 us per ms)");
    }
    return out;
}

} // namespace tools
} // namespace midibn
