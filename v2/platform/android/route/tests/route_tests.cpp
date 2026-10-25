// Tests of the Android route layer against the stand-in target (no device, no JNI, no sound):
// message encoding, batching (one write per burst), SysEx assembly and chunking, reset, panic, failure handling.
// Built by v2/android/run-native-tests.ps1 (and runnable on any host with the core sources).
#include "fake_route_target.h"
#include "route_output.h"
// RoutedOutput (the player's output) is header only and lives next to player_jni.cpp; it needs no JNI call to be tested.
#include "../../../../android/core/src/main/cpp/routed_output.h"

#include <cstdio>
#include <memory>

using namespace midibn;
using namespace midibn::android;
using midibn::platform::OutputStatus;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

struct Rig {
    FakeRouteTarget* fake;
    std::unique_ptr<BatchingOutput> out;
    Rig()
    {
        std::unique_ptr<FakeRouteTarget> f(new FakeRouteTarget());
        fake = f.get();
        out.reset(new BatchingOutput(std::move(f)));
    }
};

static uint32_t pack(unsigned status, unsigned d1, unsigned d2) { return status | (d1 << 8) | (d2 << 16); }

static output::SystemExclusiveEvent sysex(uint8_t status, const std::vector<uint8_t>& bytes)
{
    output::SystemExclusiveEvent e;
    e.status = status;
    e.data = bytes.data();
    e.length = bytes.size();
    return e;
}

static void test_encoding()
{
    uint8_t b[3];
    CHECK(encode_short_message(pack(0x90, 60, 100), b) == 3 && b[0] == 0x90 && b[1] == 60 && b[2] == 100);
    CHECK(encode_short_message(pack(0x80, 60, 0), b) == 3);
    CHECK(encode_short_message(pack(0xA5, 60, 9), b) == 3);
    CHECK(encode_short_message(pack(0xB3, 7, 127), b) == 3 && b[0] == 0xB3);
    CHECK(encode_short_message(pack(0xC2, 5, 99), b) == 2 && b[1] == 5);
    CHECK(encode_short_message(pack(0xD2, 77, 99), b) == 2 && b[1] == 77);
    CHECK(encode_short_message(pack(0xEF, 0x7F, 0x7F), b) == 3 && b[1] == 0x7F && b[2] == 0x7F);
    CHECK(encode_short_message(pack(0x90, 0xFF, 0xFF), b) == 3 && b[1] == 0x7F && b[2] == 0x7F);   // data bytes masked to 7 bits
    CHECK(encode_short_message(0xF1 | (0x12 << 8), b) == 2 && b[1] == 0x12);
    CHECK(encode_short_message(pack(0xF2, 1, 2), b) == 3);
    CHECK(encode_short_message(pack(0xF3, 4, 0), b) == 2);
    const unsigned oneByte[] = {0xF6, 0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
    for (unsigned s : oneByte) CHECK(encode_short_message(s, b) == 1 && b[0] == s);
    const unsigned rejected[] = {0x00, 0x40, 0x7F, 0xF0, 0xF4, 0xF5, 0xF7, 0xF9, 0xFD};
    for (unsigned s : rejected) CHECK(encode_short_message(s | (1 << 8), b) == 0);
}

static void test_open_close()
{
    Rig r;
    OutputStatus st = r.out->send_short(pack(0x90, 60, 64));
    CHECK(!st.ok() && st.kind == platform::OutputErrorKind::InvalidOperation);
    CHECK(r.out->open(0).ok() && r.out->is_open());
    CHECK(r.out->send_short(pack(0x90, 60, 64)).ok());
    CHECK(r.fake->writes.size() == 1 && r.fake->writes[0].size() == 3);
    CHECK(r.out->close().ok() && !r.out->is_open());
    CHECK(!r.out->send_short(pack(0x90, 60, 64)).ok());
    CHECK(r.out->open(0).ok() && r.out->send_short(pack(0x90, 61, 64)).ok());
    CHECK(r.out->enumerate().size() == 1);
    CHECK(r.out->panic().ok() == true);

    Rig bad;
    bad.fake->open_fails = true;
    OutputStatus o = bad.out->open(0);
    CHECK(!o.ok() && o.message == "Fake open failure." && !bad.out->is_open());
}

static void test_unbatched_one_write_each()
{
    Rig r;
    r.out->open(0);
    for (int i = 0; i < 10; ++i) CHECK(r.out->send_short(pack(0x90, i, 64)).ok());
    CHECK(r.fake->writes.size() == 10);
}

static void test_batching()
{
    Rig r;
    r.out->open(0);
    {
        platform::SendBatch batch(r.out.get());
        for (int i = 0; i < 100; ++i) {
            platform::OutputStatus f;
            CHECK(r.out->try_send_short(pack(i % 2 ? 0x80 : 0x90, i, 64), f));
        }
        CHECK(r.fake->writes.empty());              // nothing leaves before the burst ends
        CHECK(r.out->pending_bytes() == 300);
    }
    CHECK(r.fake->writes.size() == 1 && r.fake->writes[0].size() == 300);   // one write for the burst
    CHECK(r.fake->writes[0][0] == 0x90 && r.fake->writes[0][1] == 0 && r.fake->writes[0][3] == 0x80 && r.fake->writes[0][4] == 1);

    // many bursts: one write each
    for (int b = 0; b < 50; ++b) {
        platform::SendBatch batch(r.out.get());
        for (int i = 0; i < 20; ++i) r.out->send_short(pack(0xB0, 20, i));
    }
    CHECK(r.fake->writes.size() == 51);

    // an empty burst writes nothing
    { platform::SendBatch batch(r.out.get()); }
    CHECK(r.fake->writes.size() == 51);
}

static void test_big_burst_splits_on_message_boundaries()
{
    Rig r;
    r.out->open(0);
    std::vector<uint8_t> expected;
    {
        platform::SendBatch batch(r.out.get());
        for (int i = 0; i < 2000; ++i) {
            uint32_t m = (i % 3 == 0) ? pack(0xC0, i & 127, 0) : pack(0x90, i & 127, (i * 7) & 127);
            uint8_t b[3];
            size_t n = encode_short_message(m, b);
            expected.insert(expected.end(), b, b + n);
            r.out->send_short(m);
        }
    }
    CHECK(r.fake->writes.size() >= 2);
    for (const auto& w : r.fake->writes) {
        CHECK(w.size() <= kMaxRouteWrite);
        CHECK(w[0] == 0xC0 || w[0] == 0x90);       // every write starts at a message boundary
    }
    CHECK(r.fake->all_bytes() == expected);
}

static void test_sysex()
{
    Rig r;
    r.out->open(0);
    // order: a buffered short message goes out before the SysEx message
    r.out->begin_send_batch();
    r.out->send_short(pack(0x90, 60, 64));
    std::vector<uint8_t> a = {0xF0, 1, 2, 3};
    CHECK(r.out->send_long(sysex(0xF0, a)).ok());       // unfinished: nothing sent
    CHECK(r.fake->writes.size() == 0);
    std::vector<uint8_t> b = {4, 5, 0xF7};
    CHECK(r.out->send_long(sysex(0xF7, b)).ok());       // completes the packet
    CHECK(r.fake->writes.size() == 2);
    CHECK(r.fake->writes[0] == std::vector<uint8_t>({0x90, 60, 64}));
    CHECK(r.fake->writes[1] == std::vector<uint8_t>({0xF0, 1, 2, 3, 4, 5, 0xF7}));
    r.out->end_send_batch();
    CHECK(r.fake->writes.size() == 2);

    CHECK(r.out->send_long(sysex(0xF0, std::vector<uint8_t>())).ok());   // zero length ignored
    CHECK(r.fake->writes.size() == 2);

    // a complete message in one event
    std::vector<uint8_t> whole = {0xF0, 0x7E, 0x7F, 9, 1, 0xF7};
    CHECK(r.out->send_long(sysex(0xF0, whole)).ok());
    CHECK(r.fake->writes.back() == whole);

    // a long message is cut into pieces, in order
    std::vector<uint8_t> big(2500, 0x55);
    big.front() = 0xF0;
    big.back() = 0xF7;
    size_t before = r.fake->writes.size();
    CHECK(r.out->send_long(sysex(0xF0, big)).ok());
    CHECK(r.fake->writes.size() - before == 3);
    std::vector<uint8_t> joined;
    for (size_t i = before; i < r.fake->writes.size(); ++i) {
        CHECK(r.fake->writes[i].size() <= kMaxRouteWrite);
        joined.insert(joined.end(), r.fake->writes[i].begin(), r.fake->writes[i].end());
    }
    CHECK(joined == big);
}

static void test_reset_and_panic()
{
    Rig r;
    r.out->open(0);
    r.out->begin_send_batch();
    r.out->send_short(pack(0x90, 60, 64));
    std::vector<uint8_t> a = {0xF0, 1, 2};
    r.out->send_long(sysex(0xF0, a));
    CHECK(r.out->reset().ok());
    CHECK(r.out->pending_bytes() == 0 && r.fake->discard_calls == 1);
    std::vector<uint8_t> tail = {3, 0xF7};
    CHECK(r.out->send_long(sysex(0xF7, tail)).ok());   // the unfinished packet was dropped: a bare tail is not sent as a complete message
    r.out->end_send_batch();
    // the F7 tail alone is not complete: nothing written at all
    CHECK(r.fake->writes.empty());

    CHECK(r.out->panic().ok());
    CHECK(r.fake->writes.size() == 1 && r.fake->writes[0].size() == 144);
    const std::vector<uint8_t>& p = r.fake->writes[0];
    CHECK(p[0] == 0xB0 && p[1] == 120 && p[2] == 0 && p[3] == 0xB0 && p[4] == 123 && p[6] == 0xB0 && p[7] == 64 && p[8] == 0);
    CHECK(p[141] == 0xBF && p[142] == 64 && p[143] == 0);

    CHECK(platform::reset_and_silence(r.out.get()).ok());
    CHECK(r.fake->writes.size() == 2 && r.fake->discard_calls == 2);

    Rig closed;
    CHECK(closed.out->panic().ok() && closed.out->reset().ok());   // closed output: nothing to do
}

static void test_failures()
{
    // the device disappears during a burst: the failure surfaces on the next call and stays until reopened
    Rig r;
    r.out->open(0);
    r.out->send_short(pack(0x90, 60, 64));          // write 0 works
    r.fake->fail_after_writes = 1;
    r.out->begin_send_batch();
    platform::OutputStatus f;
    CHECK(r.out->try_send_short(pack(0x90, 61, 64), f));   // buffered
    r.out->end_send_batch();                                // the write fails here
    CHECK(!r.out->try_send_short(pack(0x90, 62, 64), f));
    CHECK(f.kind == platform::OutputErrorKind::IoError && f.message == "Fake device unplugged.");
    OutputStatus again = r.out->send_short(pack(0x90, 63, 64));
    CHECK(!again.ok() && again.message == f.message);       // send_short and try_send_short report the same status
    CHECK(r.fake->writes.size() == 1);                      // no further attempts on the dead device
    CHECK(!r.out->panic().ok());
    CHECK(r.out->reset().ok());                             // reset never fails
    CHECK(r.out->close().ok());
    r.fake->fail_after_writes = -1;
    CHECK(r.out->open(0).ok() && r.out->send_short(pack(0x90, 60, 64)).ok());

    // unplug reported from another thread's callback
    Rig l;
    l.out->open(0);
    l.out->set_lost("The MIDI device was unplugged.");
    OutputStatus lost = l.out->send_short(pack(0x90, 60, 64));
    CHECK(!lost.ok() && lost.message == "The MIDI device was unplugged.");
    CHECK(l.fake->writes.empty());
    CHECK(l.out->send_long(sysex(0xF0, std::vector<uint8_t>({0xF0, 1, 0xF7}))).message == "The MIDI device was unplugged.");

    // invalid message
    Rig v;
    v.out->open(0);
    OutputStatus bad = v.out->send_short(0x40);
    CHECK(!bad.ok() && bad.kind == platform::OutputErrorKind::InvalidOperation);

    // a failed write in the middle of an oversized burst is reported by the call that triggered it
    Rig m;
    m.out->open(0);
    m.fake->fail_after_writes = 0;
    m.out->begin_send_batch();
    bool failedSomewhere = false;
    for (int i = 0; i < 1000; ++i) {
        platform::OutputStatus s;
        if (!m.out->try_send_short(pack(0x90, 60, 64), s)) { failedSomewhere = true; break; }
    }
    CHECK(failedSomewhere);
    m.out->end_send_batch();
}

static void test_dense_file_write_count()
{
    // 100,000 messages in 400 engine bursts: 400 writes, not 100,000
    Rig r;
    r.out->open(0);
    for (int b = 0; b < 400; ++b) {
        platform::SendBatch batch(r.out.get());
        for (int i = 0; i < 250; ++i) {
            platform::OutputStatus f;
            r.out->try_send_short(pack(0x90, i & 127, 64), f);
        }
    }
    CHECK(r.fake->writes.size() == 400 * 1 + 0 || r.fake->writes.size() <= 400 * 1);   // 750 bytes per burst fits one write
    CHECK(r.out->write_count() == r.fake->writes.size());
}

// ---------------------------------------------------------------------------------------- RoutedOutput (player output)

// Stand-in for the Java catalog: two routes (hardware with 2 devices, "synth" with 1). open_device makes what the Java
// route does through RouteNative: a BatchingOutput over a FakeRouteTarget, attached as current_route_output().
struct FakeCatalog : RouteCatalog {
    int list_calls = 0, open_calls = 0, close_calls = 0;
    std::string open_error;
    std::string last_name;
    size_t last_route = 99, last_index = 99;
    FakeRouteTarget* target = nullptr;          // target of the most recent open
    std::vector<std::string> hardware{"Piano", "Module - port 2"};
    std::vector<std::string> synth{"Built-in synthesizer"};

    size_t route_count() override { return 2; }
    std::vector<std::string> list_devices(size_t route) override
    {
        ++list_calls;
        return route == 0 ? hardware : synth;
    }
    std::string open_device(size_t route, size_t index, const std::string& name) override
    {
        ++open_calls;
        last_route = route; last_index = index; last_name = name;
        if (!open_error.empty()) return open_error;
        std::unique_ptr<FakeRouteTarget> t(new FakeRouteTarget());
        target = t.get();
        std::shared_ptr<BatchingOutput> out(new BatchingOutput(std::move(t)));
        out->open(0);
        set_current_route_output(out);
        return std::string();
    }
    void close_route(size_t) override
    {
        ++close_calls;
        std::shared_ptr<BatchingOutput> out = current_route_output();
        set_current_route_output(nullptr);
        if (out) out->close();
    }
};

struct SilentCatalog : FakeCatalog {
    std::string open_device(size_t, size_t, const std::string&) override
    {
        set_current_route_output(nullptr);
        return std::string();
    }
};

struct EmptyCatalog : FakeCatalog {
    size_t route_count() override { return 0; }
};

static void test_routed_output()
{
    set_current_route_output(nullptr);
    std::shared_ptr<FakeCatalog> cat(new FakeCatalog());
    RoutedOutput out(cat);

    // list: hardware devices first, then the synth route; ids encode route and device index; names as the routes give them
    std::vector<platform::OutputDeviceInfo> list = out.enumerate();
    CHECK(list.size() == 3);
    CHECK(list[0].name == "Piano" && list[0].device_id == RoutedOutput::make_id(0, 0));
    CHECK(list[1].name == "Module - port 2" && list[1].device_id == RoutedOutput::make_id(0, 1));
    CHECK(list[2].name == "Built-in synthesizer" && list[2].device_id == RoutedOutput::make_id(1, 0));
    CHECK(RoutedOutput::route_of(list[2].device_id) == 1 && RoutedOutput::index_of(list[1].device_id) == 1);
    CHECK(cat->list_calls == 2);

    // sends before open fail with a plain message
    OutputStatus st = out.send_short(pack(0x90, 60, 64));
    CHECK(!st.ok() && st.kind == platform::OutputErrorKind::InvalidOperation && !out.is_open());

    // open reaches the catalog with route, index and name, and attaches the shared output
    CHECK(out.open(list[1].device_id).ok());
    CHECK(cat->open_calls == 1 && cat->last_route == 0 && cat->last_index == 1 && cat->last_name == "Module - port 2");
    CHECK(out.is_open() && cat->target != nullptr);
    CHECK(current_route_output() && current_route_output()->is_open());

    // sends reach the target; the catalog (the Java side) is not called again
    const int listBefore = cat->list_calls, openBefore = cat->open_calls, closeBefore = cat->close_calls;
    CHECK(out.send_short(pack(0x90, 60, 64)).ok());
    CHECK(cat->target->writes.size() == 1 && cat->target->writes[0] == std::vector<uint8_t>({0x90, 60, 64}));
    out.begin_send_batch();
    platform::OutputStatus f;
    for (int i = 0; i < 100; ++i) CHECK(out.try_send_short(pack(0x90, 61, 64), f));
    CHECK(cat->target->writes.size() == 1);   // buffered
    out.end_send_batch();
    CHECK(cat->target->writes.size() == 2 && cat->target->writes[1].size() == 300);   // one write for the burst
    CHECK(out.send_long(sysex(0xF0, std::vector<uint8_t>({0xF0, 0x7E, 0xF7}))).ok());
    CHECK(cat->target->writes.back() == std::vector<uint8_t>({0xF0, 0x7E, 0xF7}));
    CHECK(out.panic().ok() && cat->target->all_bytes().size() > 144);
    CHECK(out.reset().ok() && cat->target->discard_calls == 1);
    CHECK(cat->list_calls == listBefore && cat->open_calls == openBefore && cat->close_calls == closeBefore);

    // the device is lost: sends report the message, nothing reaches the target
    const size_t writesBefore = cat->target->writes.size();
    current_route_output()->set_lost("The MIDI device \"Module\" was unplugged. Choose the output again.");
    st = out.send_short(pack(0x90, 62, 64));
    CHECK(!st.ok() && st.kind == platform::OutputErrorKind::IoError && st.message.find("unplugged") != std::string::npos);
    CHECK(cat->target->writes.size() == writesBefore);
    CHECK(!out.try_send_short(pack(0x90, 62, 64), f) && f.message == st.message);

    // close goes to the catalog; later sends fail; the output can be opened again (another route)
    CHECK(out.close().ok() && !out.is_open() && cat->close_calls == 1);
    CHECK(!out.send_short(pack(0x90, 62, 64)).ok());
    CHECK(out.panic().ok() && out.reset().ok());
    CHECK(out.open(list[2].device_id).ok() && cat->last_route == 1 && cat->last_name == "Built-in synthesizer");
    CHECK(out.send_short(pack(0x90, 70, 64)).ok() && cat->target->writes.size() == 1);

    // opening another device while open closes the first through the catalog
    const int closes = cat->close_calls;
    CHECK(out.open(list[0].device_id).ok() && cat->close_calls == closes + 1 && cat->last_name == "Piano");

    // open failures pass the route's text on and leave the output closed
    cat->open_error = "The MIDI device is in use by another app.";
    OutputStatus bad = out.open(list[0].device_id);
    CHECK(!bad.ok() && bad.message == "The MIDI device is in use by another app." && !out.is_open());
    cat->open_error.clear();

    // the catalog says ok but attached nothing: a plain failure, not a crash
    std::shared_ptr<FakeCatalog> silent(new SilentCatalog());
    RoutedOutput none(silent);
    none.enumerate();
    OutputStatus s2 = none.open(RoutedOutput::make_id(0, 0));
    CHECK(!s2.ok() && !none.is_open());

    // no routes at all is "None" only
    std::shared_ptr<FakeCatalog> empty(new EmptyCatalog());
    RoutedOutput nothing(empty);
    CHECK(nothing.enumerate().empty());

    set_current_route_output(nullptr);
}

// P13-USB: the write trace is off by default (no record), and records bytes, time and result of each write when on.
static void test_write_trace()
{
    write_trace_enable(false);
    {
        Rig rig;
        rig.out->open(0);
        rig.out->send_short(pack(0x90, 60, 100));
        CHECK(write_trace_snapshot().empty());   // off: nothing recorded
    }
    write_trace_enable(true);
    {
        Rig rig;
        rig.out->open(0);
        rig.out->begin_send_batch();
        rig.out->send_short(pack(0x90, 60, 100));
        rig.out->send_short(pack(0x80, 60, 0));
        rig.out->end_send_batch();
        rig.out->send_short(pack(0xB0, 64, 127));
        std::vector<WriteTraceRecord> r = write_trace_snapshot();
        CHECK(r.size() == 2);
        if (r.size() == 2) {
            CHECK(r[0].bytes == 6 && r[0].result == 6 && r[0].duration_ns >= 0 && r[0].route == 0);
            CHECK(r[1].bytes == 3 && r[1].start_ns >= r[0].start_ns);
        }
        rig.fake->fail_after_writes = 2;
        rig.out->send_short(pack(0xB0, 64, 0));
        r = write_trace_snapshot();
        CHECK(r.size() == 3 && r[2].result == -1);   // a failed write is recorded as -1
    }
    write_trace_enable(true);   // turning it on clears the ring
    CHECK(write_trace_snapshot().empty());
    write_trace_enable(false);
}

int main()
{
    test_routed_output();
    test_write_trace();
    test_encoding();
    test_open_close();
    test_unbatched_one_write_each();
    test_batching();
    test_big_burst_splits_on_message_boundaries();
    test_sysex();
    test_reset_and_panic();
    test_failures();
    test_dense_file_write_count();
    if (g_failures == 0) {
        std::printf("RESULT: PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("RESULT: FAIL (%d of %d checks)\n", g_failures, g_checks);
    return 1;
}
