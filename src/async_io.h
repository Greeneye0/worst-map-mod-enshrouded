#pragma once
// The mod's disk writes on a thread of their own: the log and the ini saves (looted chests, bosses, settings). On the
// game's thread a slow disk froze the game: a log line took 4 to 6 s on HadToRegister's drive, long enough for Windows
// to count the game as not responding (it loses focus, the taskbar shows). The game's thread only queues; the writer
// keeps the log open and writes in order. Whoever reads one of these ini files calls Flush() first.

#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace async_io {

struct Job {
    bool ini;   // an ini key, else text appended to a file
    bool erase; // ini: remove the key
    std::string path, section, key, text;
};

// A log rolls over at 10 MB, kept through restarts: the full one becomes name.old.log, so the last 10 to 20 MB are
// always there for a bug report
constexpr long long kRollBytes = 10LL * 1024 * 1024;

inline std::mutex g_lock;
inline std::condition_variable g_wake, g_idle;
inline std::deque<Job> g_jobs;
// Never destroyed: a std::thread still joinable when the game exits would end the process from its destructor
inline std::thread* g_thread = nullptr;
inline bool g_stop = false;
inline bool g_busy = false;
inline bool g_closed = false; // Stop() ran (a hot reload is unloading this copy): writes are made on the spot

// Past 10 MB: name.log -> name.old.log (the one before goes), and fp is null for a new name.log. Checked as the file
// opens too, so a session's first lines start the new log.
inline void RollIfFull(FILE*& fp, const std::string& path) {
    static bool rollFailed = false; // the file is open elsewhere: tried again next start
    if (!fp || rollFailed) return;
    _fseeki64(fp, 0, SEEK_END); // "a" reports 0 until the first write
    if (_ftelli64(fp) <= kRollBytes) return;
    std::fclose(fp);
    fp = nullptr;
    std::string old = path;
    const size_t dot = old.rfind(".log");
    old.insert(dot == std::string::npos ? old.size() : dot, ".old");
    if (!MoveFileExA(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) rollFailed = true;
}

inline void Do(const Job& j, std::unordered_map<std::string, FILE*>* open) {
    if (j.ini) {
        WritePrivateProfileStringA(j.section.c_str(), j.key.c_str(), j.erase ? nullptr : j.text.c_str(), j.path.c_str());
        return;
    }
    FILE* once = nullptr;
    FILE*& fp = open ? (*open)[j.path] : once;
    if (!fp) {
        fp = _fsopen(j.path.c_str(), "a", _SH_DENYNO);
        RollIfFull(fp, j.path);
        if (!fp) fp = _fsopen(j.path.c_str(), "a", _SH_DENYNO);
    }
    if (fp) std::fputs(j.text.c_str(), fp);
    if (once) std::fclose(once);
}

inline void Run() {
    std::unordered_map<std::string, FILE*> open; // appended files stay open
    std::unique_lock<std::mutex> lk(g_lock);
    for (;;) {
        g_wake.wait(lk, [] { return g_stop || !g_jobs.empty(); });
        if (g_jobs.empty() && g_stop) break;
        std::deque<Job> batch;
        batch.swap(g_jobs);
        g_busy = true;
        lk.unlock();
        for (const Job& j : batch) Do(j, &open);
        for (auto& [path, fp] : open) {
            if (!fp) continue;
            std::fflush(fp);
            RollIfFull(fp, path);
        }
        lk.lock();
        g_busy = false;
        if (g_jobs.empty()) g_idle.notify_all();
    }
    for (auto& [path, fp] : open)
        if (fp) std::fclose(fp);
    g_idle.notify_all();
}

inline void Queue(Job&& job) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_closed) {
            if (!g_thread) g_thread = new std::thread(Run);
            g_jobs.push_back(std::move(job));
            g_wake.notify_one();
            return;
        }
    }
    Do(job, nullptr);
}

inline void Append(const std::string& path, std::string text) {
    Queue({false, false, path, {}, {}, std::move(text)});
}

// WritePrivateProfileStringA's arguments; a null value removes the key
inline void IniWrite(const char* section, const char* key, const char* value, const char* path) {
    Queue({true, value == nullptr, path, section, key, value ? value : ""});
}

// Waits until everything queued is on disk (before reading a file this writes)
inline void Flush() {
    std::unique_lock<std::mutex> lk(g_lock);
    if (!g_thread || g_closed) return;
    g_idle.wait(lk, [] { return g_jobs.empty() && !g_busy; });
}

// Writes what is queued and ends the thread, before a hot reload unloads this copy; later writes are made on the spot
inline void Stop() {
    std::thread* t = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_closed = true;
        t = g_thread;
        g_thread = nullptr;
        g_stop = true;
        g_wake.notify_one();
    }
    if (t) {
        t->join();
        delete t;
    }
}

} // namespace async_io
