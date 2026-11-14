// The recompiled game code is built on the player's PC, so a release carries none of it: xbe_recomp translates the
// xbe to C++, clang compiles that and lld links it into <exe>.so beside the executable, all from the compiler folder.
// The first start does this with a console progress log. Finished objects are kept, so a closed window or a crash
// loses only the files in progress and the next start carries on. (The SDK's game_code.cpp with POSIX processes and
// an .so instead of a .dll.)
#include <fcntl.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include <SDL3/SDL.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "compat/windows.h"
#include "core/log.h"
#include "hook_exports.h"
#include "image_symbols.h"
#include "kernel/disc.h"

namespace dispatch {
bool Load(const std::filesystem::path& so);  // cpu/dispatch.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kJobMemory = 512ull << 20;  // a compiler's peak on the biggest file is about half of this
constexpr uint64_t kDiskNeeded = 1ull << 30;   // sources, objects and shared objects, with room to spare

enum class Stage { Translating, Compiling, Linking, Ready };

// written by the build thread, read by the progress log
struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::string g_failure;  // the step that failed, written by the build thread before it ends

// the tools (clang, lld, xbe_recomp, headers): inside the exe folder, which a packaged AppImage mounts
// read-only. Everything written (the work folder, the .so, logs) goes to the game folder.
fs::path CompilerDir() { return ExeDir() / "compiler"; }
fs::path WorkRoot() { return GameDir() / "compiler" / "work"; }

// the config in compiler/config whose [xbe] path is this xbe; an included one has no path
fs::path ConfigFor(const std::string& xbe) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(CompilerDir() / "config", ec)) {
        try {
            toml::table t = toml::parse_file(e.path().string());
            if (fs::path(t["xbe"]["path"].value_or(std::string())).filename() == xbe) return e.path();
        } catch (const toml::parse_error&) {
        }
    }
    return {};
}

struct Image {
    std::string exe;
    std::string xbe;
    fs::path config;
};

std::vector<Image> Images() {
    std::vector<Image> images;
    for (auto& [xbe, exe] : kGame.images) images.push_back({exe, xbe, ConfigFor(xbe)});
    return images;
}

fs::path WorkDir(const Image& image) { return WorkRoot() / image.exe; }
fs::path CodeSo(const Image& image) { return GameDir() / (image.exe + ".so"); }
// beside the .so, so deleting the compiler folder keeps it: without it the .so was judged by file times and could
// look stale, and the next start had no compiler to rebuild with
fs::path StampFile(const Image& image) { return GameDir() / (image.exe + ".built.txt"); }
fs::path CompileLog() { return GameDir() / "logs" / "compile.log"; }

// keeps the failed step for the message the player sees; false, to return
bool RecordFailure(const std::string& what) {
    g_failure = what;
    LOGE("Game code: %s", what.c_str());
    return false;
}

// an image to build, with what its finished .so is stamped with
struct Target {
    const Image* image = nullptr;
    std::vector<uint8_t> xbe;
    std::string stamp;
};

// a .so serves the executable it was linked against and the xbe it was translated from
bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    std::error_code ec;
    fs::path path = ExeDir() / image.exe;
    uint64_t size = fs::file_size(path, ec);
    if (ec) return false;
    uint64_t time = uint64_t(fs::last_write_time(path, ec).time_since_epoch().count());
    if (ec) return false;
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + Sha1(xbe);
    return true;
}

// a build's .so has no stamp: it is current when newer than the executable it was linked against
bool Current(const Target& target) {
    std::error_code ec;
    fs::file_time_type built = fs::last_write_time(CodeSo(*target.image), ec);
    if (ec) return false;
    std::ifstream f(StampFile(*target.image));
    if (!f) return built > fs::last_write_time(ExeDir() / target.image->exe, ec);
    std::string line;
    return std::getline(f, line) && line == target.stamp;
}

// the image's xbe from the game in assets: the extracted file, else the mounted disc image
bool ReadXbe(const GameFiles& game, const char* name, std::vector<uint8_t>& out) {
    if (std::string_view(name) == IMAGE_XBE) {
        out = game.xbe;
        return true;
    }
    std::ifstream f(game.dir / name, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    const kernel::Disc* disc = kernel::MountedDisc();
    return disc && disc->Load(name, out);
}

std::vector<Target> StaleTargets(const GameFiles& game, const std::vector<Image>& images) {
    std::vector<Target> stale;
    for (const Image& image : images) {
        Target target{&image};
        if (!ReadXbe(game, image.xbe.c_str(), target.xbe)) {
            LOGR("Game code: no %s in the game files, %s is not built", image.xbe.c_str(), image.exe.c_str());
            continue;
        }
        if (!StampFor(image, target.xbe, target.stamp) || Current(target)) continue;
        stale.push_back(std::move(target));
    }
    return stale;
}

// a source to compile and the object it makes
struct Job {
    fs::path source;
    fs::path object;
    uint64_t bytes = 0;
};

uint32_t PartitionCount(const Image& image) {
    try {
        toml::table t = toml::parse_file(image.config.string());
        return uint32_t(t["xbe"]["partitions"].value_or(int64_t(0)));
    } catch (const toml::parse_error&) {
        return 0;
    }
}

// the newest of what every object of an image is built from besides its own source
fs::file_time_type SharedInputsTime(const Image& image) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(WorkDir(image) / "x86_recomp_shared.h", ec);
    newest = std::max(newest, fs::last_write_time(CompilerDir() / "clang", ec));
    for (auto& e : fs::recursive_directory_iterator(CompilerDir() / "include", ec))
        newest = std::max(newest, e.last_write_time(ec));
    return newest;
}

// the image's sources whose object is missing or older than what it is built from
void AddJobs(const Image& image, std::vector<Job>& jobs) {
    fs::path work = WorkDir(image);
    std::vector<std::string> names{"x86_func_mapping"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) names.push_back("x86_recomp." + std::to_string(i));
    fs::file_time_type shared = SharedInputsTime(image);
    for (const std::string& name : names) {
        Job job{work / (name + ".cpp"), work / (name + ".o")};
        std::error_code ec;
        job.bytes = fs::file_size(job.source, ec);
        fs::file_time_type built = fs::last_write_time(job.object, ec);
        if (!ec && built > shared && built > fs::last_write_time(job.source, ec)) continue;
        jobs.push_back(std::move(job));
    }
}

// compilers that run at once: one a logical processor, as many as the free memory holds
size_t Slots() {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t available = 0;
    std::ifstream mem("/proc/meminfo");
    for (std::string key; mem >> key;) {
        if (key == "MemAvailable:") {
            mem >> available;
            available <<= 10;
            break;
        }
        mem.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (!available) available = 4ull << 30;
    return std::clamp<size_t>(size_t(std::min<uint64_t>(available / kJobMemory, uint64_t(cpus > 0 ? cpus : 1))), 1, 64);
}

int ExitCode(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : 1; }

// starts and reaps the build's processes; cancel kills each one's whole process group
struct Processes {
    Processes() { log_ = open(CompileLog().string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); }
    ~Processes() {
        if (log_ >= 0) close(log_);
    }

    // 0 when it did not start; its output goes to logs/compile.log. dir is the child's working directory (the
    // compile passes bare file names, as the Windows driver's cwd did)
    pid_t Start(char* const argv[], const fs::path& dir) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        posix_spawn_file_actions_addchdir_np(&fa, dir.string().c_str());
#endif
        if (log_ >= 0) {
            posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, CompileLog().string().c_str(),
                                             O_WRONLY | O_CREAT | O_APPEND, 0644);
            posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
        }
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);  // a group of its own, so Cancel reaches the children it spawns too
        pid_t pid = 0;
        int err = posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        if (err) {
            LOGE("game code: could not start %s (%s)", argv[0], strerror(err));
            return 0;
        }
        return pid;
    }

    // kills the running and reaps them
    static void CancelAll(std::vector<pid_t>& pids) {
        for (pid_t pid : pids) kill(-pid, SIGKILL);
        for (pid_t pid : pids) waitpid(pid, nullptr, 0);
        pids.clear();
    }

    int log_ = -1;
};

// xbe_recomp writes each image's C++ into its work folder, the images side by side
bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<pid_t> pids;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), target.xbe.size());
        std::string recomp = (CompilerDir() / "xbe_recomp").string();
        std::string config = target.image->config.string();
        std::string xbe_s = xbe.string();
        std::string work_s = work.string();
        std::string xdk_s = (CompilerDir() / "xdk").string();
        char* const argv[] = {recomp.data(), config.data(), xbe_s.data(), work_s.data(), (char*)"--xdk", xdk_s.data(), nullptr};
        pid_t pid = run.Start(argv, work);
        if (!pid) {
            Processes::CancelAll(pids);
            return RecordFailure("xbe_recomp could not be started.");
        }
        pids.push_back(pid);
    }
    std::vector<int> codes(pids.size(), 1);
    for (size_t i = 0; i < pids.size(); i++) {
        while (waitpid(pids[i], &codes[i], WNOHANG) == 0) {
            if (g_progress.cancel) {
                Processes::CancelAll(pids);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    bool ok = true;
    for (size_t i = 0; i < pids.size(); i++)
        if (int code = ExitCode(codes[i]); code) ok = RecordFailure("Translating " + targets[i].image->xbe + " failed.");
    return ok;
}

std::vector<std::string> CompileArgv(const Job& job) {
    // the flags sdk/cmake/XbeRecomp.cmake compiles the game code with, minus the MSVC target; one string per
    // argument, held alive by the caller
    fs::path include = CompilerDir() / "include";
    return {(CompilerDir() / "clang").string(), "-std=c++20", "-O2",           "-march=x86-64-v2",
            "-fno-strict-aliasing",             "-DNDEBUG",  "-DX86_GAME_CODE",
            "-nostdinc",                        "-nostdinc++", "-w",
            "-resource-dir",                    (CompilerDir() / "resource").string(),
            "-isystem",                         (include / "freestanding").string(),
            "-I",                               include.string(),
            "-c",                               job.source.filename().string(),
            "-o",                               job.object.filename().string() + ".tmp"};
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, int status) {
    std::string name = job.source.parent_path().filename().string() + "/" + job.source.filename().string();
    if (ExitCode(status)) return RecordFailure("Compiling " + name + " failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure("Saving the compiled " + name + " failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows; a line a second on the console
bool Compile(std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.stage = Stage::Compiling;

    std::vector<std::vector<std::string>> arg_store(jobs.size());
    std::vector<pid_t> running;
    std::vector<uint32_t> running_jobs;
    size_t next = 0;
    uint64_t logged_at = 0;
    auto log_progress = [&] {
        uint64_t now = uint64_t(SDL_GetTicks());
        if (now - logged_at < 1000) return;
        logged_at = now;
        uint64_t total = g_progress.bytes_total;
        uint64_t done = g_progress.bytes_done;
        uint32_t pct = total ? uint32_t(done * 100 / total) : 0;
        LOGR("Game code: %u%%, file %u of %u", pct, g_progress.files_done + 1, g_progress.files_total.load());
    };
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            arg_store[next] = CompileArgv(jobs[next]);
            std::vector<char*> argv;
            for (std::string& a : arg_store[next]) argv.push_back(a.data());
            argv.push_back(nullptr);
            pid_t pid = run.Start(argv.data(), jobs[next].source.parent_path());
            if (!pid) {
                for (pid_t p : running) kill(-p, SIGKILL);
                for (pid_t p : running) waitpid(p, nullptr, 0);
                return RecordFailure("clang could not be started.");
            }
            running.push_back(pid);
            running_jobs.push_back(uint32_t(next));
        }
        int status = 0;
        pid_t done = waitpid(-1, &status, WNOHANG);
        if (done == 0) {
            if (g_progress.cancel) {
                for (pid_t p : running) kill(-p, SIGKILL);
                for (pid_t p : running) waitpid(p, nullptr, 0);
                return false;
            }
            log_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (done < 0) {
            if (errno == EINTR) continue;
            for (pid_t p : running) kill(-p, SIGKILL);
            for (pid_t p : running) waitpid(p, nullptr, 0);
            return RecordFailure("Waiting on the compilers failed.");
        }
        auto it = std::find(running.begin(), running.end(), done);
        if (it == running.end()) continue;
        size_t i = size_t(it - running.begin());
        running.erase(it);
        uint32_t job_i = running_jobs[i];
        running_jobs.erase(running_jobs.begin() + i);
        if (ExitCode(status)) {
            for (pid_t p : running) kill(-p, SIGKILL);
            for (pid_t p : running) waitpid(p, nullptr, 0);
            return RecordFailure("Compiling " + jobs[job_i].source.filename().string() + " failed.");
        }
        if (!FinishJob(jobs[job_i], status)) {
            for (pid_t p : running) kill(-p, SIGKILL);
            for (pid_t p : running) waitpid(p, nullptr, 0);
            return false;
        }
    }
    return true;
}

// lld links the objects into the .so; the host's symbols it calls resolve from the executable at dlopen
bool Link(const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    std::vector<std::string> args{(CompilerDir() / "clang").string(), "-shared", "-nostdlib", "-fuse-ld=lld",
                                  "-B", CompilerDir().string(), "-o", CodeSo(image).string()};
    args.push_back((work / "x86_func_mapping.o").string());
    for (uint32_t i = 0; i < PartitionCount(image); i++)
        args.push_back((work / ("x86_recomp." + std::to_string(i) + ".o")).string());
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = run.Start(argv.data(), work);
    if (!pid) return RecordFailure("clang could not be started for the link.");
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (g_progress.cancel) {
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (ExitCode(status)) {
        std::string hint = " If the game is already running, close it, then start it again.";
        return RecordFailure("Linking " + CodeSo(image).filename().string() + " failed." + hint);
    }
    std::ofstream(StampFile(image)) << target.stamp << "\n";
    return true;
}

bool Build(std::vector<Target>& targets) {
    Processes run;
    if (!Translate(targets, run)) return false;
    std::vector<Job> jobs;
    for (const Target& target : targets) AddJobs(*target.image, jobs);
    if (!Compile(jobs, run)) return false;
    g_progress.stage = Stage::Linking;
    for (const Target& target : targets)
        if (!Link(target, run)) return false;
    return true;
}

// what stops a build before it starts, for the player to fix; empty when nothing does
std::string Blocker(const std::vector<Target>& targets) {
    for (const char* tool : {"clang", "xbe_recomp"}) {
        std::error_code ec;
        if (!fs::exists(CompilerDir() / tool, ec))
            return "The compiler folder is missing files. Extract the whole release again, then start the game.";
    }
    for (const Target& target : targets) {
        if (target.image->config.empty())
            return "The compiler folder is missing files. Extract the whole release again, then start the game.";
        std::error_code ec;
        fs::create_directories(WorkDir(*target.image), ec);
        if (ec) return "The game's folder cannot be written to. Move it somewhere you can write to.";
    }
    struct statvfs sv {};
    if (!statvfs(GameDir().string().c_str(), &sv) && uint64_t(sv.f_bavail) * uint64_t(sv.f_frsize) < kDiskNeeded)
        return "Preparing the game needs 1 GB of free space on the drive it is on.";
    return {};
}

// the last lines of logs/compile.log, where a compiler's error is
std::string CompileLogTail() {
    constexpr size_t kTailBytes = 1500;
    std::ifstream f(CompileLog(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    if (start) tail.erase(0, tail.find('\n') + 1);  // from a whole line
    return tail;
}

void ShowFailure() {
    std::string text = "Preparing the game failed. " + g_failure;
    if (std::string tail = CompileLogTail(); !tail.empty()) text += "\n\nThe last lines of the log:\n\n" + tail;
    text += "\n\nThe full details are in logs/compile.log.";
    LOGE("%s", text.c_str());
    std::wstring wide(text.begin(), text.end());
    MessageBoxW(nullptr, wide.c_str(), kGame.title, MB_ICONERROR);
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    // one build at a time: a second copy of the game started meanwhile waits for it, then finds the .so current
    std::error_code dir_ec;
    fs::create_directories(GameDir() / "compiler", dir_ec);
    int lock = open((GameDir() / "compiler" / "build.lock").string().c_str(), O_CREAT | O_RDWR, 0644);
    if (lock >= 0) flock(lock, LOCK_EX);
    std::vector<Image> images = Images();
    std::vector<Target> stale = StaleTargets(game, images);
    for (const Target& target : stale) LOGR("Game code: building %s.so", target.image->exe.c_str());
    bool built = true;
    if (!stale.empty()) {
        if (std::string blocker = Blocker(stale); !blocker.empty()) {
            std::wstring wide(blocker.begin(), blocker.end());
            MessageBoxW(nullptr, wide.c_str(), kGame.title, MB_ICONERROR);
            built = false;
        } else {
            built = Build(stale);
            if (built) LOGR("Game code: ready");
        }
    }
    if (lock >= 0) {
        flock(lock, LOCK_UN);
        close(lock);
    }
    if (!built) return false;
    for (const Image& image : images)
        if (image.xbe == IMAGE_XBE && dispatch::Load(CodeSo(image))) return true;
    MessageBoxW(nullptr, L"The game's code could not be loaded, see the log in the logs folder.", kGame.title,
                MB_ICONERROR);
    return false;
}

}  // namespace app
