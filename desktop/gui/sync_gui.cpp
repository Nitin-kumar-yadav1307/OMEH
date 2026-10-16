/*
 * sync_gui.cpp - standalone Dear ImGui monitor/control panel for pipewire_sync.
 *
 * Owns the ENGINE as a child process (spawned with --ui) and talks to it over
 * two pipes using desktop/ui_protocol.h: machine lines ('#S'/'#D'/'#A') come
 * back on stdout, one command per line goes in on stdin. The child's stderr is
 * merged into its stdout, so engine human logs ([main], [align], ...) arrive
 * here too - they never start with '#' (protocol guarantee), so the reader
 * below sorts data from log with a single prefix test.
 *
 * Threading: one blocking reader thread drains the pipe into a mutexed queue;
 * everything else (parse, send, draw) runs on the main/SDL thread, so no
 * audio or PipeWire code ever executes here - this TU links only SDL2, GL
 * and the vendored ImGui sources.
 *
 * Build: cmake option in desktop/CMakeLists.txt (needs SDL2 + GL dev files).
 *   Linux:   ./sync_gui [engine-path]   (default: pipewire_sync next to sync_gui)
 *   Windows: sync_gui.exe [engine-path] (default: pipewire_sync_win.exe next
 *            to sync_gui.exe; engine is spawned as "<path> --ui")
 * Keys: Q quits (sends 'quit' first so the engine shuts down gracefully).
 */

#include "../ui_protocol.h"

// We own main() ourselves (SUBSYSTEM:WINDOWS via pragma in main below);
// SDL_MAIN_HANDLED stops SDL from redefining main to SDL_main (which would
// need SDL2main + WinMain). Must precede ALL SDL includes, including the
// transitive one inside imgui_impl_sdl2.h - hence before the imgui headers.
#ifdef _WIN32
#define SDL_MAIN_HANDLED
#endif

#ifdef _WIN32
// windows.h BEFORE GL/gl.h: the SDK's gl.h needs the WINGDIAPI macros from
// windows.h, and NOMINMAX keeps windows.h from breaking std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
// Plain <SDL.h> on both toolchains: the imgui backend includes it the same
// way, and the include dir comes from the SDL2 targets (vcpkg CONFIG on
// Windows, pkg-config on Linux) - never hardcode the SDL2/ prefix here.
#include <SDL.h>
#include <GL/gl.h>

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>   /* mutable CreateProcess command buffer (Windows) */

/* Fatal startup error, shown where the user can actually see it.
 * sync_gui.exe is a WINDOWS-subsystem process: when double-clicked there is
 * no console, so the old fprintf-only paths fired into nothing - a missing
 * engine exe or an SDL failure looked exactly like "the GUI never opens".
 * MessageBoxA makes every fatal path visible; stderr stays for terminals
 * and CI logs. */
static void fatal_box(const std::string &msg)
{
    fprintf(stderr, "sync_gui: %s\n", msg.c_str());
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "SyncWifi", MB_OK | MB_ICONERROR);
#endif
}

/* ------------------------------------------------------------------ */
/* Engine child process                                              */
/* ------------------------------------------------------------------ */

struct EngineProc
{
#ifdef _WIN32
    HANDLE proc        = nullptr;  /* child process handle (wait/terminate) */
    HANDLE to_child    = nullptr;  /* GUI writes commands here (child stdin)  */
    HANDLE from_child  = nullptr;  /* GUI reads status+log here (child stdout) */
#else
    pid_t pid          = -1;
    int   to_child     = -1;   /* GUI writes commands here (child stdin)  */
    int   from_child   = -1;   /* GUI reads status+log here (child stdout) */
#endif
    bool  alive        = false;
};

struct Shared
{
    std::mutex lines_mtx;
    std::vector<std::string> lines;   /* raw lines, capped below */
    bool engine_eof = false;          /* reader saw EOF / engine died */
};

/* Directory containing this executable - the engine ships next to the GUI. */
static std::string self_dir()
{
#ifdef _WIN32
    // GetModuleFileNameA has no procfs equivalent; backslashes are the
    // native separator, so strip at the last '\' or '/' (MinGW may mix).
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, p, sizeof(p));
    if (n == 0 || n >= sizeof(p)) return ".";
    std::string s(p, n);
    size_t sl = s.find_last_of("/\\");
    return (sl == std::string::npos) ? "." : s.substr(0, sl);
#else
    char p[4096];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n <= 0) return ".";
    p[n] = '\0';
    std::string s(p);
    size_t sl = s.find_last_of('/');
    return (sl == std::string::npos) ? "." : s.substr(0, sl);
#endif
}

#ifdef _WIN32
static void close_handle(HANDLE &h)
{
    if (h && h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = nullptr; }
}
#else
static void close_fd(int &fd)
{
    if (fd >= 0) { close(fd); fd = -1; }
}
#endif

/* Spawn `path --ui`. Returns true if the fork/exec succeeded. */
#ifdef _WIN32
static bool spawn_engine(const std::string &path, EngineProc &e, Shared &sh)
{
    // CreateProcess needs inheritable pipe ends for the child and private
    // ends for the GUI; bInheritHandle=false on ours keeps them local.
    // stderr is merged into stdout so engine logs reach the log panel
    // (the '#' prefix still separates machine lines from human logs).
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE to_read = nullptr, to_write = nullptr;
    HANDLE from_read = nullptr, from_write = nullptr;
    if (!CreatePipe(&to_read, &to_write, &sa, 0) ||
        !CreatePipe(&from_read, &from_write, &sa, 0))
    {
        close_handle(to_read); close_handle(to_write);
        close_handle(from_read); close_handle(from_write);
        return false;
    }
    SetHandleInformation(to_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = to_read;
    si.hStdOutput = from_write;
    si.hStdError = from_write;
    std::string cmd = "\"" + path + "\" --ui";
    // CreateProcess may write into the command buffer, so it must be mutable.
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(to_read);
    CloseHandle(from_write);
    if (!ok)
    {
        close_handle(to_write);
        close_handle(from_read);
        return false;
    }
    CloseHandle(pi.hThread);
    e.proc = pi.hProcess;
    e.to_child = to_write;
    e.from_child = from_read;
    e.alive = true;
    sh.engine_eof = false;
    return true;
}
#else
static bool spawn_engine(const std::string &path, EngineProc &e, Shared &sh)
{
    int to_in[2] = { -1, -1 }, from_out[2] = { -1, -1 };
    if (pipe(to_in) < 0 || pipe(from_out) < 0)
    {
        close_fd(to_in[0]); close_fd(to_in[1]);
        close_fd(from_out[0]); close_fd(from_out[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0)
    {
        /* Child: wire pipes onto stdio, merge stderr into stdout (the GUI
           shows engine logs in its log panel; protocol keeps them distinct
           from machine lines by the '#' prefix). */
        dup2(to_in[0], STDIN_FILENO);
        dup2(from_out[1], STDOUT_FILENO);
        dup2(from_out[1], STDERR_FILENO);
        close(to_in[0]); close(to_in[1]);
        close(from_out[0]); close(from_out[1]);
        execl(path.c_str(), path.c_str(), "--ui", (char *)nullptr);
        _exit(127);   /* execl failed: no engine there */
    }
    close(to_in[0]);
    close(from_out[1]);
    e.pid = pid; e.to_child = to_in[1]; e.from_child = from_out[0];
    e.alive = true;
    sh.engine_eof = false;
    return true;
}
#endif

/* Best-effort graceful stop: ask nicely, then escalate. */
#ifdef _WIN32
static void stop_engine(EngineProc &e)
{
    if (!e.proc) return;
    // 'quit' was already sent by the caller where possible; the WASAPI
    // engine exits its main loop on it, so wait ~2 s before terminating.
    // TerminateProcess is the only escalation (no SIGTERM on Windows).
    if (WaitForSingleObject(e.proc, 2000) != WAIT_OBJECT_0)
        TerminateProcess(e.proc, 1);
    WaitForSingleObject(e.proc, INFINITE);
    CloseHandle(e.proc);
    e.proc = nullptr;
    close_handle(e.to_child);
    close_handle(e.from_child);
    e.alive = false;
}
#else
static void stop_engine(EngineProc &e)
{
    if (e.pid < 0) return;
    /* 'quit' was already sent by the caller where possible; give it a moment. */
    int status = 0;
    for (int i = 0; i < 20; i++)
    {
        pid_t r = waitpid(e.pid, &status, WNOHANG);
        if (r == e.pid) break;
        usleep(100 * 1000);
    }
    if (kill(e.pid, 0) == 0)   /* still there: escalate */
    {
        kill(e.pid, SIGTERM);
        usleep(300 * 1000);
    }
    if (kill(e.pid, 0) == 0) kill(e.pid, SIGKILL);
    waitpid(e.pid, &status, 0);
    e.pid = -1;
    close_fd(e.to_child);
    close_fd(e.from_child);
    e.alive = false;
}
#endif

#ifdef _WIN32
static void send_cmd(EngineProc &e, const std::string &cmd)
{
    // WriteFile on an anonymous pipe blocks only when the buffer is full;
    // one short line per UI click never fills it, so no overlapped I/O.
    if (!e.to_child || e.to_child == INVALID_HANDLE_VALUE) return;
    std::string line = cmd + "\n";
    const char *p = line.c_str();
    DWORD left = (DWORD)line.size();
    while (left > 0)
    {
        DWORD done = 0;
        if (!WriteFile(e.to_child, p, left, &done, nullptr) || done == 0)
        {
            e.alive = false;   // broken pipe: engine went away
            break;
        }
        p += done; left -= done;
    }
}
#else
static void send_cmd(EngineProc &e, const std::string &cmd)
{
    if (e.to_child < 0) return;
    std::string line = cmd + "\n";
    const char *p = line.c_str();
    size_t left = line.size();
    while (left > 0)
    {
        ssize_t n = write(e.to_child, p, left);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            e.alive = false;   /* EPIPE: engine went away */
            break;
        }
        p += (size_t)n; left -= (size_t)n;
    }
}
#endif

/* Blocking pipe drain: runs on its own thread, pushes raw lines. */
static void push_reader_chunk(Shared *sh, std::string &buf,
                              const char *tmp, size_t n)
{
    buf.append(tmp, n);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos)
    {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::lock_guard<std::mutex> lk(sh->lines_mtx);
        sh->lines.push_back(line);
        if (sh->lines.size() > 2000)
            sh->lines.erase(sh->lines.begin(),
                            sh->lines.begin() + (sh->lines.size() - 2000));
    }
}

static void reader_main(EngineProc *e, Shared *sh)
{
    std::string buf;
    char tmp[1024];
#ifdef _WIN32
    // ReadFile blocks until the engine writes or exits; a broken pipe
    // (ERROR_BROKEN_PIPE) means clean EOF, anything else is engine gone.
    for (;;)
    {
        DWORD n = 0;
        if (!ReadFile(e->from_child, tmp, sizeof(tmp), &n, nullptr) || n == 0)
            break;
        push_reader_chunk(sh, buf, tmp, n);
    }
#else
    for (;;)
    {
        ssize_t n = read(e->from_child, tmp, sizeof(tmp));
        if (n <= 0) break;   /* EOF or error: engine gone */
        push_reader_chunk(sh, buf, tmp, (size_t)n);
    }
#endif
    std::lock_guard<std::mutex> lk(sh->lines_mtx);
    sh->engine_eof = true;
}

/* ------------------------------------------------------------------ */
/* GUI-side state (main thread only)                                 */
/* ------------------------------------------------------------------ */

struct GuiState
{
    UiSummary summary;
    std::vector<UiDeviceStatus> devices;
    std::vector<std::string> log;   /* engine human logs + acks */
    std::chrono::steady_clock::time_point last_status;
    bool saw_status = false;
    bool stale      = true;
    bool engine_gone = false;
    int selected = 0;               /* device the trim/mute rows act on */
};

/* Drain queued lines: machine lines update state, everything else (and #A)
   lands in the log panel. Called once per frame. */
static void pump_lines(Shared &sh, GuiState &st)
{
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(sh.lines_mtx);
        batch.swap(sh.lines);
        if (sh.engine_eof) st.engine_gone = true;
    }
    auto now = std::chrono::steady_clock::now();
    for (const std::string &line : batch)
    {
        if (ui_classify(line) != UI_LINE_STATUS)
        {
            st.log.push_back(line);   /* human log line */
        }
        else if (line.compare(0, 3, "#S ") == 0)
        {
            if (ui_parse_summary(line, &st.summary))
            {
                st.last_status = now;
                st.saw_status = true;
                st.stale = false;
                st.devices.clear();   /* fresh snapshot follows */
            }
        }
        else if (line.compare(0, 3, "#D ") == 0)
        {
            UiDeviceStatus d;
            if (ui_parse_device(line, &d))
            {
                if ((int)st.devices.size() <= d.idx)
                    st.devices.resize((size_t)d.idx + 1);
                st.devices[(size_t)d.idx] = d;
            }
        }
        else
        {
            st.log.push_back(line);   /* #A acks read as log entries */
        }
    }
    if (st.log.size() > 500)
        st.log.erase(st.log.begin(), st.log.begin() + (st.log.size() - 500));
    if (st.saw_status && !st.stale &&
        now - st.last_status > std::chrono::seconds(3))
        st.stale = true;   /* engine quiet >3 s: flag, keep last values */
    if (st.selected >= (int)st.devices.size())
        st.selected = (int)st.devices.size() - 1;
    if (st.selected < 0) st.selected = 0;
}

/* One row of the device table. Trim/mute send immediately; the engine's next
   1 Hz tick echoes the result back (ack line + refreshed #D). */
static void device_row(GuiState &st, EngineProc &e, int i)
{
    const UiDeviceStatus &d = st.devices[(size_t)i];
    ImGui::PushID(i);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool sel = (st.selected == i);
    /* Device names come from the engine (Bluetooth strings) - clamp the
       label width so a hostile name cannot blow out the table. */
    std::string label = std::to_string(i) + ": " +
                        d.name.substr(0, 40);
    if (ImGui::Selectable(label.c_str(), sel,
                           ImGuiSelectableFlags_SpanAllColumns))
    {
        st.selected = i;
        /* 'select' is advisory on the Linux engine (acks only), but sending
           it keeps the round-trip synchronous for engines that use it. */
        send_cmd(e, "select " + std::to_string(i));
    }
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", d.lat);
    ImGui::TableNextColumn();
    ImVec4 col = (d.off >= -5.0 && d.off <= 5.0)
                 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                 : ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
    ImGui::TextColored(col, "%+.1f", d.off);
    ImGui::TableNextColumn();
    ImGui::Text("%.5f", d.ratio);
    ImGui::TableNextColumn();
    ImGui::Text("%llu", d.buf);
    ImGui::TableNextColumn();
    ImGui::Text("%llu", d.underruns);
    ImGui::TableNextColumn();
    /* Editable trim: drag or type, applied on Enter/deactivate. */
    float trim = (float)d.trim;
    ImGui::SetNextItemWidth(90.0f);
    char trim_id[32]; snprintf(trim_id, sizeof(trim_id), "##trim%d", i);
    if (ImGui::DragFloat(trim_id, &trim, 0.5f, -150.0f, 150.0f, "%.1f ms"))
    {
        /* live drag: intentionally no send (would spam the pipe) */
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "trim %d %.2f", i, (double)trim);
        send_cmd(e, cmd);
    }
    ImGui::TableNextColumn();
    {
        /* mute: two-way toggle. checked == muted; the engine echoes muted=
           back each tick, so the box always reflects the sink's actual state. */
        bool muted = (d.muted > 0.0f);
        char mute_id[32]; snprintf(mute_id, sizeof(mute_id), "mute%d", i);
        if (ImGui::Checkbox(mute_id, &muted))
        {
            send_cmd(e, "mute " + std::to_string(i) + " " +
                     (muted ? "1" : "0"));
        }
    }
    ImGui::TableNextColumn();
    {
        /* disconnect/reconnect: two-way toggle. checked == stream is
           connected (engine echoes conn=). Uncheck to drop, check to
           re-attach; the checkbox state is kept in sync with the engine
           tick, so both directions stay visible. */
        bool connected = (d.conn > 0);
        char conn_id[32]; snprintf(conn_id, sizeof(conn_id), "conn%d", i);
        if (ImGui::Checkbox(conn_id, &connected))
        {
            send_cmd(e, "connect " + std::to_string(i) + " " +
                     (connected ? "1" : "0"));
        }
    }
    ImGui::TableNextColumn();
    {
        /* volume: step-based control. -1/+ buttons step one percent,
           the edit field clamps to 0-100 and sends the exact value. */
        float vol = (float)d.vol;
        char v_id[32]; snprintf(v_id, sizeof(v_id), "##vol%d", i);
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::InputFloat(v_id, &vol, 0.0f, 0.0f, "%.0f",
                              ImGuiInputTextFlags_CharsDecimal))
        {
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 100.0f) vol = 100.0f;
            send_cmd(e, "volume " + std::to_string(i) + " " +
                     std::to_string((int)vol));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("-"))
        {
            send_cmd(e, "volume " + std::to_string(i) + " " +
                     std::to_string((int)(d.vol - 1.0)));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+"))
        {
            send_cmd(e, "volume " + std::to_string(i) + " " +
                     std::to_string((int)(d.vol + 1.0)));
        }
    }
    ImGui::PopID();
}

static void draw_status_panel(const GuiState &st)
{
    if (st.engine_gone)
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                            "engine exited - showing last state");
    else if (st.stale)
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                            "waiting for engine status...");
    else
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "live");
    ImGui::Separator();
    ImGui::Text("Aligning: %s", st.summary.aligning ? "yes" : "no");
    ImGui::Text("Devices:  %u", st.summary.devices);
    ImGui::Text("Ref emit: %.2f ms", st.summary.ref_ms);
    ImGui::Text("Master:   %llu frames", st.summary.master);
}

static void draw_devices_panel(GuiState &st, EngineProc &e)
{
    if (st.devices.empty())
    {
        ImGui::TextDisabled("no devices reported yet");
    }
    else if (ImGui::BeginTable("devs", 10,
                                ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("device");
        ImGui::TableSetupColumn("lat ms");
        ImGui::TableSetupColumn("off ms");
        ImGui::TableSetupColumn("ratio");
        ImGui::TableSetupColumn("buf");
        ImGui::TableSetupColumn("underrun");
        ImGui::TableSetupColumn("trim ms");
        ImGui::TableSetupColumn("mute");
        ImGui::TableSetupColumn("reconnect");
        ImGui::TableSetupColumn("vol %");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)st.devices.size(); i++)
            device_row(st, e, i);
        ImGui::EndTable();
    }
}

static void draw_controls_panel(GuiState &st, EngineProc &e)
{
    if (ImGui::Button("Ping engine"))
        send_cmd(e, "ping");
    ImGui::SameLine();
    if (ImGui::Button("Reference click"))
        send_cmd(e, "click");
    ImGui::SameLine();
    if (ImGui::Button("Quit engine"))
        send_cmd(e, "quit");
    ImGui::Separator();
    ImGui::TextDisabled("selected device: %d", st.selected);
    float step = 1.0f;
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputFloat("trim step ms", &step, 0.5f, 5.0f, "%.1f");
    if (step < 0.1f) step = 0.1f;
    if (st.selected >= 0 && st.selected < (int)st.devices.size())
    {
        double cur = st.devices[(size_t)st.selected].trim;
        char cmd[64];
        if (ImGui::Button("trim -")) {
            snprintf(cmd, sizeof(cmd), "trim %d %.2f",
                     st.selected, cur - step);
            send_cmd(e, cmd);
        }
        ImGui::SameLine();
        if (ImGui::Button("trim +")) {
            snprintf(cmd, sizeof(cmd), "trim %d %.2f",
                     st.selected, cur + step);
            send_cmd(e, cmd);
        }
        ImGui::SameLine();
        if (ImGui::Button("trim 0")) {
            snprintf(cmd, sizeof(cmd), "trim %d 0", st.selected);
            send_cmd(e, cmd);
        }
    }
}

static void draw_log_panel(const GuiState &st)
{
    /* Copy-on-draw keeps the panel correct even while the reader appends. */
    for (const std::string &l : st.log)
        ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
        ImGui::SetScrollHereY(1.0f);   /* sticky tail while at bottom */
}

/* ------------------------------------------------------------------ */
/* main: window, ImGui, frame loop                                  */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
// GUI subsystem entry: plain main() under /SUBSYSTEM:WINDOWS would need
// WinMain, so keep the portable main() and set the subsystem via pragma.
// The engine child still inherits working pipes; only OUR console goes
// away (CREATE_NO_WINDOW already hides the child's).
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#endif

int main(int argc, char **argv)
{
    // Same-folder default per OS: the Windows engine ships as
    // pipewire_sync_win.exe next to sync_gui.exe (backslashes native).
#ifdef _WIN32
    std::string engine_path =
        (argc > 1) ? argv[1] : self_dir() + "\\pipewire_sync_win.exe";
#else
    std::string engine_path =
        (argc > 1) ? argv[1] : self_dir() + "/pipewire_sync";
#endif

    EngineProc engine;
    Shared shared;
    if (!spawn_engine(engine_path, engine, shared))
    {
        char msg[512];
#ifdef _WIN32
        /* ERROR_FILE_NOT_FOUND (2) is the common case: sync_gui.exe was
           copied somewhere alone. Say so in words a user can act on. */
        snprintf(msg, sizeof(msg),
                 "Cannot start the audio engine:\n"
                 "  %s\n(error %lu)\n\n"
                 "Keep pipewire_sync_win.exe in the same folder as this "
                 "program - or install the SyncWifi-Setup installer from "
                 "the Releases page.",
                 engine_path.c_str(), (unsigned long)GetLastError());
#else
        snprintf(msg, sizeof(msg), "cannot spawn engine '%s': %s",
                 engine_path.c_str(), strerror(errno));
#endif
        fatal_box(msg);
        return 1;
    }
    std::thread reader(reader_main, &engine, &shared);
    reader.detach();   /* dies with the process; EOF ends its loop anyway */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fatal_box(std::string("SDL_Init failed: ") + SDL_GetError());
        stop_engine(engine);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window *window = SDL_CreateWindow(
        "SyncWifi monitor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1100, 700, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        fatal_box(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        SDL_Quit();
        stop_engine(engine);
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl)
    {
        fatal_box(std::string("SDL_GL_CreateContext failed: ") +
                  SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        stop_engine(engine);
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);   /* vsync: 1 Hz engine ticks, no need to spin */

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = 1.25f;   /* readable on HiDPI without hunting menus */
    ImGui::StyleColorsDark();
    /* Roomier default style: the previous tight padding made the cramped
       panels in the bug report overlap into unreadable strips. */
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.FramePadding      = ImVec2(6.0f, 3.0f);
    /* checkmarks must be visible on the default table background; the
       dark theme's default white tick is invisible against mid-grey. */
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.22f, 0.82f, 0.42f, 1.0f);
    style.FrameRounding     = 6.0f;
    style.WindowPadding     = ImVec2(12, 10);
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ScrollbarSize     = 14.0f;
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    GuiState st;
    bool done = false;
    while (!done)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT)
                done = true;
            /* Q quits like the window-close button; ImGui keys are only
               valid after NewFrame, so read the raw scancode here. */
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q &&
                !ImGui::GetIO().WantCaptureKeyboard)
            {
                done = true;
            }
        }

        pump_lines(shared, st);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int vw, vh;
        SDL_GL_GetDrawableSize(window, &vw, &vh);
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)vw, (float)vh), ImGuiCond_Always);
        ImGui::Begin("SyncWifi", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
            draw_status_panel(st);
        if (ImGui::CollapsingHeader("Devices", ImGuiTreeNodeFlags_DefaultOpen))
            draw_devices_panel(st, engine);
        if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
            draw_controls_panel(st, engine);
        if (ImGui::CollapsingHeader("Engine log"))
            draw_log_panel(st);
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, 1100, 700);
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    /* Graceful shutdown first so the engine can restore the default sink. */
    send_cmd(engine, "quit");
    stop_engine(engine);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
