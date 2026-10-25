// The layout of every message box that reports a failure (prompt 012, user decision 2026-10-19):
//
//     <one plain sentence that says what went wrong>
//
//     Details:
//     <error codes, file positions, a driver's own error text: only where they help a bug report>
//
// The plain sentence comes first and is understandable without any technical knowledge. The details come below it,
// separately, in the same message box (the Windows message box shows them after a blank line; the GTK dialog shows
// them as its secondary text). The details are the original wording of the lower layers, word for word, so no fact is
// lost. A message that has nothing technical to add is only the plain sentence.
//
// The lower layers (the MIDI outputs, the loader, the file system) keep their texts: v2's comparison tests and the
// diagnostic tools read them. This header decides where such a text goes in the box.
#pragma once

#include "midibn/engine/engine_types.h"

#include <string>

namespace midibn {
namespace app {

struct ErrorText {
    std::string plain;
    std::string details;   // empty: nothing technical to add
};

// The heading of the details part (also what a frontend looks for to split a message again).
inline const char* error_details_heading() { return "Details:"; }

// "<plain>\n\nDetails:\n<details>", or only "<plain>" when there are no details.
inline std::string join_error_text(const std::string& plain, const std::string& details)
{
    if (details.empty()) return plain;
    return plain + "\n\n" + error_details_heading() + "\n" + details;
}

inline std::string join_error_text(const ErrorText& text) { return join_error_text(text.plain, text.details); }

// The reverse, for a frontend that shows the two parts apart (GTK). A text without the heading is all plain.
inline void split_error_text(const std::string& text, std::string& plain, std::string& details)
{
    const std::string marker = std::string("\n\n") + error_details_heading() + "\n";
    const size_t at = text.find(marker);
    if (at == std::string::npos) { plain = text; details.clear(); return; }
    plain = text.substr(0, at);
    details = text.substr(at + marker.size());
}

namespace error_text_detail {

inline bool starts_with(const std::string& text, const char* prefix)
{
    return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

inline bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

inline bool contains_no_case(const std::string& text, const char* part)
{
    std::string a = text, b = part;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] >= 'A' && a[i] <= 'Z') a[i] = static_cast<char>(a[i] - 'A' + 'a');
    for (size_t i = 0; i < b.size(); ++i) if (b[i] >= 'A' && b[i] <= 'Z') b[i] = static_cast<char>(b[i] - 'A' + 'a');
    return a.find(b) != std::string::npos;
}

inline std::string trimmed(const std::string& text)
{
    size_t first = 0, last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\r' || text[first] == '\n' || text[first] == '\t')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\r' || text[last - 1] == '\n' || text[last - 1] == '\t')) --last;
    return text.substr(first, last - first);
}

// True when the text carries something only a developer reads: a native error code, a hexadecimal value, a
// "Windows error N".
inline bool looks_technical(const std::string& text)
{
    return contains(text, "native code") || contains(text, "(Windows error") || contains(text, "(PE machine") ||
        contains(text, "0x") || contains(text, "Exception");
}

// A text of the form "<sentence>: <path>[ (...)]." where the part after the last ": " is a file path.
inline bool split_trailing_path(const std::string& text, std::string& sentence, std::string& path)
{
    const size_t colon = text.rfind(": ");
    if (colon == std::string::npos) return false;
    const std::string rest = text.substr(colon + 2);
    const bool pathLike = rest.size() > 3 && ((rest[1] == ':' && (rest[2] == '\\' || rest[2] == '/')) || rest[0] == '/' || rest.compare(0, 2, "\\\\") == 0);
    if (!pathLike) return false;
    sentence = text.substr(0, colon);
    path = rest;
    return true;
}

} // namespace error_text_detail

// An error text of a MIDI output (or of any lower layer that reports in the same style). `fallbackPlain` is the plain
// sentence to use when the text is technical and says nothing plainer; it names what the person was doing.
inline ErrorText explain_error(const std::string& rawText, const std::string& fallbackPlain)
{
    using namespace error_text_detail;
    ErrorText out;
    const std::string raw = trimmed(rawText);
    if (raw.empty()) { out.plain = fallbackPlain; return out; }

    if (starts_with(raw, "MIDI error while ")) {   // "MIDI error while <doing> (native code N): <text from Windows or the driver>"
        if (starts_with(raw, "MIDI error while opening")) out.plain = "The MIDI output could not be opened.";
        else if (starts_with(raw, "MIDI error while waiting for two accepted initialization probes"))
            out.plain = "The MIDI output did not accept the start-up test messages.";
        else out.plain = "A message could not be sent to the MIDI output.";
        out.details = raw;
        return out;
    }
    if (starts_with(raw, "MIDI System Exclusive failure")) {
        out.plain = "A System Exclusive (SysEx) message in the song could not be sent to the MIDI output.";
        out.details = raw;
        return out;
    }
    std::string sentence, path;
    if (split_trailing_path(raw, sentence, path)) {   // "The KDMAPI provider ...: C:\...\OmniMIDI.dll"
        std::string clean = sentence;
        while (!clean.empty() && clean[clean.size() - 1] == '.') clean.erase(clean.size() - 1);
        out.plain = clean + ".";
        out.details = path;
        if (starts_with(raw, "The ")) out.details = "File: " + path;
        return out;
    }
    if (contains(raw, "ALSA")) {   // Linux: "<sentence> (<text from the system>)" or "<sentence> (<text from the system>)."
        std::string body = raw;
        if (body.size() > 2 && body.compare(body.size() - 2, 2, ").") == 0) body.erase(body.size() - 1);
        const size_t open = body.rfind(" (");
        if (!body.empty() && body[body.size() - 1] == ')' && open != std::string::npos && open > 0) {
            out.plain = body.substr(0, open);
            if (out.plain[out.plain.size() - 1] != '.') out.plain += ".";
            out.details = body.substr(open + 2, body.size() - open - 3);
            return out;
        }
    }
    if (looks_technical(raw)) {
        out.plain = fallbackPlain;
        out.details = raw;
        return out;
    }
    out.plain = raw;
    return out;
}

// The same for an error of the playback engine; the engine's own plain sentence is the message, and the cause
// underneath it (what the output or the driver said) goes to the details.
inline ErrorText explain_engine_error(const engine::EngineError& error, const std::string& fallbackPlain)
{
    ErrorText out = explain_error(error.message, fallbackPlain);
    if (!error.inner_message.empty()) {
        if (!out.details.empty()) out.details += "\n";
        out.details += "Cause: " + error_text_detail::trimmed(error.inner_message);
    }
    return out;
}

// A failed load: `type` and `message` as the loader reports them (the type names the kind of failure).
inline ErrorText explain_load_error(const std::string& type, const std::string& message)
{
    using namespace error_text_detail;
    ErrorText out;
    const std::string raw = trimmed(message);
    if (type == "IOException" && (starts_with(raw, "Could not open ") || starts_with(raw, "Could not read "))) {
        out.plain = "The MIDI file could not be opened.";
        out.details = raw;
        return out;
    }
    if (type == "InvalidDataException" || type == "EndOfStreamException") {
        // A limit of this player ("not supported", "Unsupported Standard MIDI File format", a size limit) is its own explanation; any other text is
        // about where the file is wrong, which only a bug report needs.
        if (contains_no_case(raw, "not supported") || starts_with(raw, "Unsupported Standard") || contains(raw, " limit") || contains(raw, "damaged")) { out.plain = raw; return out; }
        out.plain = "This file could not be read as a MIDI file. It may be damaged or not a complete MIDI file.";
        out.details = raw;
        return out;
    }
    return explain_error(raw, "The MIDI file could not be loaded.");
}

} // namespace app
} // namespace midibn
