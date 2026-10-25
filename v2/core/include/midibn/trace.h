// Canonical load trace (v2/harness/TRACE_FORMAT.md, version 1).
#pragma once

#include "midibn/platform.h"

#include <string>

namespace midibn {

// Produces the full trace text for one input. `producer` and `input_name`
// appear only in ignored comment lines.
std::string write_load_trace(platform::ByteSource& source, const std::string& input_name,
    bool summary, const std::string& producer);

// Load-flow trace ("kind flow"): progress sequences, large-file decisions and
// scripted cancellation outcomes of load_with_preflight for one pointer size.
std::string write_flow_trace(platform::ByteSource& source, const std::string& input_name,
    bool summary, int pointer_size, const std::string& producer);

// Removes comment lines (starting with '#') for parity comparison.
std::string strip_trace_comments(const std::string& trace);

} // namespace midibn
