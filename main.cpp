#include <Windows.h>
#include <atomic>

// OS-specific auto-reset event implementation.
// Alternative implementation: use WaitOnAddress (may be faster.)
class auto_reset_event {
  HANDLE m_handle = NULL;

public:
  // Create/destroy an auto-reset event handle.
  auto_reset_event() : m_handle(CreateEvent(NULL, FALSE, FALSE, NULL)) {}
  ~auto_reset_event() { CloseHandle(m_handle); }

  // Noncopyable.
  auto_reset_event(const auto_reset_event &) = delete;
  auto_reset_event &operator=(const auto_reset_event &) = delete;

  // Set the event to a signaled state.
  // The event will remain signaled until somebody waits on it,
  // in which case it will become unsignaled.
  void signal() { SetEvent(m_handle); }

  // Wait until the event becomes signaled.
  // If the event is already signaled, the wait ends immediately.
  // A successful wait() on the event causes it to become unsignaled.
  void wait() { WaitForSingleObject(m_handle, INFINITE); }
};

// OS-specific type for thread IDs.
using thread_id = DWORD;
// A value that cannot be returned by get_current_thread_id().
static constexpr thread_id null_thread_id = 0;
// Get a value that uniquely identifies the currently executing thread.
thread_id get_current_thread_id() { return GetCurrentThreadId(); }

// Mutex using an event to sleep/wake threads.
// Makes no calls to the event unless necessary due to contention.
//
// # Explanation
//
// For performance, we want to avoid calling signal/wait when possible.
// This is difficult because of the following (hypothetical) scenario:
//
// 1. One thread wants to lock, and another thread is currently unlocking it.
// 2. The lock thread is *just about* to wait on the event.
// 3. The unlock thread checks m_waiters and sees 0 waiters. It does not signal.
// 4. The lock thread waits on the event indefinitely, which causes a deadlock.
//
// Here is why this deadlock scenario cannot happen:
//
// 1. The lock thread is just about to wait.
// 2. (1) would mean that m_waiters is currently > 0
// 3. For a deadlock to happen, the unlock thread needs to see m_waiters == 0.
// 4. (1) and (2) contradict (3), so a deadlock is not possible.
// 5. Furthermore, m_waiters == 0 means the lock thread is before m_waiters++.
// 6. If the unlock thread saw m_waiters == 0, then it already released m_owner.
// 7. If m_owner was released, then the lock thread will succeed to CAS it.
// 8. Since the lock thread will succeed to CAS the owner, there is no deadlock.
//
// Based on the above, there can be no deadlock under the following conditions:
//
// 1. The lock thread updates m_waiters before doing a CAS of m_owner.
// 2. The unlock thread releases `m_owner` before loading `m_waiters`.
//
// Ordering the two operations could be done by making them both `seq_cst`.
// However, acquire/release semantics are not necessary on `m_waiters`.
// This is because acquire/release is already implemented using `m_owner`.
// Therefore, instead use a fence to order the two operations.
// I saw better performance using a fence than atomics on ARM Cortex-A57.
class event_mutex {
  // The current owner of the mutex.
  std::atomic<thread_id> m_owner{null_thread_id};

  // The number of threads waiting for the mutex.
  std::atomic<uint64_t> m_waiters{0};

  // Event used to sleep/wake threads when contention happens.
  auto_reset_event m_event;

public:
  // Acquire a lock on the mutex.
  // If the mutex is already held, then sleep until we acquire it.
  // The lock is not guaranteed to be fair or ordered.
  void lock() {
    const thread_id this_thread_id = get_current_thread_id();

    // Fast-path: try to get ownership as fast as possible.
    {
      thread_id expected = null_thread_id;
      if (m_owner.compare_exchange_strong(expected, this_thread_id,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
        // Success!
        return;
      }
    }

    // Signal our intention to wait.
    m_waiters.fetch_add(1, std::memory_order_relaxed);

    // Require that m_waiters is updated before touching m_owner.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Keep trying to lock until we succeed.
    while (true) {
      // Attempt to lock.
      thread_id expected = null_thread_id;
      if (m_owner.compare_exchange_strong(expected, this_thread_id,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
        // Success! And we no longer need to be a waiter.
        m_waiters.fetch_sub(1, std::memory_order_relaxed);
        return;
      }

      // Sleep until we can try again.
      m_event.wait();
    }
  }

  // Attempt to get ownership of the mutex.
  // Succeeds or fails immediately with no blocking.
  // Returns true on success, false on failure.
  bool try_lock() {
    const thread_id this_thread_id = get_current_thread_id();

    // Just try getting ownership.
    thread_id expected = null_thread_id;
    return m_owner.compare_exchange_strong(expected, this_thread_id,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed);
  }

  // Release ownership of the mutex acquired by a preivous call to lock().
  // May only be called by a thread that currently owns the mutex.
  void unlock() {
    // Relinquish ownership.
    m_owner.store(null_thread_id, std::memory_order_release);

    // Require that m_owner is updated before touching m_waiters.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Wake up waiters if there are any.
    if (m_waiters.load(std::memory_order_relaxed) > 0) {
      m_event.signal();
    }
  }
};

// Basic test case.
// Increment a counter in parallel and check that the result is consistent.
// To make it likely fail, comment out the lock() and unlock() in worker_main.

struct worker_context {
  // The event to wait on before doing the work.
  HANDLE start_event = NULL;

  // The shared lock. Ownership is required to access the shared counter.
  event_mutex *counter_lock = nullptr;

  // The shared counter that all workers are incremenet in parallel.
  unsigned long long *shared_counter = nullptr;

  // The number of iterations of counter increments this worker should do.
  unsigned long long num_loops = 0;
};

DWORD WINAPI worker_main(LPVOID lpParam) {
  worker_context *context = static_cast<worker_context *>(lpParam);

  // Wait for the start signal.
  WaitForSingleObject(context->start_event, INFINITE);

  // Do the contentious work.
  for (unsigned long long loop = 0; loop < context->num_loops; loop++) {
    // Acquire ownership of the shared counter.
    context->counter_lock->lock();

    // Read the shared counter.
    unsigned long long counter = *context->shared_counter;

    // Compiler barrier to split the counter read/write and avoid optimizations.
    std::atomic_signal_fence(std::memory_order_seq_cst);

    // Update the shared counter.
    *context->shared_counter = counter + 1;

    // Release ownership of the shared counter.
    context->counter_lock->unlock();
  }

  return 0;
}

int main() {
  // Number of worker threads.
  const unsigned num_workers = 16;
  // Number of iterations per worker.
  const unsigned long long num_loops_per_worker = 100'000;

  // Event to signal the workers to start working.
  HANDLE start_event =
      CreateEvent(NULL, /*bManualReset*/ TRUE, /*bInitialState*/ FALSE, NULL);

  // Worker threads.
  HANDLE threads[num_workers];
  worker_context contexts[num_workers];

  // Non-atomic counter shared between the workers, protected by a mutex.
  event_mutex counter_lock;
  unsigned long long shared_counter = 0;

  // Initialize worker threads.
  for (unsigned w = 0; w < num_workers; w++) {
    worker_context *context = &contexts[w];
    context->start_event = start_event;
    context->counter_lock = &counter_lock;
    context->shared_counter = &shared_counter;
    context->num_loops = num_loops_per_worker;
    threads[w] = CreateThread(NULL, 0, worker_main, context, 0, NULL);
  }

  // Make the workers start doing contentious work.
  SetEvent(start_event);

  // Wait until all work is done.
  WaitForMultipleObjects(num_workers, threads, /*bWaitAll*/ TRUE, INFINITE);

  // Clean up the workers.
  for (unsigned w = 0; w < num_workers; w++) {
    CloseHandle(threads[w]);
  }
  CloseHandle(start_event);

  // Check that the result is as expected.
  if (num_loops_per_worker * num_workers != shared_counter) {
    OutputDebugStringW(L"Test failed!\n");
    return -1;
  }

  OutputDebugStringW(L"Test passed!\n");
  return 0;
}