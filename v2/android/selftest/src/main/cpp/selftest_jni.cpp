// JNI library of the MIDIBottleneck Self-test app (Java side: org.midibottleneck.selftest.Native).
//
//  runProgram   runs one test program (libst_<name>.so, its main() exported) in a forked child process: the child changes
//               to the data folder, sends stdout and stderr to a result file, calls main(argc, argv) and exits with its
//               result. A crash or a hang therefore fails only that program (the parent kills it after the time limit).
//  loadMidi     loads a MIDI file with the portable core's loader and describes the result (file-picker check).
#include <jni.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "linux_platform.h"
#include "midibn/midi_loader.h"
#include "midibn/platform.h"

namespace {

std::string java_string(JNIEnv* env, jstring text)
{
    if (!text) return std::string();
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(text, chars);
    return result;
}

[[noreturn]] void child_main(const std::string& library, const std::vector<std::string>& args, const std::string& cwd,
    const std::string& outFile, const std::string& tmpDir)
{
    int fd = ::open(outFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); if (fd > 2) ::close(fd); }
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) { std::fprintf(stderr, "cannot change to %s\n", cwd.c_str()); std::fflush(nullptr); ::_exit(124); }
    if (!tmpDir.empty()) ::setenv("TMPDIR", tmpDir.c_str(), 1);
    void* handle = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) { std::fprintf(stderr, "dlopen %s failed: %s\n", library.c_str(), ::dlerror()); std::fflush(nullptr); ::_exit(125); }
    typedef int (*MainFunction)(int, char**);
    MainFunction entry = reinterpret_cast<MainFunction>(::dlsym(handle, "main"));
    if (!entry) { std::fprintf(stderr, "no main in %s: %s\n", library.c_str(), ::dlerror()); std::fflush(nullptr); ::_exit(126); }
    std::vector<std::string> storage;
    storage.push_back(library);
    for (size_t i = 0; i < args.size(); ++i) storage.push_back(args[i]);
    std::vector<char*> argv;
    for (size_t i = 0; i < storage.size(); ++i) argv.push_back(const_cast<char*>(storage[i].c_str()));
    argv.push_back(nullptr);
    int rc = entry(static_cast<int>(storage.size()), argv.data());
    std::fflush(nullptr);
    ::_exit(rc & 0xFF);
}

} // namespace

extern "C" {

// Returns the program's exit code (0 = success); 128 + signal when it crashed; -1 when fork failed; -2 on time out.
JNIEXPORT jint JNICALL Java_org_midibottleneck_selftest_Native_runProgram(JNIEnv* env, jclass, jstring library,
    jobjectArray args, jstring cwd, jstring outFile, jstring tmpDir, jint timeoutSeconds)
{
    std::string lib = java_string(env, library), dir = java_string(env, cwd), out = java_string(env, outFile), tmp = java_string(env, tmpDir);
    std::vector<std::string> argList;
    jsize n = args ? env->GetArrayLength(args) : 0;
    for (jsize i = 0; i < n; ++i) {
        jstring item = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        argList.push_back(java_string(env, item));
        env->DeleteLocalRef(item);
    }
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) child_main(lib, argList, dir, out, tmp);

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -3;
        }
        if (done < 0) return -3;
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds > timeoutSeconds) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -2;
        }
        struct timespec pause = { 0, 20 * 1000 * 1000 };
        ::nanosleep(&pause, nullptr);
    }
}

// "OK;format=1;tracks=3;division=480;events=1234;notes=600;duration_us=3000000;size=2345;load_ms=1.2"
// or "ERROR;<type>;<message>".
JNIEXPORT jstring JNICALL Java_org_midibottleneck_selftest_Native_loadMidi(JNIEnv* env, jclass, jstring path)
{
    std::string file = java_string(env, path);
    midibn::posix::PosixFileSystem files;
    std::string error;
    std::unique_ptr<midibn::platform::ByteSource> source = files.open_read(file, error);
    if (!source) return env->NewStringUTF(("ERROR;open;" + error).c_str());
    midibn::LoadOptions options;
    midibn::Song song;
    midibn::LoadError loadError;
    const int64_t begin = midibn::posix::performance_counter();
    bool ok = midibn::load_song(*source, options, song, loadError);
    const double ms = static_cast<double>(midibn::posix::performance_counter() - begin) / 1e6;
    if (!ok) return env->NewStringUTF(("ERROR;" + loadError.type + ";" + loadError.message).c_str());
    char text[400];
    std::snprintf(text, sizeof(text), "OK;format=%d;tracks=%d;division=%d;events=%lld;notes=%lld;duration_us=%lld;size=%lld;load_ms=%.1f",
        song.format, song.track_count, song.division, static_cast<long long>(song.store.count()), static_cast<long long>(song.note_count),
        static_cast<long long>(song.duration_us), static_cast<long long>(song.file_size), ms);
    return env->NewStringUTF(text);
}

} // extern "C"
