// Native (AMidi) route target: writes go straight from C++ to the device with AMidiInputPort_send (API 29+).
// Compiled only when MIDIBN_ROUTE_AMIDI is defined (the "modern" flavor; the CMake file then links amidi).
#ifdef MIDIBN_ROUTE_AMIDI

#include "route_targets.h"

#include <amidi/AMidi.h>

#include <cerrno>
#include <cstring>

namespace midibn {
namespace android {

namespace {

class AMidiRouteTarget : public RouteTarget {
public:
    AMidiRouteTarget(AMidiDevice* device, AMidiInputPort* port, int port_number)
        : device_(device), port_(port), portNumber_(port_number) {}
    ~AMidiRouteTarget() override { release(); }

    const char* name() const override { return "Native MIDI (AMidi)"; }

    std::vector<platform::OutputDeviceInfo> devices() const override
    {
        platform::OutputDeviceInfo info;
        info.device_id = static_cast<uint32_t>(portNumber_);
        info.name = "Attached MIDI device, input port " + std::to_string(portNumber_);
        return std::vector<platform::OutputDeviceInfo>(1, info);
    }

    // The Java side opened the device and attached it; open only confirms.
    bool open(uint32_t, std::string& error) override
    {
        if (!port_) { error = "The native MIDI port is closed. Open the device again."; return false; }
        return true;
    }

    // The engine closing its output does not detach the device; the Java side detaches (the destructor releases).
    void close() override {}

    void release()
    {
        if (port_) { AMidiInputPort_close(port_); port_ = nullptr; }
        if (device_) { AMidiDevice_release(device_); device_ = nullptr; }
    }

    bool is_open() const override { return port_ != nullptr; }

    bool write(const uint8_t* data, size_t length, std::string& error) override
    {
        if (!port_) { error = "The native MIDI port is closed."; return false; }
        while (length > 0) {
            ssize_t sent = AMidiInputPort_send(port_, data, length);
            if (sent < 0) {
                error = "The MIDI device did not accept the messages (the device may have been unplugged; native error " +
                    std::to_string(static_cast<long>(sent)) + "). Choose the output again.";
                return false;
            }
            if (sent == 0) {
                error = "The MIDI device accepted no data. It may have been unplugged. Choose the output again.";
                return false;
            }
            data += sent;
            length -= static_cast<size_t>(sent);
        }
        return true;
    }

    void discard_pending() override
    {
        if (port_) AMidiInputPort_sendFlush(port_);
    }

    std::string describe() const override { return "Android native MIDI (AMidi), batched: one send per burst."; }

private:
    AMidiDevice* device_;
    AMidiInputPort* port_;
    int portNumber_;
};

} // namespace

std::unique_ptr<RouteTarget> make_amidi_route_target(JNIEnv* env, jobject midiDevice, int port_number, std::string& error)
{
    AMidiDevice* device = nullptr;
    media_status_t status = AMidiDevice_fromJava(env, midiDevice, &device);
    if (status != AMEDIA_OK || !device) {
        error = "The native MIDI interface could not take the device (error " + std::to_string(static_cast<int>(status)) + ").";
        return nullptr;
    }
    AMidiInputPort* port = nullptr;
    status = AMidiInputPort_open(device, port_number, &port);
    if (status != AMEDIA_OK || !port) {
        AMidiDevice_release(device);
        error = "The MIDI port could not be opened natively (error " + std::to_string(static_cast<int>(status)) +
            "). It may be in use by another app.";
        return nullptr;
    }
    return std::unique_ptr<RouteTarget>(new AMidiRouteTarget(device, port, port_number));
}

} // namespace android
} // namespace midibn

#endif // MIDIBN_ROUTE_AMIDI
