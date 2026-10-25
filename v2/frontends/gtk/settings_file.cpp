#include "settings_file.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace midibn {
namespace gtkui {
namespace {

std::string error_detail(const char* what, int code, const std::string& path)
{
    return std::string("Error ") + std::to_string(code) + " (" + std::strerror(code) + ") while " + what + " " + path;
}

void make_parent_folders(const std::string& path)
{
    // mkdir -p for the folder that holds the file (at most two missing levels: ~/.config and midibottleneck)
    std::string folder = path.substr(0, path.find_last_of('/'));
    for (size_t i = 1; i <= folder.size(); ++i)
        if (i == folder.size() || folder[i] == '/') ::mkdir(folder.substr(0, i).c_str(), 0700);
}

class PosixSettingsIo : public app::SettingsIo {
public:
    explicit PosixSettingsIo(const std::string& path) : path_(path) {}

    ReadResult read(std::string& bytes, std::string& detail) override
    {
        bytes.clear();
        if (path_.empty()) return ReadResult::Missing;
        int fd = ::open(path_.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR) return ReadResult::Missing;
            detail = error_detail("opening", errno, path_);
            return ReadResult::Failed;
        }
        char buffer[4096];
        for (;;) {
            ssize_t got = ::read(fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR) continue;
                int code = errno;
                ::close(fd);
                bytes.clear();
                detail = error_detail("reading", code, path_);
                return ReadResult::Failed;
            }
            if (got == 0) break;
            bytes.append(buffer, static_cast<size_t>(got));
            if (bytes.size() > 70000) break;   // not our file; the parser calls anything over 64 KiB damaged
        }
        ::close(fd);
        return ReadResult::Ok;
    }

    bool write_atomic(const std::string& bytes, std::string& detail) override
    {
        if (path_.empty()) { detail = "No folder for the settings file was found."; return false; }
        make_parent_folders(path_);
        const std::string temp = path_ + ".tmp";
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { detail = error_detail("creating", errno, temp); return false; }
        size_t done = 0;
        while (done < bytes.size()) {
            ssize_t put = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (put < 0) {
                if (errno == EINTR) continue;
                int code = errno;
                ::close(fd);
                ::unlink(temp.c_str());
                detail = error_detail("writing", code, temp);
                return false;
            }
            done += static_cast<size_t>(put);
        }
        ::fsync(fd);
        ::close(fd);
        if (::rename(temp.c_str(), path_.c_str()) != 0) {
            int code = errno;
            ::unlink(temp.c_str());
            detail = error_detail("replacing", code, path_);
            return false;
        }
        return true;
    }

private:
    std::string path_;
};

}   // namespace

std::string default_settings_path()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') return std::string(xdg) + "/midibottleneck/settings.ini";
    const char* home = std::getenv("HOME");
    if (home && home[0] == '/') return std::string(home) + "/.config/midibottleneck/settings.ini";
    return std::string();
}

std::unique_ptr<app::SettingsIo> make_settings_io(const std::string& explicitPath)
{
    return std::unique_ptr<app::SettingsIo>(new PosixSettingsIo(explicitPath.empty() ? default_settings_path() : explicitPath));
}

}   // namespace gtkui
}   // namespace midibn
