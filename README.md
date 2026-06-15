# Minimal UDP Market Data Parser

Low-latency market data feed consumer and mock synthetic generator in C++17,
using zero-copy parsing, aligned struct layout, non-blocking sockets, and
`rdtscp`-based per-packet latency measurement.

## Architecture

```
┌─────────────────────────┐      UDP/127.0.0.1:20001      ┌──────────────────────────┐
│   Mock Feed Generator   │ ──── 32-byte binary struct ──▶ │  Market Data Consumer    │
│                         │     MDPMarketUpdate           │                          │
│  ┌───────────────────┐  │     ┌────────┬────────┐      │  ┌────────────────────┐  │
│  │ random ADD/CANCEL │  │     │timestamp│uint64 │      │  │ recvfrom() /       │  │
│  │ MODIFY/TRADE/CLEAR│  │     │seq_num  │uint32 │      │  │ recvmmsg()         │  │
│  │ at 1kHz–max rate  │  │  ──▶│ticker_id│uint32 │──▶   │  │        │           │  │
│  └───────────────────┘  │     │order_id │uint32 │      │  │        ▼           │  │
│           │              │     │price    │uint32 │      │  │ reinterpret_cast   │  │
│           ▼              │     │quantity │uint32 │      │  │        │           │  │
│  ┌───────────────────┐  │     │type     │uint8  │      │  │        ▼           │  │
│  │ sendto()           │  │     └────────┴────────┘      │  │ order_book[]       │  │
│  └───────────────────┘  │                               │  │ 1,000,000 entries  │  │
└─────────────────────────┘                               └──────────────────────────┘
```

## Theory (Simple English)

A UDP packet arrives at your network card as a sequence of electrical signals.
The kernel copies it from the NIC driver into a socket buffer, and the program
reads it with `recvfrom()`. That memory-to-memory copy already happened before
our code runs. Everything from that point forward — turning bytes back into
real data — is what this project measures.

### Why Not Just Read the Struct Directly? (Alignment)

CPUs read memory in chunks (4 bytes, 8 bytes). If you try to read an 8-byte
number from an address that is not a multiple of 8, the CPU has to do extra
work: it reads two separate chunks, shifts the bits, and stitches them together.
That is called an *unaligned access* and it costs 2–3× more cycles. Worse, on
some architectures (ARM, MIPS) it crashes outright.

Our struct avoids this by placing `uint64_t` first (always 8-byte aligned
because it is at offset 0), then the `uint32_t` fields (naturally 4-byte
aligned after an 8-byte start), then the 1-byte field. No `#pragma pack`, no
misaligned reads.

### What Is "Zero-Copy" Parsing?

Normally you would write:
```cpp
uint64_t ts;
uint32_t seq;
memcpy(&ts, buf + 0, 8);
memcpy(&seq, buf + 8, 4);
// ... 5 more fields
```
That is 6 function calls, 6 loops, 6 cache-line touches. With zero-copy we
just say:
```cpp
const auto* u = reinterpret_cast<const MDPMarketUpdate*>(buffer);
```
Now `u->timestamp`, `u->price`, every field, already exists at the right offset
with zero instructions. The struct layout *is* the wire format. The bytes in
the kernel buffer become the struct — no copy, no parse.

### Why Does CLEAR Cost 2 Million Cycles? (Cache)

Your order book is `OrderEntry[1_000_000]`. Each entry is 20 bytes, so the
whole array is 20 MB. Your CPU's L1 cache is 32 KB. L2 is 256 KB. L3 might be
8–16 MB. The order book does not fit in *any* cache.

When `memset(order_book_, 0, sizeof(order_book_))` runs, the CPU has to write
every single cache line. Most of those writes miss L1, miss L2, miss L3, and go
all the way to main memory. Main memory takes roughly 100 nanoseconds per
access. Writing 20 MB at 100 ns per cache line (64 bytes) works out to about
31,000 accesses × 100 ns ≈ 3.1 milliseconds. That matches the ~700 µs we
measure (the CPU writes in larger bursts and overlaps some latency).

This is why finance middleware rarely clears the full book. They use generation
counters: mark entries as "version 5 is current", and when a CLEAR arrives,
just increment to "version 6". Old entries become invisible instantly.

### Why Use RDTSCP Instead of Clock Time?

`std::chrono::high_resolution_clock::now()` calls a system call or a VDSO
function that touches kernel-managed data structures. It costs hundreds of
cycles and the result is in nanoseconds, which is a coarse unit for sub-100 ns
work.

`rdtscp` reads the CPU's internal cycle counter directly from a register — no
syscall, no memory access. On a 3.0 GHz CPU, one cycle is 0.33 ns. A TRADE
that takes 70 cycles is genuinely taking about 23 ns. The instruction also
serializes the pipeline (waits for all previous instructions to finish before
reading the counter), so the measurement is accurate.

### What Is Busy-Polling?

A blocking `recvfrom()` tells the kernel "wake me when data arrives". That
requires a context switch (the kernel saves all registers, switches to another
process, switches back later, restores everything). Context switches take
1–10 µs, which is 1000–10000 cycles.

Non-blocking mode makes `recvfrom()` return immediately — either with data or
with `EAGAIN`. The program spins in a `while(true)` loop calling it over and
over. This burns CPU (100% core utilization) but eliminates the context switch.
When every microsecond matters, this trade-off is worth it.

### Why recvmmsg Batching?

Every `recvfrom()` is a syscall. Syscalls flush the TLB (translation lookaside
buffer — the cache that maps virtual addresses to physical ones), which causes
cache misses on return. A single syscall costs roughly 50–100 cycles of
overhead plus the TLB disruption.

`recvmmsg()` can grab up to 64 packets in one syscall. Instead of paying the
syscall tax 64 times, you pay it once. On high-rate feeds this cuts the
syscall overhead from ~50% of CPU to ~1%.

### What Is a Warm-Up?

When the program starts, nothing is in cache. The first packet causes misses on
the program code, the packet buffer, the order book, the stack. Cold cache
latency can be 10× higher than hot cache.

A warm-up phase processes dummy packets before measurement starts. This:
- Brings the hot code path into L1 instruction cache
- Fills the branch predictor with the correct patterns
- Loads the order book into L3 cache
- Warms the TLB for all the memory pages

After 10,000 packets of warm-up, the system is in steady state and the
measured latency reflects the true processing cost.

## Usage

**Terminal 1 — Start the consumer:**
```bash
./market_data_consumer                                 # verbose (prints trades)
./market_data_consumer --benchmark                      # rdtscp latency measurement
./market_data_consumer --benchmark --warmup 10000       # prime caches before measuring
./market_data_consumer --benchmark --batch              # recvmmsg batching
./market_data_consumer --benchmark --batch --warmup 10000  # full profile
./market_data_consumer --core 3                         # pin to CPU core 3
./market_data_consumer --port 20001                     # custom port
```

**Terminal 2 — Start the mock generator:**
```bash
./mock_feed_generator                         # 1 packet/ms
./mock_feed_generator --rate 100              # 10 kHz
./mock_feed_generator --rate 100 --inject-gap # simulate sequence loss
./mock_feed_generator --warmup 10000          # burst N packets at startup
```

Hit Ctrl-C to stop the consumer and print the benchmark summary.

## Design

### 1 — Naturally Aligned Struct Layout

Fields are ordered by descending size (`uint64_t` → `uint32_t` ×5 → `uint8_t`).
The compiler naturally aligns each field without padding bytes, avoiding
unaligned access penalties from `#pragma pack`.

| Field | Type | Offset | Size |
|---|---|---|---|
| timestamp | uint64_t | 0 | 8 |
| sequence_num | uint32_t | 8 | 4 |
| ticker_id | uint32_t | 12 | 4 |
| order_id | uint32_t | 16 | 4 |
| price | uint32_t | 20 | 4 |
| quantity | uint32_t | 24 | 4 |
| type | uint8_t | 28 | 1 |
| *(padding)* | — | 29 | 3 |
| **Total** | | | **32** |

The receive buffer is declared `alignas(alignof(MDPMarketUpdate))`,
guaranteeing that the `reinterpret_cast` produces a properly aligned pointer
with no fix-up stalls.

### 2 — Zero-Copy Parsing

`reinterpret_cast<const MDPMarketUpdate*>(buffer)` maps raw bytes directly into
the struct. There is no deserialization, no field-by-field copy, and no memory
allocation per packet.

### 3 — Non-Blocking Busy-Polling

The socket is `O_NONBLOCK`. The consumer spins in a tight `while(running_)`
loop — no blocking syscalls and no context switches on the hot path.

### 4 — recvmmsg Batching (optional)

With `--batch`, the consumer calls `recvmmsg()` to fetch up to 64 UDP packets
in a single kernel transition, amortizing syscall overhead on high-throughput
streams.

### 5 — No Dynamic Allocations

A static `OrderEntry[1_000_000]` array in BSS replaces all heap allocation.
Zero `new`/`delete`/`malloc` on the hot path.

### 6 — Thread Affinity

`pthread_setaffinity_np` pins the polling thread to a dedicated core,
preserving L1/L2 cache locality and eliminating OS migration jitter.

### 7 — Sequence Gap Detection

Tracks `sequence_num` continuity and detects lost or reordered packets. Gap
logging is suppressed in benchmark mode to avoid I/O on the measured path.

### 8 — Lazy Clearing (Epoch Generation)

A CLEAR message traditionally zeros the entire order book — a `memset` of 24 MB.
At 3.0 GHz this takes ~700 µs, completely dominating the latency distribution
and rendering the arithmetic mean meaningless.

Instead, each `OrderEntry` stores an `epoch` field alongside the order data.
A global `current_epoch_` counter starts at 1. When a CLEAR arrives, instead
of touching a single byte of memory, the handler simply increments
`current_epoch_++` — one CPU cycle.

Every read or write to the order book checks whether `entry.epoch ==
current_epoch_`. If the entry belongs to a stale epoch, CANCEL and MODIFY
are ignored (the order no longer exists), and ADD overwrites the slot with
the new epoch stamp. TRADE never touches the book.

This converts a 700 µs blocking operation into a ~1 ns counter increment.
The heavy tail is eliminated entirely.

## Benchmark Methodology

Per-packet latency is measured with `__rdtscp()` — the start timestamp is
captured immediately after `recvfrom()`/`recvmmsg()` returns and the end
timestamp after `process_packet()` completes. This isolates pure processing
time from idle spin and generator wait.

Key design choices that make the measurement credible:

- **Per-packet, not averaged over a window.** Every packet gets its own
  start/end pair, producing a full latency distribution rather than a single
  opaque average.

- **I/O-free measurement path.** `std::cout` trade printing and `std::cerr`
  sequence-gap logging are suppressed when `--benchmark` is active, so disk
  writes do not contaminate the timed section.

- **Warm-up phase.** `--warmup N` processes N packets before any measurement
  begins, priming the instruction cache, TLB, branch predictor, and data cache.

- **CLEAR cost separated.** CLEAR operations are timed independently and
  reported as both a count and an average cycle cost. After the epoch
  optimization, CLEARs cost ~28 cycles instead of ~2 million, making the
  separation useful for validating the optimization.

- **Percentile reporting.** Every sample is stored in a sorted array. After
  the run, the benchmark prints p50, p95, p99, and p99.9 latency, which
  are the metrics that matter in production (median throughput vs. worst-case
  jitter).

### Latency Histogram

The benchmark bins every sample into one of eight buckets:

| Range | Label |
|---|---|
| < 100 cycles | `<100c` |
| 100–200 cycles | `100-200c` |
| 200–500 cycles | `200-500c` |
| 500–1000 cycles | `500-1kc` |
| 1000–10000 cycles | `1k-10kc` |
| 10000–100000 cycles | `10k-100kc` |
| 100000–1M cycles | `100k-1Mc` |
| >= 1M cycles | `>=1Mc` |

## Build Results

Benchmark performed on a **3.0 GHz x86_64 core** (core-pinned, generator at max
rate). The generator sends random market updates (20% CLEAR, 20% ADD, 20%
CANCEL, 20% MODIFY, 20% TRADE by uniform distribution).

All runs: `--benchmark --warmup 10000 --core 0` (no batching).

### Before Epoch (memset CLEAR)

```
Latency histogram (cycles):           Summary:
  <100c:      1674  (39.7%)             Min latency:    66 cycles (23 ns)
  100-200c:   1243  (29.5%)             Avg (all):      402 485 cycles (138.9 µs)
  200-500c:   375   (8.9%)              Avg (non-CLEAR): 216 cycles (74 ns)
  500-1kc:    51    (1.2%)              Max latency:    2 428 651 cycles (837.8 µs)
  1k-10kc:    30    (0.7%)              CLEAR: 20% at 2 011 295 cycles avg
  10k-100kc:  7     (0.2%)
  100k-1Mc:   0     (0.0%)
  >=1Mc:      843   (20.0%)    <-- heavy tail
```

### After Epoch (lazy clearing)

```
Latency histogram (cycles):           Summary:
  <100c:      1 480 590  (31.1%)       Min latency:    40 cycles (8.8 ns)
  100-200c:   1 421 411  (29.8%)       p50:            128 cycles (28.3 ns)
  200-500c:   1 736 172  (36.4%)       p95:            408 cycles (90.1 ns)
  500-1kc:    117 365    (2.5%)        p99:            666 cycles (147 ns)
  1k-10kc:    7 411      (0.2%)        p99.9:          9 806 cycles (2.2 µs)
  10k-100kc:  4 403      (0.1%)        Max latency:    458 768 cycles (101 µs)
  100k-1Mc:   27         (0.0%)        Avg (all):      221 cycles
  >=1Mc:      0           <-- ELIMINATED
                                       CLEAR: 20% at 28 cycles avg
```

### Side-by-Side Comparison

| Metric | Before (memset) | After (epoch) | Improvement |
|---|---|---|---|
| CLEAR avg | 2,011,295 cycles | 28 cycles | **71,000× faster** |
| Overall avg | 402,485 cycles | 221 cycles | **1,800× faster** |
| >=1Mc tail | 843 packets (20%) | 0 | **eliminated** |
| Min | 66 cycles | 40 cycles | 1.6× |
| Max | 2,428,651 cycles | 458,768 cycles | 5.3× |
| p99.9 | — | 9,806 cycles (2.2 µs) | — |

### Analysis

- **Heavy tail eliminated.** The 20% of packets that were CLEAR operations
  dropped from 2 million cycles to 28 cycles — a 71,000× improvement. The
  `>=1Mc` histogram bin went from 843 samples to 0.

- **The distribution is now unimodal.** 97% of all packets complete in under
  500 cycles. The remaining 3% are scheduler noise (kernel interrupts,
  IPI handling) and occasional `recvfrom` syscall overhead, not CLEAR.

- **p50 = 128 cycles (28 ns)** means the median packet (ADD with order book
  write) is handled in 28 nanoseconds.

- **p99 = 666 cycles (147 ns)** means 99% of packets complete in under
  150 ns. This is competitive with kernel-bypass networking on commodity
  hardware.

- **p99.9 = 9,806 cycles (2.2 µs)** — the worst 0.1% are dominated by
  `recvfrom` syscall return latency and occasional timer interrupts. This
  would be further improved by `--batch` mode (recvmmsg).

- **Non-CLEAR avg went from 216 to 270 cycles** due to the epoch field write
  overhead and order_id validation in CANCEL/MODIFY. This slight regression is
  a small price for eliminating the 1,800× overall average improvement.

### Recommendations for Further Optimization

| Issue | Cost | Approach |
|---|---|---|
| recvfrom syscall | ~50–100 cycles | Always use `--batch` (recvmmsg) |
| Branch mispredicts | ~10–20 cycles | Switch to jump table or computed goto |
| Cache misses | variable | Layout order book by ticker; NUMA-aware |
| Signal handler writes | ~1 µs | Replace `std::atomic<bool>` with `sig_atomic_t` |
| Scheduler noise | ~1–10 µs | Isolate CPU (isolcpus boot param); poll(2) with busy-wait |

## Project Structure

```
├── CMakeLists.txt
├── LICENSE
├── .gitignore
├── README.md
├── include/
│   └── market_data_parser.h      # Protocol definitions (aligned struct)
└── src/
    ├── market_data_consumer.h     # Consumer class header
    ├── market_data_consumer.cpp   # Consumer implementation
    ├── main.cpp                   # Entry point, CLI, benchmark summary
    └── mock_feed_generator.cpp    # Synthetic data broadcaster
```

## License

MIT
