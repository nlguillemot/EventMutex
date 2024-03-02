# EventMutex

Proposed lock-free mutex. Is lock-free unless contended.

Note: The name "lock-free mutex" is a contradiction, I'm looking for a better name. 🙂

## Pros/Cons

Useful when you don't anticipate contention. Here are some example scenarios:

* The data is rarely accessed concurrently.
* The locks are generally short-lived.
* You have few cores so there is little parallelism in the first place.

Some other benefits:

* Does not use spin locks, so it works with non-preemptive schedulers.
* Does not require WaitOnAddress/futex support.

Some cons:

* No guarantee of fairness or order between accesses.
* Probably better to use WaitOnAddress/futex if possible?

## Implementation

Uses two variables with sequentially consistent ordering to avoid deadlocks.

This design is inspired by [Dekker's algorithm](https://en.wikipedia.org/wiki/Dekker%27s_algorithm).

The lock has three member variables:

* `m_owner` : the thread ID of the current owner of the lock.
* `m_waiters` : the number of threads waiting on the lock.
* `m_event` : an auto-reset event to sleep/wake threads during contention.

For performance, we want to avoid calling signal/wait when possible.  
This is difficult because of the following (hypothetical) scenario:

1. One thread wants to lock, and another thread is currently unlocking it.
2. The lock thread is *just about* to wait on the event.
3. The unlock thread checks `m_waiters` and sees 0 waiters. It does not signal.
4. The lock thread waits on the event indefinitely, which causes a deadlock.

Here is why this deadlock scenario cannot happen:

1. The lock thread is just about to wait.
2. (1) would mean that `m_waiters` is currently > 0
3. For a deadlock to happen, the unlock thread needs to see `m_waiters == 0`.
4. (1) and (2) contradict (3), so a deadlock is not possible.
5. Furthermore, `m_waiters == 0` means the lock thread is before `m_waiters++`.
6. If the unlock thread saw `m_waiters == 0`, then it already released `m_owner`.
7. If `m_owner` was released, then the lock thread will succeed to CAS it.
8. Since the lock thread will succeed to CAS `m_owner`, there is no deadlock.

Based on the above, there can be no deadlock under the following conditions:

1. The lock thread updates `m_waiters` before doing a CAS of `m_owner`.
2. The unlock thread releases `m_owner` before loading `m_waiters`.

Ordering the two operations could be done by making them both `seq_cst`.  
However, acquire/release semantics are not necessary on `m_waiters`.  
This is because acquire/release is already implemented using `m_owner`.  
Therefore, instead use a fence to order the two operations.  
I saw better performance using a fence than atomics on ARM Cortex-A57.

## Build/Test

1. Open EventMutex.sln using Visual Studio 2019.
2. Build and run.
3. Expected command-line output: "Test passed!"

## Usage

1. Copy `event_mutex` and the code it uses into your project.
2. Replace the Windows-specific APIs with the APIs of your platform.

## Disclaimer

I'm not aware of bugs but I also haven't proven its correctness thoroughly.  
If you do find a bug, please let me know! This is a proposal after all.

Use at your own risk.
