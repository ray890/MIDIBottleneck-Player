#include "midibn/app/analysis_cache.h"

namespace midibn {
namespace app {

using analysis::AnalysisConfiguration;
using analysis::WorkloadAnalysis;
using models::ServiceDurationMode;

namespace {

std::string net_bool(bool value) { return value ? "True" : "False"; }
std::string num(int64_t value) { return std::to_string(value); }

struct RegistryEntry {
    std::weak_ptr<const void> owner;
    std::shared_ptr<SongAnalysisCaches> caches;
};

struct Registry {
    std::mutex mutex;
    std::map<const void*, RegistryEntry> songs;
};

Registry g_registry;

} // namespace

std::string analysis_cache_key(const AnalysisConfiguration& value, int64_t resolution)
{
    std::string overrideKey = "|forced-overrides=" +
        (value.override_projection == nullptr ? std::string("off") : value.override_projection->signature());
    if (value.per_note_interval_gate_enabled)
        return num(resolution) + "|per-note|" + num(value.processing_us) + "|adaptive=" + net_bool(value.scale_note_interval_by_traffic) +
            "|polyphony=" + net_bool(value.interval_polyphony_limit_enabled) + ":" + num(value.interval_polyphony_limit) + overrideKey;
    if (value.delayed_note_off_enabled)
        return num(resolution) + "|delayed-note-off|" + num(value.processing_us) + "|adaptive=" +
            net_bool(value.scale_note_interval_by_traffic) + "|polyphony=" + net_bool(value.interval_polyphony_limit_enabled) + ":" +
            num(value.interval_polyphony_limit) + overrideKey;
    if (value.note_attack_limit_enabled)
        return num(resolution) + "|note-start-rate-gate|" + num(value.events_per_second) + overrideKey;
    if (value.output_rate_limit_enabled)
        return num(resolution) + "|output-rate-gate|" + num(value.events_per_second) + overrideKey;
    bool service = value.simulate_slowdown || (value.apply_queue_limit_without_slowdown && value.queue_length_limit_enabled);
    return num(resolution) + "|" + net_bool(value.simulate_slowdown) + "|paced=" + net_bool(value.output_paced_timeline) + "|" +
        net_bool(value.apply_queue_limit_without_slowdown) + "|" + num(static_cast<int>(value.service_duration_mode)) + "|" +
        num(service && value.service_duration_mode == ServiceDurationMode::ProcessingTime ? value.processing_us : 0) + "|" +
        num(service && value.service_duration_mode == ServiceDurationMode::MidiBitrate ? value.midi_bitrate : 0) + "|" +
        num(service && value.service_duration_mode == ServiceDurationMode::EventsPerSecond ? value.events_per_second : 0) + "|" +
        net_bool(value.queue_length_limit_enabled) + "|" + num(value.queue_length_limit_enabled ? value.queue_length_limit : 0) + "|" +
        net_bool(value.queue_age_limit_enabled) + "|" + num(value.queue_age_limit_enabled ? value.queue_age_limit_us : 0) + "|" +
        num(value.queue_length_limit_enabled ? static_cast<int>(value.overflow_policy) : 0) + overrideKey;
}

// ---------------------------------------------------------------- LocalAnalysisCache

std::shared_ptr<const WorkloadAnalysis> LocalAnalysisCache::find(const std::string& key) const
{
    auto it = results_.find(key);
    return it == results_.end() ? nullptr : it->second;
}

void LocalAnalysisCache::remember(const std::string& key, std::shared_ptr<const WorkloadAnalysis> analysis)
{
    if (results_.find(key) == results_.end()) {
        while (results_.size() >= kAnalysisCacheCapacity && !order_.empty()) {
            results_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_back(key);
    }
    results_[key] = std::move(analysis);
}

void LocalAnalysisCache::clear()
{
    results_.clear();
    order_.clear();
}

// ---------------------------------------------------------------- SharedAnalysisResults

std::shared_ptr<const WorkloadAnalysis> SharedAnalysisResults::find(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = results_.find(key);
    if (it == results_.end()) return nullptr;
    std::shared_ptr<const WorkloadAnalysis> analysis = it->second.lock();
    if (analysis) return analysis;
    results_.erase(it);
    return nullptr;
}

void SharedAnalysisResults::remember(const std::string& key, const std::shared_ptr<const WorkloadAnalysis>& analysis)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (results_.find(key) == results_.end()) {
        while (results_.size() >= kAnalysisCacheCapacity && !order_.empty()) {
            results_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_back(key);
    }
    results_[key] = analysis;
}

size_t SharedAnalysisResults::size()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return results_.size();
}

std::deque<std::string> SharedAnalysisResults::order()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return order_;
}

// ---------------------------------------------------------------- registry

std::shared_ptr<SongAnalysisCaches> song_analysis_caches(const std::shared_ptr<const void>& song)
{
    std::lock_guard<std::mutex> lock(g_registry.mutex);
    for (auto it = g_registry.songs.begin(); it != g_registry.songs.end();) {
        if (it->second.owner.expired()) it = g_registry.songs.erase(it);
        else ++it;
    }
    auto found = g_registry.songs.find(song.get());
    if (found != g_registry.songs.end()) return found->second.caches;
    RegistryEntry entry;
    entry.owner = song;
    entry.caches = std::make_shared<SongAnalysisCaches>();
    g_registry.songs[song.get()] = entry;
    return entry.caches;
}

size_t song_analysis_cache_count()
{
    std::lock_guard<std::mutex> lock(g_registry.mutex);
    size_t count = 0;
    for (auto it = g_registry.songs.begin(); it != g_registry.songs.end(); ++it)
        if (!it->second.owner.expired()) ++count;
    return count;
}

} // namespace app
} // namespace midibn
