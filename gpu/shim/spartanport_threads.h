// SPDX-License-Identifier: GPL-2.0-or-later
// spartanport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace SpartanThreads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// Statistics: CPU use of the GPU-side threads, measured from inside the process (the Xbox has
/// no outside profiler). Threads register themselves; CpuReport() gives each one's share of the
/// time since the previous report.
struct CpuThread {
    std::string name;
    HANDLE handle;
    unsigned long long last;
};
inline std::mutex cpu_threads_mutex;
inline std::vector<CpuThread> cpu_threads;

inline void RegisterCpu(const std::string& name) {
    HANDLE handle{};
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        std::scoped_lock lock{cpu_threads_mutex};
        cpu_threads.push_back({name, handle, 0});
    }
}

inline std::string CpuReport(double seconds) {
    std::scoped_lock lock{cpu_threads_mutex};
    std::string out;
    for (auto& thread : cpu_threads) {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(thread.handle, &created, &exited, &kernel, &user)) {
            continue;
        }
        const unsigned long long total =
            (static_cast<unsigned long long>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
            (static_cast<unsigned long long>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        if (thread.last != 0) {
            char item[96];
            std::snprintf(item, sizeof(item), "%s%s %.0f%%", out.empty() ? "" : ", ",
                          thread.name.c_str(), (total - thread.last) / (seconds * 1e5));
            out += item;
        }
        thread.last = total;
    }
    return out;
}
#else
inline void RegisterCpu(const std::string&) {}
inline std::string CpuReport(double) {
    return {};
}

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

} // namespace SpartanThreads
