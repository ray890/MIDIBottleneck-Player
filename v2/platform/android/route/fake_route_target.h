// The stand-in (fake) route target for tests: records every write, can be told to fail, no device needed.
#pragma once

#include "route_output.h"

namespace midibn {
namespace android {

class FakeRouteTarget : public RouteTarget {
public:
    std::vector<std::vector<uint8_t>> writes;   // one entry per write call
    int fail_after_writes = -1;                  // fail the write with this index (0 based) and every later one; -1 never
    bool open_fails = false;
    int discard_calls = 0;
    int close_calls = 0;
    bool opened = false;

    const char* name() const override { return "Fake"; }
    std::vector<platform::OutputDeviceInfo> devices() const override
    {
        platform::OutputDeviceInfo info;
        info.device_id = 0;
        info.name = "Fake device";
        return std::vector<platform::OutputDeviceInfo>(1, info);
    }
    bool open(uint32_t, std::string& error) override
    {
        if (open_fails) { error = "Fake open failure."; return false; }
        opened = true;
        return true;
    }
    void close() override { opened = false; ++close_calls; }
    bool is_open() const override { return opened; }
    bool write(const uint8_t* data, size_t length, std::string& error) override
    {
        if (fail_after_writes >= 0 && static_cast<int>(writes.size()) >= fail_after_writes) {
            error = "Fake device unplugged.";
            return false;
        }
        writes.push_back(std::vector<uint8_t>(data, data + length));
        return true;
    }
    void discard_pending() override { ++discard_calls; }
    std::string describe() const override { return "Fake route."; }

    std::vector<uint8_t> all_bytes() const
    {
        std::vector<uint8_t> all;
        for (size_t i = 0; i < writes.size(); ++i) all.insert(all.end(), writes[i].begin(), writes[i].end());
        return all;
    }
};

} // namespace android
} // namespace midibn
