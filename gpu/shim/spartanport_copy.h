// spartanport: parallel copies of guest memory. Streaming a new area uploads 50-400 MB of textures
// and buffers per frame; copied on one thread that is tens of milliseconds (stutter).
#pragma once
#include <cstddef>
#include <functional>

namespace SpartanCopy {

/// Runs `task(i)` for every i in [0, count) on the copy threads and the caller; returns when
/// all are done. Runs inline when count is 1, the pool is disabled, or when called from a copy
/// thread (no nesting).
void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task);

/// True when ParallelFor would split work (more than one thread available).
bool Enabled();

/// Starts `task` on a copy thread now (inline when the pool is disabled). Tasks run in any
/// order and concurrently.
void Async(std::function<void()> task);

/// A small copy for QueueCopy(): `run(item)` does the work on a copy thread.
struct Item {
    void (*run)(const Item&);
    void* context;
    unsigned long long source, destination, size, extra;
};

/// Queues a small copy in the calling thread's batch; the batch is started with Async() once it
/// holds about 512 KiB or 256 items, or by FlushBatch()/WaitAsync(). One wakeup per batch
/// instead of one per copy (thousands of constant buffer copies per frame).
void QueueCopy(const Item& item);

/// Starts the calling thread's batch.
void FlushBatch();

/// Starts the calling thread's batch, then waits until every task passed to Async() so far
/// has finished.
void WaitAsync();

/// Starts the calling thread's batch and runs `callback` once every task passed to Async() so
/// far has finished (at once when none is pending), on whichever thread finishes last.
/// Callbacks run in the order they were registered.
void AfterCopies(std::function<void()> callback);

/// Times a thread yielded while waiting for copies (diagnostics, #209).
unsigned long long Yields();

/// True on a copy thread while it runs a task (and on any thread running copy work inline).
bool InCopyThread();

/// A copy thread that reads guest memory the GPU wrote must not wait for the GPU thread to read
/// it back: that thread may be waiting for the copy (a deadlock). Such a task stops at the fault
/// (page_manager.cpp) and stays pending until a GPU-side thread runs it again with RunDeferred(),
/// where the fault is handled inline. WaitAsync() does so; `post` (set by the GPU command thread)
/// asks that thread to call RunDeferred() without waiting for it.
void SetDeferredPoster(void (*post)());
void RunDeferred();

/// `check` (set by the GPU command thread) tells whether the calling thread is GPU-side, where
/// faults on GPU-written memory are handled inline. WaitAsync() on any other thread runs copies
/// under a recovery point, as a copy thread does, and leaves the stopped ones to GPU-side threads.
void SetGpuSideCheck(bool (*check)());

} // namespace SpartanCopy
