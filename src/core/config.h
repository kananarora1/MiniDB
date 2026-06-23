#pragma once
#include <cstdint>
#include <cstddef>

// HeapHackers MiniDB - global compile-time constants.
// Kept in one spot so the page geometry and pool sizing are easy to reason about.
namespace heapdb {

// Disk page size. 4 KiB lines up with a typical OS page / SSD read unit, so one
// logical page fetch maps to roughly one physical read.
inline constexpr std::size_t kPageBytes = 4096;

// Sentinel for "no page".
inline constexpr int64_t kNoPage = -1;

// Default number of frames the frame cache keeps resident.
inline constexpr std::size_t kDefaultFrames = 64;

// Sentinel transaction id meaning "never set" (used for version end-stamps).
inline constexpr uint64_t kNoTxn = 0;

// Concurrency-control discipline a transaction runs under. The engine defaults
// to snapshot-isolation MVCC (Track B); `SET isolation = 2pl` switches it to
// strict two-phase locking so transactions take shared/exclusive locks held to
// commit, with deadlock detection.
enum class Iso : uint8_t { Mvcc, TwoPL };

// Durability mode for COMMIT.
//   false (default): the commit record is written through to the OS but not
//     fsync'd per commit; the WAL is fsync'd at checkpoint/shutdown. Committed
//     data survives a process crash (our crash model: the OS keeps the page
//     cache), at a large throughput win. This mirrors SQLite's
//     `PRAGMA synchronous = NORMAL` / Postgres `synchronous_commit = off`.
//   true: fsync the WAL on every commit for power-loss durability, at the cost
//     of one disk flush per transaction.
inline constexpr bool kSyncOnCommit = false;

}  // namespace heapdb
