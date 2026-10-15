/*
 * ui_protocol.h — line protocol spoken between the SyncWifi desktop ENGINE
 * (pipewire_sync / pipewire_sync_win) and the standalone GUI (sync_gui).
 *
 * Deliberately dependency-free (<string>/<cstring>/<cstdio> only) and header
 * only (functions are `inline`), so it can be compiled into several separate
 * translation units — the engine, the GUI, and sync_selftest — without any
 * link step. That is what lets the format/parse logic be unit-tested without
 * an audio device or a window.
 *
 * Direction & framing
 * -------------------
 *   engine -> GUI : machine lines START WITH '#'. The engine's existing human
 *                   log lines ([main], [align], "  dev 0 ...") never start
 *                   with '#', so the GUI can split "data" from "log" by a
 *                   single prefix test — no need to rewire ~30 log sites to
 *                   stderr. (See --ui in the engine.)
 *   GUI -> engine : one command per line, no prefix, parsed by
 *                   ui_parse_command().
 *
 * Status lines
 * ------------
 *   #S aligning=<0|1> devices=<n> ref_ms=<f> master=<n>
 *   #D i=<n> lat=<f> off=<f> ratio=<f> buf=<n> underruns=<n> trim=<f> \
 *      meas=<0|1> muted=<0|1> conn=<0|1> vol=<0-100> name=<rest-of-line>
 *
 *   muted/conn/vol were added after the first release; old engines emit only
 *   up to meas=. Parsers MUST accept both: missing keys default to
 *   muted=0, conn=1, vol=100. Name stays LAST and verbatim.
 *   #A <free-text acknowledgement, e.g. "pong">
 *
 * The device NAME is always the LAST field and is taken verbatim as the rest
 * of the line, because Bluetooth names routinely contain spaces and '=' and
 * cannot contain a newline. Everything before it is space-separated key=value.
 *
 * Commands (GUI -> engine), one per line
 * ---------------------------------------
 *   ping
 *   trim  <idx> <ms>        set absolute per-device extra trim (ms)
 *   mute  <idx> <0|1>       mute (1) / unmute (0) the device sink. On Linux
 *                           this drives PulseAudio; the mute bit is echoed
 *                           back in #D so the toggle shows real state.
 *   volume <idx> <0-100>     per-device volume percent (0-100, rounded).
 *                           Linux: pactl set-sink-volume; Windows: endpoint
 *                           scalar. Echoed back in #D as vol=.
 *   connect <idx> <0|1>     drop (0) / re-attach (1) a device stream.
 *                           Dropping destroys our playback stream and stops
 *                           feeding that ring, but KEEPS the device entry so
 *                           re-attach resumes without a registry round-trip.
 *                           (Physically powering off a bud is different: the
 *                           device node itself vanishes, which the registry
 *                           reports; this command only controls OUR stream.)
 *   click                   short reference click/tone
 *   select <idx>            highlight device idx (engine may just ack)
 *   quit                    graceful shutdown
 */
#pragma once

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>

/* ------------------------------------------------------------------ */
/* Status structs + line classification                                */
/* ------------------------------------------------------------------ */

struct UiSummary
{
    int           aligning = 1;
    unsigned      devices  = 0;
    double        ref_ms   = 0.0;
    unsigned long long master = 0;
};

struct UiDeviceStatus
{
    int           idx      = 0;
    double        lat      = 0.0;   /* total emit latency (ms)         */
    double        off      = 0.0;   /* offset vs reference device (ms) */
    double        ratio    = 1.0;   /* drift resampler ratio           */
    unsigned long long buf = 0;     /* ring level (frames)             */
    unsigned long long underruns = 0;
    double        trim     = 0.0;   /* manual extra trim (ms)          */
    bool          meas     = false; /* still probing own latency       */
    bool          muted    = false; /* sink currently muted            */
    bool          conn     = true;  /* our stream attached (false = GUI-dropped) */
    double        vol      = 100.0; /* last known volume percent       */
    std::string   name;
};

enum UiLineKind { UI_LINE_OTHER = 0, UI_LINE_STATUS };

/* A line carries machine data iff it starts with '#'. */
inline UiLineKind ui_classify(const std::string &line)
{
    return (!line.empty() && line[0] == '#') ? UI_LINE_STATUS : UI_LINE_OTHER;
}

/* ------------------------------------------------------------------ */
/* Small key=value readers                                             */
/*                                                                     */
/* ui_kv_* find " key=" (key preceded by a space so a prefix of a       */
/* longer key can never match) and parse the value that follows. They    */
/* return false when the key is absent.                                 */
/* ------------------------------------------------------------------ */

inline bool ui_kv_int(const std::string &line, const char *key, long long *out)
{
    std::string pat = std::string(" ") + key + "=";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    const char *s = line.c_str() + p + pat.size();
    char *end = nullptr;
    long long v = std::strtoll(s, &end, 10);
    if (end == s) return false;
    *out = v;
    return true;
}

inline bool ui_kv_dbl(const std::string &line, const char *key, double *out)
{
    std::string pat = std::string(" ") + key + "=";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    const char *s = line.c_str() + p + pat.size();
    char *end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* Parsing (GUI side): status lines -> structs                         */
/* ------------------------------------------------------------------ */

/* Parses "#S ...". Returns false if the line is not a summary. */
inline bool ui_parse_summary(const std::string &line, UiSummary *out)
{
    if (line.rfind("#S ", 0) != 0) return false;

    long long a = 0, d = 0, m = 0;
    double ref = 0.0;
    bool ok = ui_kv_int(line, "aligning", &a);
    ok = ui_kv_int(line, "devices", &d) && ok;
    ok = ui_kv_dbl(line, "ref_ms", &ref) && ok;
    ok = ui_kv_int(line, "master", &m) && ok;
    if (!ok) return false;

    out->aligning = (int)a;
    out->devices  = (unsigned)d;
    out->ref_ms   = ref;
    out->master   = (unsigned long long)m;
    return true;
}

/* Parses "#D ...". The name is the rest of the line after " name=". */
inline bool ui_parse_device(const std::string &line, UiDeviceStatus *out)
{
    if (line.rfind("#D ", 0) != 0) return false;

    /* name is last: everything after the first " name=" is verbatim */
    size_t np = line.find(" name=");
    std::string name;
    if (np != std::string::npos)
        name = line.substr(np + 6);
    /* the key=value part is everything up to the name (or the whole line) */
    std::string kv = (np != std::string::npos) ? line.substr(0, np) : line;

    long long idx = 0, buf = 0, unr = 0, meas = 0;
    double lat = 0, off = 0, ratio = 1, trim = 0;
    bool ok = ui_kv_int(kv, "i", &idx);
    ok = ui_kv_dbl(kv, "lat", &lat) && ok;
    ok = ui_kv_dbl(kv, "off", &off) && ok;
    ok = ui_kv_dbl(kv, "ratio", &ratio) && ok;
    ok = ui_kv_int(kv, "buf", &buf) && ok;
    ok = ui_kv_int(kv, "underruns", &unr) && ok;
    ok = ui_kv_dbl(kv, "trim", &trim) && ok;
    ok = ui_kv_int(kv, "meas", &meas) && ok;
    if (!ok) return false;

    /* muted/conn/vol are optional: engines that predate the GUI-volume
       update stop at meas=. Defaults keep the toggle honest: assume
       playing, attached, full volume. Name match is exact ("vol=" must
       not match a device-name fragment) because kv excludes the name. */
    long long muted = 0, conn = 1;
    double vol = 100.0;
    ui_kv_int(kv, "muted", &muted);
    ui_kv_int(kv, "conn", &conn);
    ui_kv_dbl(kv, "vol", &vol);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    out->idx       = (int)idx;
    out->lat       = lat;
    out->off       = off;
    out->ratio     = ratio;
    out->buf       = (unsigned long long)buf;
    out->underruns = (unsigned long long)unr;
    out->trim      = trim;
    out->meas      = (meas != 0);
    out->muted     = (muted != 0);
    out->conn      = (conn != 0);
    out->vol       = vol;
    out->name      = name;
    return true;
}

/* ------------------------------------------------------------------ */
/* Formatting (engine side): structs -> status lines                   */
/* ------------------------------------------------------------------ */

inline std::string ui_format_summary(const UiSummary &s)
{
    char b[192];
    std::snprintf(b, sizeof(b),
                  "#S aligning=%d devices=%u ref_ms=%.3f master=%llu",
                  s.aligning, s.devices, s.ref_ms,
                  (unsigned long long)s.master);
    return std::string(b);
}

inline std::string ui_format_device(const UiDeviceStatus &d)
{
    char head[256];
    std::snprintf(head, sizeof(head),
                  "#D i=%d lat=%.3f off=%+.3f ratio=%.6f buf=%llu "
                  "underruns=%llu trim=%.3f meas=%d muted=%d conn=%d "
                  "vol=%.1f name=",
                  d.idx, d.lat, d.off, d.ratio,
                  (unsigned long long)d.buf,
                  (unsigned long long)d.underruns,
                  d.trim, d.meas ? 1 : 0,
                  d.muted ? 1 : 0, d.conn ? 1 : 0, d.vol);
    return std::string(head) + d.name;
}

inline std::string ui_format_ack(const char *text)
{
    return std::string("#A ") + (text ? text : "");
}

/* ------------------------------------------------------------------ */
/* Commands (engine side): line -> action                              */
/* ------------------------------------------------------------------ */

enum UiCmd { UI_CMD_NONE = 0, UI_CMD_PING, UI_CMD_TRIM, UI_CMD_MUTE,
             UI_CMD_VOLUME, UI_CMD_CONNECT, UI_CMD_CLICK, UI_CMD_SELECT,
             UI_CMD_QUIT };

struct UiCommand
{
    UiCmd  op    = UI_CMD_NONE;
    int    idx   = -1;
    double value = 0.0;
};

/* Parses one GUI command line. Unknown / empty -> UI_CMD_NONE. */
inline UiCommand ui_parse_command(const std::string &raw)
{
    UiCommand c;

    /* trim trailing whitespace (CRLF from some spawn paths) */
    std::string s = raw;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return c;
    s = s.substr(b);

    if (s == "ping")  { c.op = UI_CMD_PING;  return c; }
    if (s == "click") { c.op = UI_CMD_CLICK; return c; }
    if (s == "quit")  { c.op = UI_CMD_QUIT;  return c; }

    size_t sp = s.find(' ');
    std::string verb = (sp == std::string::npos) ? s : s.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? std::string()
                                                 : s.substr(sp + 1);

    /* pull the next space-separated token off `rest` */
    auto next_tok = [&rest]() -> std::string {
        size_t p = rest.find_first_not_of(" \t");
        if (p == std::string::npos) { rest.clear(); return std::string(); }
        rest = rest.substr(p);
        size_t e = rest.find_first_of(" \t");
        std::string t = (e == std::string::npos) ? rest : rest.substr(0, e);
        rest = (e == std::string::npos) ? std::string() : rest.substr(e + 1);
        return t;
    };

    if (verb == "trim" || verb == "mute" || verb == "volume" ||
        verb == "connect" || verb == "select")
    {
        std::string si = next_tok();
        if (si.empty()) return c;                      /* idx required */

        /* strict: the whole token must be a number (no trailing garbage) */
        char *end = nullptr;
        long idx = std::strtol(si.c_str(), &end, 10);
        if (end == si.c_str() || *end != '\0') return c;
        if (verb == "select")
        {
            c.op = UI_CMD_SELECT; c.idx = (int)idx; return c;
        }

        std::string sv = next_tok();
        if (sv.empty()) return c;                      /* value required */
        end = nullptr;
        double v = std::strtod(sv.c_str(), &end);
        if (end == sv.c_str() || *end != '\0') return c;

        c.idx = (int)idx; c.value = v;
        c.op = (verb == "trim")   ? UI_CMD_TRIM :
               (verb == "mute")   ? UI_CMD_MUTE :
               (verb == "volume") ? UI_CMD_VOLUME : UI_CMD_CONNECT;
        return c;
    }

    return c;   /* unknown verb */
}

