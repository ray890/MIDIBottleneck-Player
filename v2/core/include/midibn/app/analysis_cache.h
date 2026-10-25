// The Analysis window's caches (v1 DiagnosticsForm): the cache key of a configuration and
// resolution, each window's own cache of finished analyses (12 entries, oldest first out),
// and the per-song cache every window of the same song shares (v1 ConditionalWeakTable of
// WeakReferences: an analysis stays reusable only while some window still shows or keeps it,
// and a song's caches disappear with the song).
#pragma once

#include "midibn/analysis/workload_analysis.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace midibn {
namespace app {

// v1 DiagnosticsForm.AnalysisCacheKey. Settings an analysis does not use are left out of the key,
// so changing them reuses the cached result.
std::string analysis_cache_key(const analysis::AnalysisConfiguration& configuration, int64_t resolutionMicroseconds);

const size_t kAnalysisCacheCapacity = 12;

// v1 _analysisCache + _analysisCacheOrder.
class LocalAnalysisCache {
public:
    std::shared_ptr<const analysis::WorkloadAnalysis> find(const std::string& key) const;
    // Remembers `analysis`; a new key past the capacity evicts the oldest key first.
    void remember(const std::string& key, std::shared_ptr<const analysis::WorkloadAnalysis> analysis);
    void clear();
    size_t size() const { return results_.size(); }
    std::deque<std::string> order() const { return order_; }

private:
    std::map<std::string, std::shared_ptr<const analysis::WorkloadAnalysis>> results_;
    std::deque<std::string> order_;
};

// v1 SharedSongAnalysisCache: weakly held finished analyses, usable from several windows and threads.
class SharedAnalysisResults {
public:
    std::shared_ptr<const analysis::WorkloadAnalysis> find(const std::string& key);
    void remember(const std::string& key, const std::shared_ptr<const analysis::WorkloadAnalysis>& analysis);
    size_t size();
    std::deque<std::string> order();

private:
    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<const analysis::WorkloadAnalysis>> results_;
    std::deque<std::string> order_;
};

// Everything cached for one song: its base workloads (v1 WorkloadAnalyzer's table) and the shared results.
struct SongAnalysisCaches {
    analysis::WorkloadCache workloads;
    SharedAnalysisResults results;
};

// The caches of `song` (any shared object identifying the song, e.g. the effective PlaybackSong).
// Created on first use; dropped once the song has been destroyed.
std::shared_ptr<SongAnalysisCaches> song_analysis_caches(const std::shared_ptr<const void>& song);
// For tests: how many songs currently have caches.
size_t song_analysis_cache_count();

} // namespace app
} // namespace midibn
