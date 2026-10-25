// The offline Help text of v2's own Help window: eight topics, the same topics and order as v1.
// help_text.cpp is written by hand for v2. The text is stored in a tiny neutral markup (UTF-8, "\n" line breaks;
// a "\r" before a line break is ignored), one block per line:
//   "# Heading"      topic heading           "## Heading"   section heading
//   "- item"         bullet                  "1. step"      numbered step (the number is kept)
//   any other line   paragraph               blank line     ignored
//   **bold**         bold words inside any block (an unclosed ** is literal text)
// parse_help_markup() turns it into blocks; each front end draws them its own way
// (Win32: a rich edit control; a later GTK front end: text tags).
#pragma once

#include <string>
#include <vector>

namespace midibn {
namespace app {

enum class HelpTopic {
    Overview,
    FileAndOutput,
    RateModel,
    QueueAndOverflow,
    Playback,
    Analysis,
    Statistics,
    Channels
};

constexpr int kHelpTopicCount = 8;

// The list-box captions ("Overview", "File and output", "Rate model", ...).
const char* help_topic_title(HelpTopic topic);
// The platform whose wording the Help uses. Windows: WinMM/KDMAPI outputs, the title-bar menu, borderless styles.
// Linux: ALSA sequencer ports, the menu button, no borderless styles or Direct WinMM section.
// Android: USB and Bluetooth MIDI devices and the built-in Sonivox synthesizer, touch wording, the top bar's menu.
enum class HelpPlatform { Windows, Linux, Android };

#ifdef _WIN32
constexpr HelpPlatform kNativeHelpPlatform = HelpPlatform::Windows;
#else
constexpr HelpPlatform kNativeHelpPlatform = HelpPlatform::Linux;
#endif

// The markup text of one topic for the platform the program runs on. An out-of-range topic gives the Overview text, as v1's default case did.
std::string help_text(HelpTopic topic);
// The same for a chosen platform (tests check both wordings on every build).
std::string help_text(HelpTopic topic, HelpPlatform platform);

enum class HelpBlockKind { Heading, Subheading, Paragraph, Bullet, Step };

struct HelpRun {
    std::string text;
    bool bold = false;
};

struct HelpBlock {
    HelpBlockKind kind = HelpBlockKind::Paragraph;
    int number = 0;               // Step only: the number written in the markup
    std::vector<HelpRun> runs;    // never empty; adjacent runs differ in bold
};

// Parses the markup above. Pure and portable.
std::vector<HelpBlock> parse_help_markup(const std::string& markup);

} // namespace app
} // namespace midibn
