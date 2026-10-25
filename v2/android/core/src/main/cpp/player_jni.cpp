// JNI bridge of the Android player (work item U13). Java side: org.midibottleneck.core.PlayerBridge.
//
// Owns one midibn::app::PlayerController (the portable app layer: it does all the thinking, as on Windows and Linux).
// Everything here is a thin translation:
//   * the file comes in as a file descriptor (Android's file picker hands out content addresses, no paths);
//   * the output is a RoutedOutput (routed_output.h): device list and open go to the Java catalog, every send goes
//     straight to the native route output (no JNI call per message);
//   * the view of the controller leaves as ONE snapshot per refresh (a long[] and a String[] filled in one call).
//
// Threading: every call that touches the controller comes from the Android main thread (the controller's contract).
// The engine and the loader call wake_ui from their own threads; that calls PlayerBridge.onNativeWake(), which posts
// to the main thread, which calls nativePump().
#include <jni.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "linux_platform.h"
#include "midibn/app/help_text.h"
#include "midibn/app/player_controller.h"
#include "midibn/app/rate_model.h"
#include "midibn/format/display_format.h"
#include "midibn/midi_output.h"
#include "midibn/null_output.h"
#include "midibn/platform.h"
#include "midibn/sysex_assembler.h"
#include "routed_output.h"

namespace {

using midibn::platform::OutputErrorKind;
using midibn::platform::OutputStatus;

// ------------------------------------------------------------------------------------------------------ JNI helpers

JavaVM* g_vm = nullptr;
pthread_key_t g_detach_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

void detach_at_thread_exit(void*) {
    if (g_vm) g_vm->DetachCurrentThread();
}
void make_key() { pthread_key_create(&g_detach_key, detach_at_thread_exit); }

// The JNIEnv of the calling thread; a native thread (engine, loader) is attached once and detached when it exits.
JNIEnv* env_for_thread() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    pthread_once(&g_key_once, make_key);
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(g_detach_key, g_vm);   // non-null value: the destructor runs at thread exit
    return env;
}

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

// UTF-8 (the core's text) to a Java string without NewStringUTF (which mishandles 4-byte sequences).
jstring to_java(JNIEnv* env, const std::string& text) {
    std::vector<jchar> out;
    out.reserve(text.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        uint32_t c = p[i];
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (extra > 0 && i + static_cast<size_t>(extra) >= n) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (p[i + k] & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += static_cast<size_t>(extra) + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<jchar>(0xD800 + (c >> 10)));
            out.push_back(static_cast<jchar>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<jchar>(c));
        }
    }
    return env->NewString(out.data(), static_cast<jsize>(out.size()));
}

std::string from_java(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const jsize len = env->GetStringLength(s);
    const jchar* chars = env->GetStringChars(s, nullptr);
    std::string out;
    out.reserve(static_cast<size_t>(len));
    for (jsize i = 0; i < len; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && chars[i + 1] >= 0xDC00 && chars[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    env->ReleaseStringChars(s, chars);
    return out;
}

// ------------------------------------------------------------------------------------------------ files from a descriptor

// A byte source over a duplicate of a descriptor the file picker handed out (pread when it is a regular file; a pipe
// or other stream is read into memory once).
class FdSource : public midibn::platform::ByteSource {
public:
    FdSource(int fd, int64_t size) : fd_(fd), size_(size) {}
    explicit FdSource(std::vector<uint8_t> bytes) : fd_(-1), size_(static_cast<int64_t>(bytes.size())), bytes_(std::move(bytes)) {}
    ~FdSource() override { if (fd_ >= 0) ::close(fd_); }
    int64_t size() const override { return size_; }
    size_t read_at(int64_t offset, uint8_t* buffer, size_t count) override {
        if (offset < 0 || offset >= size_) return 0;
        count = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(count), size_ - offset));
        if (fd_ < 0) {
            std::memcpy(buffer, bytes_.data() + offset, count);
            return count;
        }
        size_t done = 0;
        while (done < count) {
            const ssize_t n = ::pread(fd_, buffer + done, count - done, static_cast<off_t>(offset + static_cast<int64_t>(done)));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        return done;
    }

private:
    int fd_;
    int64_t size_;
    std::vector<uint8_t> bytes_;
};

// Paths of the form "/saf/<descriptor>/<display name>" stand for descriptors held by the bridge.
class DescriptorFileSystem : public midibn::platform::FileSystem {
public:
    std::unique_ptr<midibn::platform::ByteSource> open_read(const std::string& path, std::string& error) override {
        int fd = -1;
        if (path.compare(0, 5, "/saf/") != 0 || std::sscanf(path.c_str() + 5, "%d", &fd) != 1 || fd < 0) {
            error = "Could not open the file: it was not chosen through the file picker.";
            return nullptr;
        }
        const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0) {
            error = "Could not open the chosen file (system error " + std::to_string(errno) + ").";
            return nullptr;
        }
        struct stat info;
        if (::fstat(copy, &info) == 0 && S_ISREG(info.st_mode))
            return std::unique_ptr<midibn::platform::ByteSource>(new FdSource(copy, static_cast<int64_t>(info.st_size)));
        // Not a regular file (a document provider streaming from the cloud): read it all once.
        std::vector<uint8_t> bytes;
        uint8_t block[65536];
        for (;;) {
            const ssize_t n = ::read(copy, block, sizeof(block));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            bytes.insert(bytes.end(), block, block + n);
        }
        ::close(copy);
        if (bytes.empty()) {
            error = "The chosen file is empty or could not be read.";
            return nullptr;
        }
        return std::unique_ptr<midibn::platform::ByteSource>(new FdSource(std::move(bytes)));
    }
};

// ------------------------------------------------------------------------------------------------ output: the route catalog

// The Java side of the device list and of open/close (org.midibottleneck.core.PlayerBridge.OutputCatalog, implemented by
// the player's RouteCatalog). Sends never come here: RoutedOutput (routed_output.h) forwards them to the native
// BatchingOutput. Calls come from the controller's thread (the UI thread); open_device may block for seconds in Java.
class JavaCatalog : public midibn::android::RouteCatalog {
public:
    JavaCatalog(JNIEnv* env, jobject catalog) {
        catalog_ = env->NewGlobalRef(catalog);
        jclass cls = env->GetObjectClass(catalog);
        route_count_ = env->GetMethodID(cls, "routeCount", "()I");
        list_ = env->GetMethodID(cls, "list", "(I)[Ljava/lang/String;");
        open_ = env->GetMethodID(cls, "open", "(IILjava/lang/String;)Ljava/lang/String;");
        close_ = env->GetMethodID(cls, "close", "(I)V");
        env->DeleteLocalRef(cls);
        clear_exception(env);
    }
    ~JavaCatalog() override {
        if (JNIEnv* env = env_for_thread()) env->DeleteGlobalRef(catalog_);
    }

    size_t route_count() override {
        JNIEnv* env = env_for_thread();
        if (!env) return 0;
        jint n = env->CallIntMethod(catalog_, route_count_);
        if (clear_exception(env) || n < 0) return 0;
        return static_cast<size_t>(n);
    }

    std::vector<std::string> list_devices(size_t route) override {
        std::vector<std::string> result;
        JNIEnv* env = env_for_thread();
        if (!env) return result;
        jobjectArray names = static_cast<jobjectArray>(env->CallObjectMethod(catalog_, list_, static_cast<jint>(route)));
        if (clear_exception(env) || !names) return result;
        const jsize n = env->GetArrayLength(names);
        for (jsize i = 0; i < n; ++i) {
            jstring s = static_cast<jstring>(env->GetObjectArrayElement(names, i));
            result.push_back(from_java(env, s));
            if (s) env->DeleteLocalRef(s);
        }
        env->DeleteLocalRef(names);
        return result;
    }

    std::string open_device(size_t route, size_t index, const std::string& name) override {
        const char* generic = "The MIDI device could not be opened.";
        JNIEnv* env = env_for_thread();
        if (!env) return generic;
        jstring jname = to_java(env, name);
        jstring err = static_cast<jstring>(env->CallObjectMethod(catalog_, open_, static_cast<jint>(route), static_cast<jint>(index), jname));
        if (jname) env->DeleteLocalRef(jname);
        if (clear_exception(env)) return generic;
        if (!err) return std::string();
        std::string text = from_java(env, err);
        env->DeleteLocalRef(err);
        return text.empty() ? std::string(generic) : text;
    }

    void close_route(size_t route) override {
        if (JNIEnv* env = env_for_thread()) {
            env->CallVoidMethod(catalog_, close_, static_cast<jint>(route));
            clear_exception(env);
        }
    }

private:
    jobject catalog_ = nullptr;
    jmethodID route_count_ = nullptr, list_ = nullptr, open_ = nullptr, close_ = nullptr;
};

// ------------------------------------------------------------------------------------------------ the player

// Shared between the player and the wake callback so a late wake from a worker thread never touches a dead object.
struct WakeLink {
    std::mutex mutex;
    jobject bridge = nullptr;      // global reference to the PlayerBridge; null once the player is gone
    jmethodID method = nullptr;    // PlayerBridge.onNativeWake()
};

struct Player {
    std::shared_ptr<WakeLink> link;
    std::unique_ptr<midibn::platform::FileSystem> files;
    std::unique_ptr<midibn::app::PlayerController> controller;
    std::vector<int> fds;          // descriptors handed in by the file picker; the last one is the current file
};

Player* player_of(jlong handle) { return reinterpret_cast<Player*>(handle); }

void close_retired_descriptors(Player* p, bool all) {
    if (p->fds.empty()) return;
    const size_t keep = all ? 0 : 1;
    if (!all && p->controller && p->controller->loading()) return;
    while (p->fds.size() > keep) {
        ::close(p->fds.front());
        p->fds.erase(p->fds.begin());
    }
}

// Indices into the snapshot arrays; they mirror the constants in PlayerBridge.java (keep in step).
enum Num {
    N_LOADING, N_LOAD_OVERALL, N_LOAD_STAGE, N_OUTPUT_SELECTED, N_OUTPUT_ENABLED,
    N_QUEUE_CHECKED, N_QUEUE_ENABLED, N_QUEUE_VALUE, N_QUEUE_MIN, N_QUEUE_MAX, N_QUEUE_VALUE_ENABLED,
    N_OVERFLOW_SELECTED, N_OVERFLOW_ENABLED, N_RATE_CHOICE, N_RATE_FORWARD,
    N_PROC_VALUE, N_PROC_MIN, N_PROC_MAX, N_PROC_ENABLED, N_PROC_VISIBLE, N_UNIT_VISIBLE,
    N_DIN_VISIBLE, N_DIN_ENABLED, N_SLIDER_VALUE, N_SLIDER_ENABLED,
    N_PLAY_ENABLED, N_STOP_ENABLED, N_SEEK_ENABLED, N_POSITION_US, N_DURATION_US,
    N_QUEUE_LIMITED, N_VIRTUAL_QUEUE, N_AGE_PRESSURE, N_OCCUPIED, N_LIMIT, N_OVERFLOW_PULSE,
    N_NEEDS_REFRESH, N_HAS_SONG, N_STATE, N_ROUTING_AVAILABLE,
    N_APPLY_QUEUE_CHECKED, N_APPLY_QUEUE_ENABLED, N_QUEUE_AGE_CHECKED, N_QUEUE_AGE_ENABLED,
    N_CHASE_CHECKED, N_CHASE_ENABLED, N_PRESERVE_CHECKED, N_PRESERVE_ENABLED,
    N_PACED_CHECKED, N_PACED_ENABLED, N_SCALE_CHECKED, N_SCALE_ENABLED, N_CURRENT_LAG_US,
    N_COUNT
};
enum Text {
    T_OPEN, T_FILE, T_FILE_INFO, T_LOAD_STATUS, T_STATE, T_OUTPUT_ITEMS, T_OVERFLOW_ITEMS,
    T_QUEUE_CHECK, T_EVENTS_LABEL, T_OVERFLOW_LABEL, T_RATE_LABEL, T_SERVICE_LABEL, T_SERVICE_UNIT,
    T_PLAY,
    // The statistics cells as the desktop Analysis window's Playback statistics list them, then the maximum rate.
    T_STAT_PLAYBACK, T_STAT_TIMELINE, T_STAT_QUEUE, T_STAT_EVENTS, T_STAT_OUTPUT_RATE, T_STAT_SPEED, T_STAT_LAG, T_STAT_MAX_RATE,
    T_PRESSURE, T_PRESSURE_TIP, T_RATE_ITEMS, T_RATE_TIP, T_QUEUE_TIP, T_OVERFLOW_TIP, T_SLIDER_TIP, T_PLAY_TIP,
    T_FILE_TIP, T_NO_SONG,
    T_COUNT
};

std::string join_lines(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += '\n';
        out += items[i];
    }
    return out;
}

} // namespace

extern "C" {

// ---- creation and destruction

JNIEXPORT jlong JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeCreate(JNIEnv* env, jobject self, jobject catalog) {
    if (!g_vm) env->GetJavaVM(&g_vm);
    std::unique_ptr<Player> player(new Player());
    player->link = std::make_shared<WakeLink>();
    player->link->bridge = env->NewGlobalRef(self);
    jclass cls = env->GetObjectClass(self);
    player->link->method = env->GetMethodID(cls, "onNativeWake", "()V");
    env->DeleteLocalRef(cls);

    player->files.reset(new DescriptorFileSystem());
    midibn::app::PlayerEnvironment pe;
    pe.services = std::make_shared<midibn::posix::PosixEngineServices>();
    pe.files = player->files.get();
    if (catalog) {
        // One output object serves the device list and the output slot (the controller keeps it). It lists and opens
        // through the Java catalog; every send goes straight into the native route output (routed_output.h).
        std::shared_ptr<midibn::android::RouteCatalog> routes(new JavaCatalog(env, catalog));
        std::shared_ptr<midibn::platform::MidiOutput> output(new midibn::android::RoutedOutput(routes));
        pe.list_devices = [output] { return output->enumerate(); };
        pe.create_winmm = [output] { return output; };
    } else {
        pe.list_devices = [] { return std::vector<midibn::platform::OutputDeviceInfo>(); };
    }
    pe.defer_device_listing = false;
    pe.pointer_size = static_cast<int>(sizeof(void*));
    pe.private_bytes = [] { return static_cast<int64_t>(midibn::posix::process_memory().current_private); };
    std::shared_ptr<WakeLink> link = player->link;
    pe.wake_ui = [link] {
        std::lock_guard<std::mutex> lock(link->mutex);
        if (!link->bridge) return;
        JNIEnv* e = env_for_thread();
        if (!e) return;
        e->CallVoidMethod(link->bridge, link->method);
        clear_exception(e);
    };
    player->controller.reset(new midibn::app::PlayerController(pe));
    player->controller->set_compact(true);   // the phone layout uses the short statistics and file-information forms
    return reinterpret_cast<jlong>(player.release());
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeDestroy(JNIEnv* env, jobject, jlong handle) {
    Player* p = player_of(handle);
    if (!p) return;
    {   // no wake call can reach Java after this
        std::lock_guard<std::mutex> lock(p->link->mutex);
        if (p->link->bridge) env->DeleteGlobalRef(p->link->bridge);
        p->link->bridge = nullptr;
    }
    p->controller.reset();   // stops the engine
    close_retired_descriptors(p, true);
    delete p;
}

JNIEXPORT jboolean JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeShutdown(JNIEnv*, jobject, jlong handle) {
    Player* p = player_of(handle);
    return p->controller->shutdown() ? JNI_TRUE : JNI_FALSE;
}

// ---- events

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativePump(JNIEnv*, jobject, jlong handle) {
    player_of(handle)->controller->pump();
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeOpenFile(JNIEnv* env, jobject, jlong handle, jint fd, jstring name) {
    Player* p = player_of(handle);
    std::string display = from_java(env, name);
    std::replace(display.begin(), display.end(), '/', '_');
    std::replace(display.begin(), display.end(), '\\', '_');
    if (display.empty()) display = "MIDI file";
    p->fds.push_back(static_cast<int>(fd));
    p->controller->open_file("/saf/" + std::to_string(static_cast<int>(fd)) + "/" + display);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeCancelLoad(JNIEnv*, jobject, jlong handle) {
    player_of(handle)->controller->cancel_load();
}

// Returns {title, heading, text} when a large-file decision is waiting, else null.
JNIEXPORT jobjectArray JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeLargeFileDecision(JNIEnv* env, jobject, jlong handle) {
    std::string title, heading, text;
    if (!player_of(handle)->controller->large_file_decision_pending(title, heading, text)) return nullptr;
    jobjectArray array = env->NewObjectArray(3, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(array, 0, to_java(env, title));
    env->SetObjectArrayElement(array, 1, to_java(env, heading));
    env->SetObjectArrayElement(array, 2, to_java(env, text));
    return array;
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeAnswerLargeFile(JNIEnv*, jobject, jlong handle, jboolean accepted) {
    player_of(handle)->controller->answer_large_file(accepted == JNI_TRUE);
}

// Returns the queued messages as {kind, title, text} triples (kind "0" information, "1" warning, "2" error).
JNIEXPORT jobjectArray JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeTakeMessages(JNIEnv* env, jobject, jlong handle) {
    std::vector<midibn::app::UiMessage> messages = player_of(handle)->controller->take_messages();
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(messages.size() * 3), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < messages.size(); ++i) {
        env->SetObjectArrayElement(array, static_cast<jsize>(i * 3), to_java(env, std::to_string(static_cast<int>(messages[i].kind))));
        env->SetObjectArrayElement(array, static_cast<jsize>(i * 3 + 1), to_java(env, messages[i].title));
        env->SetObjectArrayElement(array, static_cast<jsize>(i * 3 + 2), to_java(env, messages[i].text));
    }
    return array;
}

// ---- actions

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeAction(JNIEnv*, jobject, jlong handle, jint action, jlong argument) {
    midibn::app::PlayerController& c = *player_of(handle)->controller;
    switch (action) {
    case 0: c.play_clicked(); break;
    case 1: c.stop_clicked(); break;
    case 2: c.seek_by(argument); break;                 // microseconds, signed
    case 3: c.seek_to(argument); break;                 // microseconds
    case 4: c.set_timeline_dragging(argument != 0); break;
    case 5: c.select_output(static_cast<int>(argument)); break;
    case 6: c.select_rate_choice(static_cast<midibn::app::RateModelChoice>(argument)); break;
    case 7: c.set_queue_limit_checked(argument != 0); break;
    case 8: c.set_queue_value(argument); break;
    case 9: c.set_overflow_selection(static_cast<int>(argument)); break;
    case 10: c.set_processing_value(argument); break;
    case 11: c.set_slider(static_cast<int>(argument)); break;
    case 12: c.din_preset(); break;
    case 13: c.reset_statistics_clicked(); break;
    case 14: c.toggle_apply_queue_limit_without_slowdown(); break;
    case 15: c.toggle_queue_age_limit(); break;
    case 16: c.toggle_state_chase(); break;
    case 17: c.toggle_preserve_pause_seek(); break;
    case 18: c.toggle_output_paced_timeline(); break;
    case 19: c.toggle_scale_note_interval_by_traffic(); break;
    case 20: c.refresh_devices(); break;
    default: break;
    }
}

// ---- the snapshot: one call per refresh fills a long[] and a String[] (indices: PlayerBridge.N_* and T_*).

JNIEXPORT void JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeSnapshot(JNIEnv* env, jobject, jlong handle, jlongArray numbers, jobjectArray texts) {
    Player* p = player_of(handle);
    midibn::app::PlayerController& c = *p->controller;
    c.refresh();
    close_retired_descriptors(p, false);
    const midibn::app::PlayerView& v = c.view();
    jlong n[N_COUNT] = {};
    n[N_LOADING] = v.loading;
    n[N_LOAD_OVERALL] = v.load_overall_permille;
    n[N_LOAD_STAGE] = v.load_stage_permille;
    n[N_OUTPUT_SELECTED] = v.output_selected;
    n[N_OUTPUT_ENABLED] = v.output_enabled;
    n[N_QUEUE_CHECKED] = v.queue_check_checked;
    n[N_QUEUE_ENABLED] = v.queue_check_enabled;
    n[N_QUEUE_VALUE] = v.queue_value.value;
    n[N_QUEUE_MIN] = v.queue_value.minimum;
    n[N_QUEUE_MAX] = v.queue_value.maximum;
    n[N_QUEUE_VALUE_ENABLED] = v.queue_value.enabled;
    n[N_OVERFLOW_SELECTED] = v.overflow_selected;
    n[N_OVERFLOW_ENABLED] = v.overflow_enabled;
    n[N_RATE_CHOICE] = static_cast<int>(v.rate_choice);
    n[N_RATE_FORWARD] = v.rate_forward_heading;
    n[N_PROC_VALUE] = v.processing_value.value;
    n[N_PROC_MIN] = v.processing_value.minimum;
    n[N_PROC_MAX] = v.processing_value.maximum;
    n[N_PROC_ENABLED] = v.processing_value.enabled;
    n[N_PROC_VISIBLE] = v.processing_value.visible;
    n[N_UNIT_VISIBLE] = v.service_unit_visible;
    n[N_DIN_VISIBLE] = v.din_visible;
    n[N_DIN_ENABLED] = v.din_enabled;
    n[N_SLIDER_VALUE] = v.slider_value;
    n[N_SLIDER_ENABLED] = v.slider_enabled;
    n[N_PLAY_ENABLED] = v.play_enabled;
    n[N_STOP_ENABLED] = v.stop_enabled;
    n[N_SEEK_ENABLED] = v.seek_enabled;
    n[N_POSITION_US] = v.position_us;
    n[N_DURATION_US] = v.duration_us;
    n[N_QUEUE_LIMITED] = v.statistics.queue_limited;
    n[N_VIRTUAL_QUEUE] = v.statistics.virtual_queue;
    n[N_AGE_PRESSURE] = v.statistics.age_pressure;
    n[N_OCCUPIED] = v.statistics.occupied;
    n[N_LIMIT] = v.statistics.limit;
    n[N_OVERFLOW_PULSE] = v.statistics.overflow_pulse;
    n[N_NEEDS_REFRESH] = c.needs_periodic_refresh();
    n[N_HAS_SONG] = c.has_song();
    n[N_STATE] = 0;   // set below from the state text
    n[N_ROUTING_AVAILABLE] = c.track_routing_available();
    n[N_APPLY_QUEUE_CHECKED] = v.menu.apply_queue_limit_checked;
    n[N_APPLY_QUEUE_ENABLED] = v.menu.apply_queue_limit_enabled;
    n[N_QUEUE_AGE_CHECKED] = v.menu.queue_age_checked;
    n[N_QUEUE_AGE_ENABLED] = v.menu.queue_age_enabled;
    n[N_CHASE_CHECKED] = v.menu.chase_checked;
    n[N_CHASE_ENABLED] = v.menu.chase_enabled;
    n[N_PRESERVE_CHECKED] = v.menu.preserve_checked;
    n[N_PRESERVE_ENABLED] = v.menu.preserve_enabled;
    n[N_PACED_CHECKED] = v.menu.output_paced_checked;
    n[N_PACED_ENABLED] = v.menu.output_paced_enabled;
    n[N_SCALE_CHECKED] = v.menu.scale_interval_checked;
    n[N_SCALE_ENABLED] = v.menu.scale_interval_enabled;
    // Engine state as a number for the service: 1 playing, 2 paused, 0 otherwise (the play button shows "Pause" while playing).
    const midibn::engine::PlaybackState engine_state = c.engine().state();
    n[N_STATE] = engine_state == midibn::engine::PlaybackState::Playing ? 1
        : engine_state == midibn::engine::PlaybackState::Paused ? 2 : 0;
    // One engine snapshot for the Analysis-window statistics below (the same quantities and wording as the desktop
    // Analysis window's Playback statistics, analysis_window_model.cpp update_playback_state) and the lag.
    const midibn::engine::PlaybackSnapshot live = c.engine().snapshot();
    n[N_CURRENT_LAG_US] = live.current_lag_microseconds;
    env->SetLongArrayRegion(numbers, 0, N_COUNT, n);

    std::vector<std::string> rate_items;
    for (int i = 0; i < midibn::app::kRateModelChoiceCount; ++i)
        rate_items.push_back(midibn::app::rate_choice_full_label(static_cast<midibn::app::RateModelChoice>(i)));
    std::string t[T_COUNT];
    t[T_OPEN] = v.open_button_text;
    t[T_FILE] = v.file_text;
    std::string info = v.file_info;
    info.erase(std::remove(info.begin(), info.end(), '\r'), info.end());
    t[T_FILE_INFO] = info;
    t[T_LOAD_STATUS] = v.load_status;
    t[T_STATE] = v.state_text;
    t[T_OUTPUT_ITEMS] = join_lines(v.output_items);
    t[T_OVERFLOW_ITEMS] = join_lines(v.overflow_items);
    t[T_QUEUE_CHECK] = v.queue_check_text;
    t[T_EVENTS_LABEL] = v.events_label;
    t[T_OVERFLOW_LABEL] = v.overflow_label;
    t[T_RATE_LABEL] = v.rate_label;
    t[T_SERVICE_LABEL] = v.service_value_label;
    t[T_SERVICE_UNIT] = v.service_unit;
    t[T_PLAY] = v.play_text;
    t[T_STAT_PLAYBACK] = midibn::engine::playback_state_name(live.state);
    t[T_STAT_TIMELINE] = midibn::format::format_timeline_output(live.intended_timeline_microseconds,
        live.last_dispatched_timeline_microseconds, false);
    t[T_STAT_QUEUE] = midibn::format::format_analysis_queue(live, c.culture());
    t[T_STAT_EVENTS] = midibn::format::format_event_counts(live, false, c.culture());
    t[T_STAT_OUTPUT_RATE] = c.output_rate_text();
    t[T_STAT_SPEED] = c.effective_speed_text();
    t[T_STAT_LAG] = midibn::format::format_analysis_lag(live, c.culture());
    t[T_STAT_MAX_RATE] = v.statistics.values[2];   // the main window's maximum-rate cell (the compact form); the Analysis list has none
    if (v.statistics.queue_limited) {
        t[T_PRESSURE] = midibn::format::format_queue_pressure_text(v.statistics.virtual_queue, v.statistics.age_pressure,
            v.statistics.occupied, v.statistics.limit, true, c.culture());
        t[T_PRESSURE_TIP] = midibn::format::queue_pressure_tooltip(v.statistics.virtual_queue, v.statistics.age_pressure,
            v.statistics.occupied, v.statistics.limit, c.culture());
    }
    t[T_RATE_ITEMS] = join_lines(rate_items);
    t[T_RATE_TIP] = v.rate_tooltip;
    t[T_QUEUE_TIP] = v.queue_check_tooltip;
    t[T_OVERFLOW_TIP] = v.overflow_tooltip;
    t[T_SLIDER_TIP] = v.slider_tooltip;
    t[T_PLAY_TIP] = v.play_tooltip;
    t[T_FILE_TIP] = v.file_tooltip;
    t[T_NO_SONG] = c.has_song() ? std::string() : c.no_song_message();
    for (int i = 0; i < T_COUNT; ++i) {
        jstring s = to_java(env, t[i]);
        env->SetObjectArrayElement(texts, i, s);
        env->DeleteLocalRef(s);
    }
}

// ---- Help (static: no player needed)

JNIEXPORT jint JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeHelpTopicCount(JNIEnv*, jclass) {
    return midibn::app::kHelpTopicCount;
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeHelpTitle(JNIEnv* env, jclass, jint topic) {
    return to_java(env, midibn::app::help_topic_title(static_cast<midibn::app::HelpTopic>(topic)));
}

// The shared Help markup (see help_text.h), in the Android wording.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_PlayerBridge_nativeHelpText(JNIEnv* env, jclass, jint topic) {
    return to_java(env, midibn::app::help_text(static_cast<midibn::app::HelpTopic>(topic), midibn::app::HelpPlatform::Android));
}

} // extern "C"
