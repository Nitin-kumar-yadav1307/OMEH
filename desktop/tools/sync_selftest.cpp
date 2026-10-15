// sync_selftest.cpp - unit tests for the shared sync engine.
//
// The headers are pure math (no I/O, no clock, no allocation), so every
// policy decision in them can be checked here without a phone, a network or
// an audio device. This is the fast feedback loop the Android build cannot
// give you: run it in milliseconds instead of deploying to two handsets.
//
//   ./sync_selftest      exit 0 = all pass
//
// Covers: wire layout + packing, ring wrap/interpolation/overflow, drift PI
// clamp + hard-sync threshold, AIMD jitter policy, and a closed-loop
// convergence check that drives the controller the way native-lib.cpp does.
#include "sync_wifi.h"
#include "ui_protocol.h"   // desktop line protocol: engine <-> GUI

#include <cstdio>
#include <cstdint>
#include <cmath>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    g_checks++; \
    double _a = (a), _b = (b); \
    if (std::fabs(_a - _b) > (eps)) { \
        g_fails++; \
        std::printf("FAIL %s:%d  %s=%.9f vs %s=%.9f\n", __FILE__, __LINE__, #a, _a, #b, _b); \
    } \
} while (0)

// --- wire.h: the contract every target compiles --------------------------
static void test_wire() {
    CHECK(sizeof(SyncWifiPacket) == (size_t)SYNC_WIFI_PACKET_BYTES);
    CHECK(SYNC_WIFI_PACKET_BYTES == 1936);           // 16 header + 1920 pcm
    CHECK(SYNC_WIFI_MAGIC == 0x53594E43u);           // "SYNC"
    CHECK(SYNC_WIFI_RATE == 48000 && SYNC_WIFI_CHANNELS == 2);
    CHECK(SYNC_WIFI_FRAMES_PER_PACKET == 480 && SYNC_WIFI_PORT == 48200);

    // Packed little-endian layout: magic at 0, seq at 4, pts at 8, pcm at 16.
    SyncWifiPacket p{};
    p.magic = SYNC_WIFI_MAGIC;
    p.seq = 0x11223344u;
    p.master_pts = 0x0102030405060708ull;
    const unsigned char* b = (const unsigned char*)&p;
    CHECK(b[0] == 0x43 && b[1] == 0x4E && b[2] == 0x59 && b[3] == 0x53); // "SYNC" LE
    CHECK((const void*)&p.pcm == (const void*)(b + 16));
}

// --- ring.h: wrap, level, interpolation, overflow -------------------------
static void test_ring() {
    SyncWifiRing r;
    CHECK(r.level() == 0);

    int16_t ramp[SYNC_WIFI_FRAMES_PER_PACKET * SYNC_WIFI_CHANNELS];
    for (int i = 0; i < SYNC_WIFI_FRAMES_PER_PACKET; i++) {
        ramp[i*2+0] = (int16_t)i;
        ramp[i*2+1] = (int16_t)(-i);
    }
    r.write_frames(ramp, SYNC_WIFI_FRAMES_PER_PACKET);
    CHECK(r.level() == (size_t)SYNC_WIFI_FRAMES_PER_PACKET);

    int16_t out[SYNC_WIFI_CHANNELS];
    r.read_frame(0.0, out);    CHECK(out[0] == 0    && out[1] == 0);
    r.read_frame(10.0, out);   CHECK(out[0] == 10   && out[1] == -10);
    r.read_frame(10.75, out);  CHECK(out[0] == 11);          // interpolates up
    r.read_frame(10.25, out);  CHECK(out[0] == 10);          // interpolates down
    r.read_frame(-5.0, out);   CHECK(out[0] == 0);           // clamps to read
    r.read_frame(1e9, out);    CHECK(out[0] == 479);         // clamps to write-1

    r.advance(480);
    CHECK(r.level() == 0);

    // Overflow keeps the NEWEST frames and drops the oldest.
    static int16_t big[SYNC_WIFI_RING_SIZE * SYNC_WIFI_CHANNELS] = {0};
    for (unsigned i = 0; i < SYNC_WIFI_RING_SIZE; i++) big[i*2] = (int16_t)i;
    SyncWifiRing o;
    o.write_frames(big, SYNC_WIFI_RING_SIZE);
    o.write_frames(big, SYNC_WIFI_RING_SIZE);
    CHECK(o.level() == (size_t)SYNC_WIFI_RING_SIZE);
    o.read_frame((double)(o.write - 1), out);
    CHECK(out[0] == (int16_t)(SYNC_WIFI_RING_SIZE - 1));     // newest survived
}

// --- drift.h: PI sign, clamp, hard-sync threshold ------------------------
static void test_drift() {
    SyncWifiDrift d;
    const double tgt = SYNC_WIFI_ALIGN_BASE_MS * SYNC_WIFI_RATE / 1000.0; // 7200
    CHECK_NEAR(d.target_frames, tgt, 1e-9);

    // At target: no error, unity ratio (no warp).
    d.primed = false;
    CHECK_NEAR(d.update((size_t)tgt, 0.01), 1.0, 1e-12);

    // Over-full ring must speed playback UP (drain) -> ratio > 1.
    d.primed = false;
    CHECK(d.update((size_t)tgt + 4800, 0.01) > 1.0);

    // Starved ring must slow playback DOWN -> ratio < 1.
    d.primed = false;
    CHECK(d.update((size_t)tgt - 4800, 0.01) < 1.0);

    // Clamp: KP*err at 4800 frames is 2.5e-3, well past 500ppm -> saturate.
    d.primed = false;
    CHECK_NEAR(d.update((size_t)tgt + 4800, 0.01), 1.0 + SYNC_WIFI_DRIFT_MAX_PPM/1e6, 1e-12);
    d.primed = false;
    CHECK_NEAR(d.update((size_t)tgt - 4800, 0.01), 1.0 - SYNC_WIFI_DRIFT_MAX_PPM/1e6, 1e-12);

    // Hard sync fires only BEYOND the threshold, not at it.
    d.primed = false;
    CHECK(!d.need_jump((size_t)tgt + (size_t)SYNC_WIFI_HARD_SYNC_FRAMES));      // == : no jump
    CHECK( d.need_jump((size_t)tgt + (size_t)SYNC_WIFI_HARD_SYNC_FRAMES + 1));  // >  : jump

    // A fully drained ring does NOT hard-jump at the default 150ms target:
    // 7200 frames behind is under the 9600-frame threshold, so the PI loop
    // recovers it smoothly instead of audible-jumping. Only at a raised
    // target (250ms = 12000 frames) does an empty ring exceed the threshold.
    d.primed = false;
    CHECK(!d.need_jump(0));                                        // 150ms: smooth
    d.target_frames = SYNC_WIFI_TARGET_CEIL_MS * SYNC_WIFI_RATE / 1000.0;
    CHECK( d.need_jump(0));                                        // 250ms: jump
}

// --- jitter.h: the AIMD latency policy ----------------------------------
static void test_jitter() {
    SyncWifiJitter j;
    SyncWifiDrift d;
    j.reset();
    CHECK_NEAR(j.target_ms, SYNC_WIFI_ALIGN_BASE_MS, 1e-9);   // starts at 150ms

    // Underrun grows the buffer and pushes the give-back clock out.
    j.advance(0.01);
    j.on_underrun(d);
    CHECK_NEAR(j.target_ms, 150.0 + SYNC_WIFI_TARGET_GROW_MS, 1e-9);  // 165
    CHECK(j.stalls == 1);
    CHECK_NEAR(d.target_frames, 165.0 * SYNC_WIFI_RATE / 1000.0, 1e-9);

    // Ceiling: hammering it can never exceed CEIL.
    for (int i = 0; i < 100; i++) j.on_underrun(d);
    CHECK_NEAR(j.target_ms, SYNC_WIFI_TARGET_CEIL_MS, 1e-9);          // 250

    // No give-back before the clean window elapses.
    double before = j.target_ms;
    j.maybe_shrink(d);
    CHECK_NEAR(j.target_ms, before, 1e-9);

    // A clean window hands latency back, and only after ADAPT_SEC.
    j.advance(SYNC_WIFI_TARGET_ADAPT_SEC);
    j.maybe_shrink(d);
    CHECK_NEAR(j.target_ms, SYNC_WIFI_TARGET_CEIL_MS - SYNC_WIFI_TARGET_SHRINK_MS, 1e-9);

    // Floor: a perfectly clean link walks down to FLOOR and stops.
    for (int i = 0; i < 100; i++) { j.advance(SYNC_WIFI_TARGET_ADAPT_SEC); j.maybe_shrink(d); }
    CHECK_NEAR(j.target_ms, SYNC_WIFI_TARGET_FLOOR_MS, 1e-9);         // 40

    // A stall right after hitting the floor takes latency back immediately.
    j.on_underrun(d);
    CHECK_NEAR(j.target_ms, SYNC_WIFI_TARGET_FLOOR_MS + SYNC_WIFI_TARGET_GROW_MS, 1e-9);
}

// --- closed loop: the policy actually self-tunes on a clean link ----------
// Mirrors native-lib.cpp clientRender exactly: advance clock, underrun check,
// shrink check, PI update. Feeds packets at the nominal rate and asserts the
// buffer walks down to the floor - i.e. a clean link really does end up low.
static void test_closed_loop() {
    SyncWifiRing ring;
    SyncWifiDrift drift;
    SyncWifiJitter jitter;
    jitter.reset();
    drift.target_frames = jitter.target_ms * SYNC_WIFI_RATE / 1000.0;

    static int16_t pcm[SYNC_WIFI_FRAMES_PER_PACKET * SYNC_WIFI_CHANNELS];
    for (int i = 0; i < SYNC_WIFI_FRAMES_PER_PACKET; i++)
        pcm[i*2] = pcm[i*2+1] = (int16_t)(i & 0x3ff);

    const int   CHUNK = 480;              // 10ms
    int         fed = 0, underruns = 0;

    // Run 30 simulated seconds: feed one packet per 10ms tick, render one.
    for (int tick = 0; tick < 3000; tick++) {
        ring.write_frames(pcm, SYNC_WIFI_FRAMES_PER_PACKET);
        fed++;

        if (!drift.primed) {
            if (ring.level() < (size_t)drift.target_frames) continue;
            ring.flush_to((size_t)drift.target_frames);
            drift.phase = (double)ring.read;
            drift.smooth_err = 0.0;
            drift.primed = true;
        }
        if (drift.need_jump(ring.level())) {
            ring.flush_to((size_t)drift.target_frames);
            drift.phase = (double)ring.read;
            drift.smooth_err = 0.0;
        }
        const double dt = (double)CHUNK / SYNC_WIFI_RATE;
        jitter.advance(dt);
        if (ring.level() == 0) { jitter.on_underrun(drift); underruns++; continue; }
        jitter.maybe_shrink(drift);

        double ratio = drift.update(ring.level(), dt);
        CHECK(ratio >= 1.0 - SYNC_WIFI_DRIFT_MAX_PPM/1e6 - 1e-12);   // never unbounded
        CHECK(ratio <= 1.0 + SYNC_WIFI_DRIFT_MAX_PPM/1e6 + 1e-12);
        for (int i = 0; i < CHUNK; i++) drift.phase += ratio;
        uint64_t nr = (uint64_t)drift.phase;
        if (nr > ring.read) {
            uint64_t adv = nr - ring.read;
            size_t avail = ring.level();
            if (adv > avail) { ring.advance(avail); drift.phase = (double)ring.write; }
            else ring.advance((size_t)adv);
        }
    }

    CHECK(fed == 3000);
    CHECK(underruns == 0);                          // clean link: no stalls
    CHECK(drift.primed);
    // The whole point of AIMD: 30s of clean streaming must land near the floor,
    // not sit pinned at the 150ms we started from.
    CHECK(jitter.target_ms <= 60.0);
}

// --- ui_protocol.h: engine <-> GUI line protocol round-trip ---------------
// The GUI spawns the engine over pipes, so this protocol is the ONLY thing
// that ties them together. Round-tripping format -> parse proves both sides
// agree without needing an audio device or a window.
static void test_ui_protocol() {
    // summary round-trip
    UiSummary s; s.aligning = 1; s.devices = 2; s.ref_ms = 182.5; s.master = 987654;
    std::string sl = ui_format_summary(s);
    CHECK(ui_classify(sl) == UI_LINE_STATUS);
    UiSummary s2;
    CHECK(ui_parse_summary(sl, &s2));
    CHECK(s2.aligning == 1 && s2.devices == 2 && s2.master == 987654);
    CHECK_NEAR(s2.ref_ms, 182.5, 1e-6);

    // device round-trip, incl. a name with spaces and '=' (common in BT names)
    UiDeviceStatus d;
    d.idx = 3; d.lat = 201.75; d.off = -12.25; d.ratio = 1.000123;
    d.buf = 4096; d.underruns = 7; d.trim = -3.5; d.meas = true;
    d.name = "WH-1000XM4 = main (L)";
    std::string dl = ui_format_device(d);
    CHECK(ui_classify(dl) == UI_LINE_STATUS);
    UiDeviceStatus d2;
    CHECK(ui_parse_device(dl, &d2));
    CHECK(d2.idx == 3 && d2.buf == 4096 && d2.underruns == 7 && d2.meas);
    CHECK_NEAR(d2.lat, 201.75, 1e-6);
    CHECK_NEAR(d2.off, -12.25, 1e-6);
    CHECK_NEAR(d2.ratio, 1.000123, 1e-9);
    CHECK_NEAR(d2.trim, -3.5, 1e-6);
    CHECK(d2.name == "WH-1000XM4 = main (L)");

    // a human log line must NOT be classified as machine data
    CHECK(ui_classify("  dev 0 bud_L  latency= 180.1 ms") == UI_LINE_OTHER);
    CHECK(ui_classify("[align] done") == UI_LINE_OTHER);
    CHECK(ui_classify("") == UI_LINE_OTHER);

    // ack lines
    CHECK(ui_classify(ui_format_ack("pong")) == UI_LINE_STATUS);

    // command parsing: no-arg verbs
    CHECK(ui_parse_command("ping").op  == UI_CMD_PING);
    CHECK(ui_parse_command("click").op == UI_CMD_CLICK);
    CHECK(ui_parse_command("quit").op  == UI_CMD_QUIT);
    // leading/trailing whitespace + CRLF tolerated
    CHECK(ui_parse_command("  ping \r\n").op == UI_CMD_PING);

    // indexed verbs with a value
    UiCommand t = ui_parse_command("trim 2 -3.5");
    CHECK(t.op == UI_CMD_TRIM && t.idx == 2);
    CHECK_NEAR(t.value, -3.5, 1e-9);
    UiCommand m = ui_parse_command("mute 0 1");
    CHECK(m.op == UI_CMD_MUTE && m.idx == 0);
    CHECK_NEAR(m.value, 1.0, 1e-9);
    UiCommand sel = ui_parse_command("select 1");
    CHECK(sel.op == UI_CMD_SELECT && sel.idx == 1);

    // malformed / unknown -> NONE, never a partial parse
    CHECK(ui_parse_command("").op        == UI_CMD_NONE);
    CHECK(ui_parse_command("   ").op     == UI_CMD_NONE);
    CHECK(ui_parse_command("frobnicate 1 2").op == UI_CMD_NONE);
    CHECK(ui_parse_command("trim").op    == UI_CMD_NONE);   // missing idx
    CHECK(ui_parse_command("trim 1").op  == UI_CMD_NONE);   // missing value
    CHECK(ui_parse_command("mute x y").op == UI_CMD_NONE);  // bad number

    // a status line must never parse as a command and vice versa
    CHECK(ui_parse_command(sl).op == UI_CMD_NONE);
}

int main() {
    test_wire();
    test_ring();
    test_drift();
    test_jitter();
    test_closed_loop();
    test_ui_protocol();

    if (g_fails == 0) {
        std::printf("sync_selftest: %d checks, all passed\n", g_checks);
        return 0;
    }
    std::printf("sync_selftest: %d/%d checks FAILED\n", g_fails, g_checks);
    return 1;
}
