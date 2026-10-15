/*
 * sync_pulse.h — PulseAudio/PipeWire routing via pactl.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER the type and
 * stream modules. Owns the virtual "Sync Master" null-sink creation, default
 * sink save/restore, sink-input enumeration, and reroute_clients() which
 * moves every playing application onto the virtual sink so we capture it.
 */
#pragma once


static bool create_virtual_sink()
{
    FILE *p = popen(
        "pactl load-module module-null-sink "
        "sink_name=sync_master "
        "sink_properties=device.description='Sync\\ Master' "
        "2>/dev/null",
        "r");
    if (!p)
        return false;

    char line[64] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);

    if (!ok || atoi(line) <= 0)
        return false;

    app.null_module_index = atoi(line);
    fprintf(stdout, "[main] virtual sink loaded (module %d)\n",
            app.null_module_index);

    /* wait for the registry to report the new node */
    int rounds = 0;
    while (app.virtual_sink_id == 0 && rounds < 100)
    {
        pw_loop_iterate(pw_main_loop_get_loop(app.loop), 100);
        rounds++;
    }

    return app.virtual_sink_id != 0;
}

/* ------------------------------------------------------------------ */
/* Default sink switching (via pipewire-pulse client 'pactl')          */
/* ------------------------------------------------------------------ */
static bool remember_default_sink()
{
    FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!p) return false;
    char line[256] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    /* strip newline */
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (ok && line[0])
        app.prev_default_sink = line;
    return ok;
}

static void set_default_sink(const char *name)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-default-sink '%s' 2>/dev/null", name);
    int r = system(cmd);
    (void)r;
    fprintf(stdout, "[main] default sink -> %s\n", name);
}

/* ------------------------------------------------------------------ */
/* Small helpers around the pulse layer (pactl)                        */
/* ------------------------------------------------------------------ */
static std::string quoted_value(const char *line)
{
    const char *a = strchr(line, '"');
    if (a == nullptr) return std::string();
    const char *b = strchr(a + 1, '"');
    if (b == nullptr) return std::string();
    return std::string(a + 1, (size_t)(b - a - 1));
}

struct SinkInput
{
    int         id         = 0;
    int         sink_index = -1;
    std::string node_name;
    std::string media_name;
    std::string app_name;
};

static std::vector<SinkInput> list_sink_inputs()
{
    std::vector<SinkInput> v;
    FILE *p = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (p == nullptr) return v;

    char line[1024];
    SinkInput cur;
    bool have = false;

    while (fgets(line, sizeof(line), p))
    {
        if (strncmp(line, "Sink Input #", 12) == 0)
        {
            if (have) v.push_back(cur);
            cur    = SinkInput();
            cur.id = atoi(line + 12);
            have   = true;
        }
        else if (!have) continue;
        else if (line[0] == '\t' && strncmp(line + 1, "Sink: ", 6) == 0)
            cur.sink_index = atoi(line + 7);
        else if (strstr(line, "node.name = \""))
            cur.node_name = quoted_value(line);
        else if (strstr(line, "media.name = \""))
            cur.media_name = quoted_value(line);
        else if (strstr(line, "application.name = \""))
            cur.app_name = quoted_value(line);
    }
    if (have) v.push_back(cur);
    pclose(p);
    return v;
}

struct SourceOutput
{
    int         id           = 0;
    int         source_index = -1;
    std::string node_name;
};

static std::vector<SourceOutput> list_source_outputs()
{
    std::vector<SourceOutput> v;
    FILE *p = popen("pactl list source-outputs 2>/dev/null", "r");
    if (p == nullptr) return v;

    char line[1024];
    SourceOutput cur;
    bool have = false;

    while (fgets(line, sizeof(line), p))
    {
        if (strncmp(line, "Source Output #", 15) == 0)
        {
            if (have) v.push_back(cur);
            cur    = SourceOutput();
            cur.id = atoi(line + 15);
            have   = true;
        }
        else if (!have) continue;
        else if (line[0] == '\t' && strncmp(line + 1, "Source: ", 8) == 0)
            cur.source_index = atoi(line + 9);
        else if (strstr(line, "node.name = \""))
            cur.node_name = quoted_value(line);
    }
    if (have) v.push_back(cur);
    pclose(p);
    return v;
}

/* index -> name map for "sinks" / "sources" (from pactl list short) */
static std::map<int, std::string> list_objects(const char *what)
{
    std::map<int, std::string> m;

    std::string cmd = std::string("pactl list short ") + what + " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (p == nullptr) return m;

    char line[1024];
    while (fgets(line, sizeof(line), p))
    {
        int  idx   = 0;
        char nm[512] = {};
        if (sscanf(line, "%d\t%511s", &idx, nm) == 2)
            m[idx] = nm;
    }
    pclose(p);
    return m;
}

static std::string get_default_sink_name()
{
    FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
    if (p == nullptr) return std::string();

    char line[256] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    if (!ok) return std::string();

    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    return std::string(line);
}

/*
 * Make sure an earbud is not muted. NOTE: pactl uses the PulseAudio
 * index space, which is NOT the PipeWire node id — so we pass the
 * sink's node NAME instead.
 */
static void unmute_sink(const DeviceOut *dev)
{
    if (dev->target_name.empty())
        return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-mute '%s' 0 2>/dev/null",
             dev->target_name.c_str());
    int r = system(cmd); (void)r;
}

/*
 * WirePlumber/Plasma may revert the default sink, and app streams that
 * existed before us do not follow the default change. So every second:
 *
 *   - re-assert sync_master as default sink,
 *   - move every app's sink-input into sync_master,
 *   - move our capture back onto sync_master's monitor if something
 *     relocated it (that relocation is the classic mic/echo bug),
 *
 * Our own streams (node.name "sync-out-*" / "sync-capture") are NEVER
 * moved: putting a playback stream on sync_master would silence the
 * earbuds and loop our own audio back into the capture (= echo).
 */
static bool is_our_stream(const std::string &node_name,
                          const std::string &media_name)
{
    if (node_name.rfind("sync-out-", 0) == 0)  return true;
    if (node_name.rfind("sync-capture", 0) == 0) return true;
    if (node_name.rfind("input.sync_master", 0) == 0) return true;
    (void)media_name;
    return false;
}

static void reroute_clients()
{
    /* 1. keep the default sink */
    std::string cur_default = get_default_sink_name();
    if (!app.virtual_sink_name.empty() && cur_default != app.virtual_sink_name)
        set_default_sink(app.virtual_sink_name.c_str());

    if (app.virtual_sink_name.empty())
        return;

    /* 2. sink-inputs -> sync_master (skip our own output streams) */
    std::map<int, std::string> sinks = list_objects("sinks");
    for (const SinkInput &si : list_sink_inputs())
    {
        if (is_our_stream(si.node_name, si.media_name))
        {
            /*
             * Safety net: our playback stream must NEVER sit on the
             * virtual sink. If it does, put it back on its earbud.
             */
            for (DeviceOut *dev : app.devices)
            {
                if (dev->stream == nullptr) continue;
                if (!si.node_name.empty() &&
                    dev->node_name == si.node_name)
                {
                    auto it = sinks.find(si.sink_index);
                    if (it != sinks.end() && it->second == app.virtual_sink_name)
                    {
                        char cmd[512];
                        snprintf(cmd, sizeof(cmd),
                                 "pactl move-sink-input %d '%s' 2>/dev/null",
                                 si.id, dev->target_name.c_str());
                        int r = system(cmd); (void)r;
                    }
                    break;
                }
            }
            continue;
        }

        auto it = sinks.find(si.sink_index);
        if (it == sinks.end() || it->second != app.virtual_sink_name)
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "pactl move-sink-input %d '%s' 2>/dev/null",
                     si.id, app.virtual_sink_name.c_str());
            int r = system(cmd); (void)r;
        }
    }

    /* 3. our capture must stay on the virtual sink's monitor */
    std::map<int, std::string> sources = list_objects("sources");
    for (const SourceOutput &so : list_source_outputs())
    {
        if (so.node_name != "sync-capture")
            continue;

        /* find sync_master's monitor source index */
        static const char *want = "sync_master.monitor";
        auto sit = sources.find(so.source_index);
        if (sit == sources.end())
            continue;

        if (sit->second.find("sync_master") == std::string::npos)
        {
            for (const auto &kv : sources)
            {
                if (kv.second.find("sync_master") != std::string::npos)
                {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd),
                             "pactl move-source-output %d '%s' 2>/dev/null",
                             so.id, kv.second.c_str());
                    int r = system(cmd); (void)r;

                    if (!app.capture_route_warned)
                    {
                        app.capture_route_warned = 1;
                        fprintf(stdout,
                                "[timer] capture had been relocated away from "
                                "sync_master - moved it back\n");
                    }
                    break;
                }
            }
        }
        (void)want;
    }
}

/* default sink by node id — pulse names get renamed by WirePlumber */
static void set_default_node(uint32_t node_id)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %u 2>/dev/null", node_id);
    int r = system(cmd);
    (void)r;
    fprintf(stdout, "[main] default sink -> sync_master (node %u)\n",
            node_id);
}

