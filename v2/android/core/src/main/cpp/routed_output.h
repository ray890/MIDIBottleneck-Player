// The output of the Android player: one midibn::platform::MidiOutput over every MIDI route the app offers.
//
// Header only and free of JNI so the native tests can use it with a stand-in catalog (platform/android/route/tests).
//
// Division of work (P12-AND6):
//   * device list and open/close go to a RouteCatalog (in the app: the Java side, player_jni.cpp's JavaCatalog). They run
//     rarely and on the thread of the controller (the UI thread) or a worker.
//   * every send, the batching, reset and panic go straight to midibn::android::current_route_output() (the shared
//     BatchingOutput, C++ all the way to the AMidi port, or one JNI call per burst for the Java route): no JNI call per
//     message from this class.
//
// Device ids: (route << 16) | device index within that route. Names are the names the routes give.
#pragma once

#include "midibn/midi_output.h"
#include "route_targets.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace midibn {
namespace android {

class RouteCatalog {
public:
    virtual ~RouteCatalog() {}
    // Number of routes whose devices are listed, in list order.
    virtual size_t route_count() = 0;
    // Fresh reading of the device names of one route.
    virtual std::vector<std::string> list_devices(size_t route) = 0;
    // Opens one device (may block for seconds). On success the route has attached its BatchingOutput as
    // current_route_output() and the result is empty; otherwise it is the plain-sentence reason. `name` is the name the
    // device had in the list; the catalog uses it to find the device again when the list changed meanwhile.
    virtual std::string open_device(size_t route, size_t index, const std::string& name) = 0;
    // Closes the open device of that route, if any.
    virtual void close_route(size_t route) = 0;
};

class RoutedOutput : public platform::MidiOutput {
public:
    explicit RoutedOutput(std::shared_ptr<RouteCatalog> catalog) : catalog_(std::move(catalog)) {}

    static uint32_t make_id(size_t route, size_t index) { return static_cast<uint32_t>((route << 16) | (index & 0xFFFF)); }
    static size_t route_of(uint32_t id) { return id >> 16; }
    static size_t index_of(uint32_t id) { return id & 0xFFFF; }

    const char* backend_name() const override { return "Android MIDI"; }

    std::vector<platform::OutputDeviceInfo> enumerate() const override
    {
        std::vector<platform::OutputDeviceInfo> result;
        names_.clear();
        const size_t routes = catalog_->route_count();
        for (size_t r = 0; r < routes; ++r) {
            const std::vector<std::string> list = catalog_->list_devices(r);
            for (size_t i = 0; i < list.size(); ++i) {
                platform::OutputDeviceInfo info;
                info.device_id = make_id(r, i);
                info.name = list[i].empty() ? "MIDI output " + std::to_string(result.size()) : list[i];
                names_.push_back(std::make_pair(info.device_id, info.name));
                result.push_back(info);
            }
        }
        return result;
    }

    platform::OutputStatus open(uint32_t device_id) override
    {
        if (open_) close();
        std::string name;
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i].first == device_id) name = names_[i].second;
        const std::string error = catalog_->open_device(route_of(device_id), index_of(device_id), name);
        if (!error.empty())
            return platform::OutputStatus::failure(platform::OutputErrorKind::InvalidOperation, error);
        out_ = current_route_output();
        if (!out_ || !out_->is_open())
            return platform::OutputStatus::failure(platform::OutputErrorKind::InvalidOperation, "The MIDI device could not be opened.");
        route_ = route_of(device_id);
        open_ = true;
        return platform::OutputStatus::success();
    }

    platform::OutputStatus close() override
    {
        if (open_) catalog_->close_route(route_);
        open_ = false;
        out_.reset();
        return platform::OutputStatus::success();
    }

    bool is_open() const override { return open_ && out_ && out_->is_open(); }

    platform::OutputStatus send_short(uint32_t packed) override
    {
        platform::OutputStatus failure;
        if (try_send_short(packed, failure)) return platform::OutputStatus::success();
        return failure;
    }

    bool try_send_short(uint32_t packed, platform::OutputStatus& failure) override
    {
        if (!out_) { failure = not_open(); return false; }
        return out_->try_send_short(packed, failure);
    }

    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        return out_ ? out_->send_long(fragment) : not_open();
    }

    platform::OutputStatus reset() override { return out_ ? out_->reset() : platform::OutputStatus::success(); }
    platform::OutputStatus panic() override { return out_ ? out_->panic() : platform::OutputStatus::success(); }
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { if (out_) out_->set_source_file(path_utf8); }
    std::string path_description() const override { return out_ ? out_->path_description() : std::string(); }
    void begin_send_batch() override { if (out_) out_->begin_send_batch(); }
    void end_send_batch() override { if (out_) out_->end_send_batch(); }

private:
    static platform::OutputStatus not_open()
    {
        return platform::OutputStatus::failure(platform::OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    }

    std::shared_ptr<RouteCatalog> catalog_;
    // What the last enumerate() returned (id and name), so open() can tell the catalog the name of the device.
    mutable std::vector<std::pair<uint32_t, std::string>> names_;
    // The output captured at open (== current_route_output() then): the engine thread reads it without a lock. The
    // engine is stopped whenever open/close run (controller contract).
    std::shared_ptr<BatchingOutput> out_;
    size_t route_ = 0;
    bool open_ = false;
};

} // namespace android
} // namespace midibn
