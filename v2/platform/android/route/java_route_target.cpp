// Java route target: a write is one JNI call into the Java MidiRoute (see route_targets.h).
#include "route_targets.h"

#include <mutex>
#include <pthread.h>

namespace midibn {
namespace android {

namespace {

JavaVM* g_vm = nullptr;
pthread_key_t g_detach_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

void detach_thread(void*)
{
    if (g_vm) g_vm->DetachCurrentThread();
}

void make_key() { pthread_key_create(&g_detach_key, detach_thread); }

// The environment of the calling thread; a native thread that the VM does not know yet is attached for its lifetime.
JNIEnv* env_for_this_thread()
{
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_once(&g_key_once, make_key);
    pthread_setspecific(g_detach_key, env);   // any non-null value: the destructor detaches when the thread ends
    return env;
}

class JavaRouteTarget : public RouteTarget {
public:
    JavaRouteTarget(JNIEnv* env, jobject route)
    {
        env->GetJavaVM(&g_vm);
        route_ = env->NewGlobalRef(route);
        jclass cls = env->GetObjectClass(route);
        sendBytes_ = env->GetMethodID(cls, "sendBytes", "([BII)Z");
        lastError_ = env->GetMethodID(cls, "lastError", "()Ljava/lang/String;");
        discard_ = env->GetMethodID(cls, "discardPending", "()V");
        isOpen_ = env->GetMethodID(cls, "isOpen", "()Z");
        open_ = env->GetMethodID(cls, "open", "(I)Ljava/lang/String;");
        listDevices_ = env->GetMethodID(cls, "listDevices", "()[Ljava/lang/String;");
        env->DeleteLocalRef(cls);
        jbyteArray array = env->NewByteArray(static_cast<jsize>(kMaxRouteWrite));
        array_ = static_cast<jbyteArray>(env->NewGlobalRef(array));
        env->DeleteLocalRef(array);
        if (env->ExceptionCheck()) env->ExceptionClear();
        ok_ = route_ && array_ && sendBytes_ && lastError_ && discard_ && isOpen_ && open_ && listDevices_;
    }

    ~JavaRouteTarget() override
    {
        JNIEnv* env = g_vm ? env_for_this_thread() : nullptr;
        if (!env) return;
        if (route_) env->DeleteGlobalRef(route_);
        if (array_) env->DeleteGlobalRef(array_);
    }

    bool valid() const { return ok_; }
    const char* name() const override { return "Java MIDI (android.media.midi)"; }

    std::vector<platform::OutputDeviceInfo> devices() const override
    {
        std::vector<platform::OutputDeviceInfo> list;
        JNIEnv* env = env_for_this_thread();
        if (!env) return list;
        jobjectArray names = static_cast<jobjectArray>(env->CallObjectMethod(route_, listDevices_));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return list; }
        if (!names) return list;
        jsize n = env->GetArrayLength(names);
        for (jsize i = 0; i < n; ++i) {
            platform::OutputDeviceInfo info;
            info.device_id = static_cast<uint32_t>(i);
            info.name = to_utf8(env, static_cast<jstring>(env->GetObjectArrayElement(names, i)));
            list.push_back(info);
        }
        env->DeleteLocalRef(names);
        return list;
    }

    bool open(uint32_t device_id, std::string& error) override
    {
        if (!ok_) { error = "The Java MIDI route is not available."; return false; }
        JNIEnv* env = env_for_this_thread();
        if (!env) { error = "The Java MIDI route is not available."; return false; }
        // The Java side normally opens the device (it needs a Looper and a wait); then the route is already open.
        if (env->CallBooleanMethod(route_, isOpen_) == JNI_TRUE) return true;
        if (env->ExceptionCheck()) env->ExceptionClear();
        jstring result = static_cast<jstring>(env->CallObjectMethod(route_, open_, static_cast<jint>(device_id)));
        if (env->ExceptionCheck()) { env->ExceptionClear(); error = "The MIDI device could not be opened."; return false; }
        if (result) { error = to_utf8(env, result); env->DeleteLocalRef(result); return false; }
        return true;
    }

    void close() override {}   // the Java side owns the device and closes it
    bool is_open() const override { return ok_; }

    bool write(const uint8_t* data, size_t length, std::string& error) override
    {
        JNIEnv* env = env_for_this_thread();
        if (!env || !ok_) { error = "The Java MIDI route is not available."; return false; }
        env->SetByteArrayRegion(array_, 0, static_cast<jsize>(length), reinterpret_cast<const jbyte*>(data));
        jboolean sent = env->CallBooleanMethod(route_, sendBytes_, array_, 0, static_cast<jint>(length));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            error = "The Java MIDI route threw an error while sending.";
            return false;
        }
        if (sent == JNI_TRUE) return true;
        jstring text = static_cast<jstring>(env->CallObjectMethod(route_, lastError_));
        if (env->ExceptionCheck()) env->ExceptionClear();
        error = text ? to_utf8(env, text) : std::string("The MIDI device did not accept the messages.");
        if (text) env->DeleteLocalRef(text);
        return false;
    }

    void discard_pending() override
    {
        JNIEnv* env = env_for_this_thread();
        if (!env || !ok_) return;
        env->CallVoidMethod(route_, discard_);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    std::string describe() const override { return "Android MIDI (Java route), batched: one call per burst."; }

private:
    static std::string to_utf8(JNIEnv* env, jstring text)
    {
        if (!text) return std::string();
        const char* chars = env->GetStringUTFChars(text, nullptr);
        std::string result = chars ? chars : "";
        if (chars) env->ReleaseStringUTFChars(text, chars);
        env->DeleteLocalRef(text);
        return result;
    }

    jobject route_ = nullptr;
    jbyteArray array_ = nullptr;
    jmethodID sendBytes_ = nullptr, lastError_ = nullptr, discard_ = nullptr, isOpen_ = nullptr, open_ = nullptr,
        listDevices_ = nullptr;
    bool ok_ = false;
};

std::mutex g_registry_lock;
std::shared_ptr<BatchingOutput> g_current;

} // namespace

std::unique_ptr<RouteTarget> make_java_route_target(JNIEnv* env, jobject route)
{
    std::unique_ptr<JavaRouteTarget> target(new JavaRouteTarget(env, route));
    if (!target->valid()) return nullptr;
    return std::unique_ptr<RouteTarget>(target.release());
}

std::shared_ptr<BatchingOutput> current_route_output()
{
    std::lock_guard<std::mutex> guard(g_registry_lock);
    return g_current;
}

void set_current_route_output(std::shared_ptr<BatchingOutput> output)
{
    std::lock_guard<std::mutex> guard(g_registry_lock);
    g_current = std::move(output);
}

bool amidi_route_compiled()
{
#ifdef MIDIBN_ROUTE_AMIDI
    return true;
#else
    return false;
#endif
}

} // namespace android
} // namespace midibn
