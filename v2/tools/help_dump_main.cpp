// Writes the markup of all eight Help topics for one platform to a file, so the text of a platform can be hashed before
// and after an edit (proof that another platform's wording is byte-identical).
//   midibn-help-dump windows|linux|android OUTPUT-FILE
#define _CRT_SECURE_NO_WARNINGS
#include "midibn/app/help_text.h"

#include <cstdio>
#include <cstring>

using namespace midibn::app;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fputs("usage: midibn-help-dump windows|linux|android OUTPUT-FILE\n", stderr);
        return 2;
    }
    HelpPlatform platform = HelpPlatform::Windows;
    if (!std::strcmp(argv[1], "linux")) platform = HelpPlatform::Linux;
    else if (!std::strcmp(argv[1], "android")) platform = HelpPlatform::Android;
    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 1;
    for (int i = 0; i < kHelpTopicCount; ++i) {
        const std::string text = help_text(static_cast<HelpTopic>(i), platform);
        std::fwrite(text.data(), 1, text.size(), out);
        std::fputs("\n=====\n", out);
    }
    std::fclose(out);
    return 0;
}
