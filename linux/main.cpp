// The Linux host: SDL3's window and event loop, timers and posted messages for the game's Win32-style hooks; the game
// is described by app::kGame (src/common/game_info.cpp)
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "compat/windows.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

int g_argc = 0;
char** g_argv = nullptr;

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_running{true};

// PostMessageW's queue, drained on the window thread (quick_menu's Quit posts WM_CLOSE)
std::mutex g_posted_lock;
std::deque<std::tuple<uint32_t, WPARAM, LPARAM>> g_posted;

// SetTimer's timers, fired on the window thread as WM_TIMER (game_info's key auto-release)
struct Timer {
    uint64_t at;     // GetTickCount64 ms
    UINT interval;
};
std::mutex g_timer_lock;
std::map<UINT_PTR, Timer> g_timers;
UINT_PTR g_next_timer = 1;

void FireTimers() {
    for (;;) {
        UINT_PTR id = 0;
        Timer t{};
        {
            std::lock_guard lock(g_timer_lock);
            uint64_t now = GetTickCount64();
            auto it = g_timers.begin();
            for (; it != g_timers.end(); it++)
                if (it->second.at <= now) break;
            if (it == g_timers.end()) return;
            id = it->first;
            t = it->second;
            if (t.interval) {
                t.at += t.interval;
                it->second = t;
            } else
                g_timers.erase(it);
        }
        intptr_t result = 0;
        if (app::kGame.message) app::kGame.message(g_window, WM_TIMER, id, 0, &result);
    }
}

// ms until the next timer, for the event wait's timeout
int TimerWaitMs() {
    std::lock_guard lock(g_timer_lock);
    if (g_timers.empty()) return 100;
    uint64_t now = GetTickCount64();
    auto it = g_timers.begin();
    for (; it != g_timers.end(); it++)
        if (it->second.at <= now) return 0;
    if (it == g_timers.end()) return 100;
    return int(std::min<uint64_t>(100, std::max<uint64_t>(1, it->second.at - now)));
}

void DrainPosted() {
    for (;;) {
        uint32_t msg;
        WPARAM wp;
        LPARAM lp;
        {
            std::lock_guard lock(g_posted_lock);
            if (g_posted.empty()) return;
            std::tie(msg, wp, lp) = g_posted.front();
            g_posted.pop_front();
        }
        if (msg == WM_CLOSE) {
            g_running = false;
            continue;
        }
        intptr_t result = 0;
        if (app::kGame.message) app::kGame.message(g_window, msg, wp, lp, &result);
    }
}

int VkOfScancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_RETURN: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_F1: return VK_F1;
        case SDL_SCANCODE_F3: return VK_F3;
        case SDL_SCANCODE_F10: return VK_F10;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        default: break;
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + int(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + int(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    return 0;
}

// an SDL key event as the window message the game hooks were written for: bit 30 of lp set on autorepeat
void KeyMessage(const SDL_Event& e, bool down) {
    int vk = VkOfScancode(e.key.scancode);
    if (!vk || !app::kGame.message) return;
    bool sys = (e.key.mod & SDL_KMOD_ALT) != 0;
    intptr_t lp = down && e.key.repeat ? (1 << 30) : 0;
    intptr_t result = 0;
    app::kGame.message(g_window, down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), vk, lp,
                       &result);
}

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
            g_running = false;
            return;
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            intptr_t result = 0;
            if (app::kGame.message) app::kGame.message(g_window, WM_KILLFOCUS, 0, 0, &result);
            return;
        }
        case SDL_EVENT_KEY_DOWN:
            KeyMessage(e, true);
            return;
        case SDL_EVENT_KEY_UP:
            KeyMessage(e, false);
            return;
        default:
            break;
    }
}

void CrashHandler(int sig) {
    LOGE("host: caught signal %d", sig);
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) kernel::LogGuestStack(*t->ctx, 64);
    logx::Flush();
    std::_Exit(128 + sig);
}

}  // namespace

// compat/windows.h's functions, used by the game's Win32-style hooks
uint64_t GetTickCount64() { return SDL_GetTicks(); }

UINT_PTR SetTimer(HWND, UINT_PTR id, UINT ms, void*) {
    std::lock_guard lock(g_timer_lock);
    if (!id) id = g_next_timer++;
    g_timers[id] = {GetTickCount64() + ms, ms};
    return id;
}

BOOL KillTimer(HWND, UINT_PTR id) {
    std::lock_guard lock(g_timer_lock);
    return g_timers.erase(id) ? TRUE : FALSE;
}

BOOL PostMessageW(HWND, uint32_t msg, WPARAM wp, LPARAM lp) {
    std::lock_guard lock(g_posted_lock);
    g_posted.emplace_back(msg, wp, lp);
    return TRUE;
}

short GetAsyncKeyState(int) { return 0; }
HCURSOR SetCursor(HCURSOR c) { return c; }

int MessageBoxW(HWND, const wchar_t* text, const wchar_t* title, unsigned flags) {
    std::string t, ttl;
    for (const wchar_t* p = title; p && *p; p++) ttl += char(*p < 0x80 ? *p : '?');
    for (const wchar_t* p = text; p && *p; p++) t += *p == L'\n' ? '\n' : char(*p < 0x80 ? *p : '?');
    const bool yesno = (flags & MB_YESNO) != 0;
    const SDL_MessageBoxButtonData buttons[] = {
        {yesno && !(flags & MB_DEFBUTTON2) ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, IDYES, "Yes"},
        {yesno ? (flags & MB_DEFBUTTON2 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
                                       : SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT)
               : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
         IDNO, yesno ? "No" : "OK"},
    };
    SDL_MessageBoxFlags type = (flags & MB_ICONERROR) ? SDL_MESSAGEBOX_ERROR
                               : (flags & (MB_ICONWARNING | MB_ICONQUESTION)) ? SDL_MESSAGEBOX_WARNING
                                                                              : SDL_MESSAGEBOX_INFORMATION;
    const SDL_MessageBoxData data{type,        g_window,  ttl.c_str(), t.c_str(),
                                  SDL_arraysize(buttons), buttons,   nullptr};
    int id = IDNO;
    if (!SDL_ShowMessageBox(&data, &id)) {
        LOGE("message box: %s", SDL_GetError());
        return (flags & MB_YESNO) ? IDNO : 0;
    }
    return id;
}

namespace app {

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) return std::filesystem::path(std::string(buf, size_t(n))).parent_path();
        return std::filesystem::current_path();
    }();
    return dir;
}

std::filesystem::path GameDir() {
    static const std::filesystem::path dir = [] {
        if (const char* d = std::getenv("XBE_GAME_DIR"); d && *d) return std::filesystem::path(d);
        return ExeDir();
    }();
    return dir;
}

int Instance() {
    static const int n = [] {
        for (int i = 1; i + 1 < g_argc; i++)
            if (!std::strcmp(g_argv[i], "--instance")) return std::clamp(std::atoi(g_argv[i + 1]), 0, 8);
        return 0;
    }();
    return n;
}

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

// the window already holds the mode the settings ask; no mode switch to apply on Linux
void ApplyDisplay() {
    if (!g_window) return;
    SDL_SetWindowSize(g_window, int(Output().width), int(Output().height));
    const DisplayMode mode = Get().display_mode;
    if (mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
    } else if (mode == DisplayMode::Fullscreen) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        SDL_DisplayMode closest{};
        if (SDL_GetClosestFullscreenDisplayMode(d, int(Output().width), int(Output().height), 0.0f, false, &closest))
            SDL_SetWindowFullscreenMode(g_window, &closest);
        SDL_SetWindowFullscreen(g_window, true);
    } else {
        SDL_SetWindowFullscreenMode(g_window, nullptr);  // borderless: the desktop's mode
        SDL_SetWindowFullscreen(g_window, true);
    }
}

// no splash and no relaunch handover on Linux: the window is up from the start
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

namespace {
std::atomic<uint32_t> g_present_interval{1};
}

uint32_t PresentInterval() { return g_present_interval.load(std::memory_order_relaxed); }

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) {
        SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
        cap = m && m->refresh_rate > 1 ? uint32_t(m->refresh_rate + 0.5f) : 60;
    }
    {
        uint32_t rate = cap;
        if (cap == kFpsCapRefresh) rate = 60;
        uint32_t n = 1;
        if (cap > 1 && rate > cap) {
            const uint32_t k = (rate + cap / 2) / cap;
            if (k >= 2 && (rate > k * cap ? rate - k * cap : k * cap - rate) <= 1) n = k;
        }
        g_present_interval.store(n, std::memory_order_relaxed);
    }
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    const char* name = nullptr;
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    LOGR("%ls (%s), %s", app::kGame.title, IMAGE_XBE, date);
    LOGR("Path: %s", app::ExeDir().string().c_str());
    LOGR("CPU: %d threads", SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    if (SDL_GetPlatform()) LOGR("Platform: %s", SDL_GetPlatform());
    if (m) LOGR("Display: %dx%d, %.2f Hz", m->w, m->h, m->refresh_rate);
    app::LogSettings();
}

}  // namespace

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    std::signal(SIGSEGV, CrashHandler);
    std::signal(SIGABRT, CrashHandler);
    std::signal(SIGFPE, CrashHandler);
    std::signal(SIGILL, CrashHandler);

    // audio starts later and non-fatally (linux/audio_sdl.cpp): a machine with no output device still runs
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    bool relaunched = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--relaunched")) relaunched = true;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::GameDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!app::PrepareGameCode(game)) return 1;

    std::string title;
    for (const wchar_t* p = app::kGame.title; p && *p; p++) title += char(*p < 0x80 ? *p : '?');
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    g_window = SDL_CreateWindow(title.c_str(), int(app::Output().width), int(app::Output().height),
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        return 1;
    }
    app::ApplyDisplay();
    SDL_ShowWindow(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "linux_sdl3";
    ImGui_ImplSDL3_InitForVulkan(g_window);

    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::GameDir() / hdd).string())) {
        MessageBoxW(nullptr, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    while (g_running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, TimerWaitMs())) {
            {
                std::lock_guard lock(app::UiLock());
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
            Dispatch(e);
            while (SDL_PollEvent(&e)) {
                {
                    std::lock_guard lock(app::UiLock());
                    ImGui_ImplSDL3_ProcessEvent(&e);
                }
                Dispatch(e);
            }
        }
        DrainPosted();
        FireTimers();
    }
    logx::Flush();
    kernel::Shutdown(0);
}
