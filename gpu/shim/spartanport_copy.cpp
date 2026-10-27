// SPDX-License-Identifier: GPL-2.0-or-later
#include "spartanport_copy.h"
#include "spartanport_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spartanport_toggles.h"
#include "common/thread.h"

namespace SpartanCopy {
namespace {

thread_local bool in_copy_thread = false;
/// Yields while waiting for copies to finish (#209 CPU report).
std::atomic<unsigned long long> yields{0};
/// SetDeferredPoster: asks the GPU command thread to call RunDeferred().
std::atomic<void (*)()> deferred_poster{nullptr};
/// SetGpuSideCheck: whether the calling thread handles faults on GPU-written memory inline.
std::atomic<bool (*)()> gpu_side_check{nullptr};

bool OnGpuSide() {
    const auto check = gpu_side_check.load(std::memory_order_acquire);
    return !check || check();
}

/// Runs `work` under a recovery point: on a copy thread, a read of guest memory the GPU wrote jumps
/// back here (page_manager.cpp declines the fault) instead of waiting for the GPU thread, which may
/// be waiting for this copy. False: `work` did not finish and must run again on a GPU-side thread.
template <typename Work>
bool RunRecoverable(Work& work) {
    sigjmp_buf recover;
    sigjmp_buf* volatile previous = runtime_fault_recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = previous;
        return false;
    }
    runtime_fault_recover = &recover;
    work();
    runtime_fault_recover = previous;
    return true;
}

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process (Steam Deck 2).
        unsigned count = std::clamp(SpartanThreads::Available() / 4, 1u, 4u);
        if (const char* env = std::getenv("SPARTAN_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    void Async(std::function<void()> task) {
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            async_tasks.push_back({std::move(task), epoch});
        }
        cv.notify_one();
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // The waiting thread helps instead of only spinning. Off the GPU side (a fault there
        // would wait for the GPU thread, which may be waiting for this copy) it runs copies as a
        // copy thread does and leaves the ones stopped at a fault alone.
        const bool gpu_side = OnGpuSide();
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                // Tasks a copy thread stopped at a fault first: only a GPU-side thread finishes them.
                if (gpu_side && !deferred.empty()) {
                    task = std::move(deferred.front());
                    deferred.pop_front();
                } else if (!async_tasks.empty()) {
                    task = std::move(async_tasks.front());
                    async_tasks.pop_front();
                }
            }
            if (task.run) {
                RunAsync(task, !gpu_side);
            } else {
                yields.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < count) {
            yields.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        {
            std::scoped_lock lk{mutex};
            if (current == job) {
                current.reset();
            }
        }
        // Parts a copy thread stopped at a fault run here, where the fault is handled.
        for (const std::size_t i : job->failed) {
            task(i);
        }
    }

    /// RunDeferred(): the tasks copy threads stopped at a fault, on this (GPU-side) thread.
    void RunDeferred() {
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (deferred.empty()) {
                    return;
                }
                task = std::move(deferred.front());
                deferred.pop_front();
            }
            RunAsync(task);
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        std::function<void()> run;
        u64 epoch{};
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::mutex failed_mutex;
        std::vector<std::size_t> failed; ///< parts stopped at a fault, run again by the caller
    };

    /// `recoverable`: on a copy thread (Loop), where a fault on GPU-written memory stops the task.
    void RunAsync(Task& task, bool recoverable = false) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        bool finished = true;
        if (recoverable) {
            finished = RunRecoverable(task.run);
        } else {
            task.run();
        }
        in_copy_thread = was;
        if (!finished) {
            Defer(std::move(task));
            return;
        }
        Complete(task.epoch);
    }

    /// A task stopped at a fault stays pending (its epoch's callbacks wait) until a GPU-side
    /// thread runs it: WaitAll(), or the GPU command thread asked through the poster.
    void Defer(Task task) {
        bool first;
        {
            std::scoped_lock lk{mutex};
            first = deferred.empty();
            deferred.push_back(std::move(task));
        }
        static std::atomic<unsigned> logged{0};
        if (const unsigned n = logged.fetch_add(1, std::memory_order_relaxed); n < 8) {
            std::printf("Copy threads: a copy reached memory the GPU wrote; a GPU-side thread runs it again (%u)\n",
                        n + 1);
            std::fflush(stdout);
        }
        if (auto* post = deferred_poster.load(std::memory_order_acquire); first && post) {
            post();
        }
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch) {
        std::unique_lock lk{mutex};
        --epochs[epoch - epoch_base].pending;
        --pending;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                for (auto& callback : epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
    }

    static void Work(Job& job, bool recoverable = false) {
        const bool was = in_copy_thread;
        in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            auto part = [&job, i] { (*job.task)(i); };
            if (!recoverable) {
                part();
            } else if (!RunRecoverable(part)) {
                std::scoped_lock lk{job.failed_mutex};
                job.failed.push_back(i);
            }
            job.done.fetch_add(1, std::memory_order_release);
        }
        in_copy_thread = was;
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("spartanport:Copy" + std::to_string(index)).c_str());
        SpartanThreads::RegisterCpu("copy" + std::to_string(index));
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [&] {
                return stop || !async_tasks.empty() || (generation != seen && current);
            });
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task = std::move(async_tasks.front());
                async_tasks.pop_front();
                lk.unlock();
                RunAsync(task, true);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job, true);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::deque<Task> deferred; ///< stopped at a fault on a copy thread (RunAsync), still pending
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    bool draining = false;
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;

} // namespace

unsigned long long Yields() {
    return yields.load(std::memory_order_relaxed);
}

bool InCopyThread() {
    return in_copy_thread;
}

void SetDeferredPoster(void (*post)()) {
    deferred_poster.store(post, std::memory_order_release);
}

void RunDeferred() {
    GetPool().RunDeferred();
}

void SetGpuSideCheck(bool (*check)()) {
    gpu_side_check.store(check, std::memory_order_release);
}

bool Enabled() {
    return GetPool().Enabled() && !SpartanToggle::Disabled(SpartanToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (batch.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
    batch.items = {};
    batch.items.reserve(256);
    batch.bytes = 0;
    Async([items] {
        for (const auto& item : *items) {
            item.run(item);
        }
    });
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    if (!batch.items.empty()) {
        for (const auto& item : batch.items) {
            item.run(item);
        }
        batch.items.clear();
        batch.bytes = 0;
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task);
}

} // namespace SpartanCopy
