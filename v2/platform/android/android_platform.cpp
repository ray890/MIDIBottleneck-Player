#include "android_platform.h"

#include <sys/system_properties.h>

namespace midibn {
namespace android {

const char* compiled_abi() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

namespace {
std::string property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    int length = __system_property_get(name, value);
    return length > 0 ? std::string(value, static_cast<size_t>(length)) : std::string();
}
} // namespace

std::string describe_device() {
    std::string text = "Android " + property("ro.build.version.release") + " (API " + property("ro.build.version.sdk") + ")";
    std::string model = property("ro.product.model");
    if (!model.empty()) text += ", " + model;
    text += ", ";
    text += compiled_abi();
    return text;
}

} // namespace android
} // namespace midibn
