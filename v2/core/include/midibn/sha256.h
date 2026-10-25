// Small first-party SHA-256 (FIPS 180-4) for trace digests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace midibn {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    void update(const std::string& text) { update(text.data(), text.size()); }
    // Finishes and returns 64 uppercase hex digits. The object must not be reused.
    std::string hex_digest();

private:
    void block(const uint8_t* data);
    uint32_t state_[8];
    uint64_t length_;
    uint8_t buffer_[64];
    size_t used_;
};

} // namespace midibn
