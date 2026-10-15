/*
 * win_main.h — main loop, cleanup, CLI parsing, and console control.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER all other
 * win_*.h modules. Owns main_loop() (the drain/render/health poll loop),
 * cleanup_all(), the Flags struct + parse_args()/print_usage(), the root-
 * console pause helper, and the Ctrl+C handler. main() itself stays in
 * pipewire_sync_win.cpp.
 */
#pragma once


static void main_loop()
{
    uint64_t t0 = GetTickCount64();
    app.last_health_ms = t0;
    while (!g_stop)
    {
        if (g_run_seconds > 0 &&
            GetTickCount64() - t0 >= (uint64_t)g_run_seconds * 1000ULL)
        {
            fprintf(stdout, "\n[main] --seconds %d reached - stopping\n",
                    g_run_seconds);
            fflush(stdout);
            break;
        }
        drain_capture();
        poll_keyboard();
        win_ui_poll();   /* no-op unless --ui; never blocks the 2 ms tick */
        if (g_click_from &&
            app.master_written > g_click_from +
                                (uint64_t)CLICK_BURSTS * CLICK_PERIOD + 4800)
            g_click_from = 0;
        for (DeviceOut *dev : app.devices)
            if (!dev->gone && !dev->failed)
                fill_render(dev);
        uint64_t now = GetTickCount64();
        if (now - app.last_health_ms >= HEALTH_MS)
        {
            app.last_health_ms = now;
            health_tick(now);
        }
        Sleep(POLL_MS);
    }
}
static void cleanup_all()
{
    if (app.switched_default && !app.prev_default_id.empty())
    {
        if (win_set_default(app.prev_default_id))
            fprintf(stdout, "[main] default output restored -> \"%s\"\n",
                    app.prev_default_name.c_str());
        else
            fprintf(stdout,
                    "[main] could not restore default output - set it "
                    "manually to \"%s\"\n", app.prev_default_name.c_str());
        fflush(stdout);
        app.switched_default = false;
    }
    trim_save();
    source_restore();
    tone_test_close();
    for (DeviceOut *dev : app.devices)
    {
        teardown_device(dev);
        if (dev->endpoint) dev->endpoint->Release();
        delete dev;
    }
    app.devices.clear();
    stop_capture();
    if (app.enumerator)
    {
        app.enumerator->Release();
        app.enumerator = nullptr;
    }
    timeEndPeriod(1);
    if (g_clean_evt) SetEvent(g_clean_evt);
    CoUninitialize();
}
struct Flags
{
    std::map<int, double> delays;
    bool list           = false;
    bool help           = false;
    bool audible_source = false;
    int  seconds        = 0;
    bool ui             = false;
};
static Flags parse_args(int argc, char *argv[])
{
    Flags f;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--audible-source") { f.audible_source = true; continue; }
        if (a == "--list" || a == "-l") { f.list = true; continue; }
        if (a == "--help" || a == "-h") { f.help = true; continue; }
        if (a == "--seconds" && i + 1 < argc) { f.seconds = atoi(argv[++i]); continue; }
        if (a == "--ui") { f.ui = true; continue; }
        const char *colon = strchr(argv[i], ':');
        if (!colon) continue;
        f.delays[atoi(argv[i])] = atof(colon + 1);
    }
    return f;
}
static void print_usage()
{
    fprintf(stdout,
        "\nUsage: pipewire_sync_win.exe [device:ms ...] [options]\n\n"
        "Sends the machine's default-output audio to EVERY connected\n"
        "Bluetooth earbud, measuring and correcting the latency and clock\n"
        "difference between them.\n\n"
        "Options:\n"
        "  --audible-source   do not mute the source output (Speakers keep\n"
        "                     playing what the buds get).  Default: the\n"
        "                     source is muted while streaming, after a\n"
        "                     self-test that proves capture survives it.\n"
        "  --list, -l         list the output endpoints and exit\n"
        "  --seconds N        stop cleanly after N seconds (quick test)\n"
"  --ui               machine mode for the desktop GUI: emit '#'\n"
"                     status lines on stdout, read commands on stdin\n"
        "  -h, --help         this text\n"
        "  <index>:<ms>       per-device extra delay, e.g. 1:25\n\n"
        "Keys while running:\n"
        "  Tab                select the next bud (marked * in the status)\n"
        "  ]  [               selected bud  +5 ms / -5 ms\n"
        "  .  ,               selected bud  +1 ms / -1 ms\n"
        "  t                  alignment click test (4 clicks, both buds)\n"
        "  m                  mute / unmute the selected bud\n"
        "  h  ?               print this key help again\n"
        "  q                  quit (Ctrl+C works too)\n\n"
        "Trims are remembered per device id in pipewire_sync_win.trims next\n"
        "to the exe, so a tuned pair starts correct on the next run.\n\n");
    fflush(stdout);
}
/* --- double-click friendliness ------------------------------------------------
 * When the console hosts ONLY this process the window was created by
 * Explorer (user double-clicked the .exe): fatal errors and the final quit
 * would otherwise flash and vanish before they can be read, so we wait for
 * a key first.  When launched from a .bat / cmd.exe, cmd.exe is attached to
 * the same console (2 processes) and we stay quiet - the launcher shows its
 * own message.  Scripted runs (--help/--list/--seconds) never pause.       */
static bool console_is_root()
{
    DWORD list[2];
    return GetConsoleProcessList(list, 2) == 1;
}
static void pause_if_root(bool root)
{
    if (!root) return;
    fprintf(stdout, "\nPress any key to close...\n");
    fflush(stdout);
    _getch();
}
