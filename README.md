# Minimal UDP market data parser

Small C++17 project that eats a market data feed over UDP and measures how fast it can. There is a mock generator that sprays fake packets, and a consumer that parses them. Parsing is zero copy, structs are aligned, sockets are non blocking, latency is measured per packet with `rdtscp`.

## Architecture

```
┌──────────────────────────────┐      UDP      ┌──────────────────────────────┐
│      MOCK FEED GENERATOR     │──────────────▶│     MARKET DATA CONSUMER     │
│                              │  127.0.0.1    │                              │
│  ┌────────────────────────┐  │   :20001      │  ┌────────────────────────┐  │
│  │ while(running) {       │  │  32B packets  │  │ while(running) {       │  │
│  │   fill MDPMarketUpdate │──┼──────────────-┼──│   recvfrom()           │  │
│  │   sendto(sock, &upd)   │  │               │  │   reinterpret_cast<>   │  │
│  │   usleep(interval)     │  │               │  │   process_packet()     │  │
│  │ }                      │  │               │  │   update order_book[]  │  │
│  └────────────────────────┘  │               │  └────────────────────────┘  │
│                              │               │                              │
│  Types: CLEAR ADD MODIFY     │               │  Order book: 1M entries      │
│         CANCEL TRADE         │               │  in BSS, epoch-gated         │
└──────────────────────────────┘               └──────────────────────────────┘

  Wire format (32-byte naturally aligned struct):
  ┌───────┬────┬──────┬────┬─────┬─────┬────┬───┐
  │ ts    │seq │ticker│ord │price│ qty │type│pad│
  │ uint64│u32 │ u32  │u32 │ u32 │ u32 │u8  │3B │
  └───────┴────┴──────┴────┴─────┴─────┴────┴───┘
  Offset: 0     8    12    16   20    24   28   31
```

## Theory in plain words

A UDP packet shows up at the network card as electrical signals. The kernel pulls it from the NIC driver into a socket buffer, then our program reads it with `recvfrom()`. That kernel copy already happened before our code runs. What we time here starts after that, turning those bytes back into usable fields.

### Why not read the struct directly? Alignment

CPUs like to read memory in chunks, 4 bytes or 8 bytes at a time. If you ask for an 8 byte number at an address that is not a multiple of 8, the CPU has to do two reads, shift bits around, and stitch the result together. Slow. On some chips (ARM, MIPS) it just faults.

I put `uint64_t` first so it sits at offset 0, always 8 byte aligned. Then the five `uint32_t` fields follow, all naturally aligned. Then the 1 byte type at the end. No `#pragma pack`, no fixups.

### What zero copy means here

The usual way looks like this:
```cpp
uint64_t ts;
uint32_t seq;
memcpy(&ts, buf + 0, 8);
memcpy(&seq, buf + 8, 4);
// ... 5 more fields
```
That is 6 copies to pull each field out. Instead I do:
```cpp
const auto* u = reinterpret_cast<const MDPMarketUpdate*>(buffer);
```
The struct layout matches the wire exactly, so `u->price` and the rest are already there. No deserialize loop, no per packet alloc. Zero instructions for parsing, just a cast.

### Why CLEAR was so slow? Cache

The order book is `OrderEntry[1_000_000]`. At 20 bytes each that is 20 MB. L1 is 32 KB, L2 is 256 KB, L3 is maybe 8 to 16 MB on this box. So the book fits nowhere on chip.

Old code did `memset(order_book_, 0, sizeof(order_book_))` on every CLEAR. That walks every cache line, misses all the way out to DRAM. DRAM is around 100 ns a pop. 20 MB divided into 64 byte lines is about 31k lines, so you end up in the milliseconds. We measured around 700 us, which lines up once the CPU overlaps some writes.

Real feeds do not memset. They bump a generation number. I copied that trick, details in Design below.

### Why rdtscp and not chrono?

`std::chrono::high_resolution_clock::now()` goes through VDSO or kernel data, costs hundreds of cycles, and reports in ns. Too coarse for work that takes 20 ns.

`rdtscp` reads the CPU cycle counter straight from a register. No syscall, no memory touch. On a 3.0 GHz box one cycle is 0.33 ns, so a 70 cycle TRADE is about 23 ns. It also waits for earlier instructions to retire first, so the numbers do not smear.

### Busy polling

Blocking `recvfrom()` puts the thread to sleep until data comes. Wakeup means a context switch, save registers, switch out, switch back in, restore. That is 1 to 10 us, or 1000s of cycles.

Here the socket is non blocking, so `recvfrom()` returns right away with data or `EAGAIN`. The loop just spins and keeps calling it. Yes, it pins a core at 100%. For this kind of test that trade is fine.

### Why recvmmsg helps

Each `recvfrom()` is a syscall. Syscalls mess with the TLB and cost something like 50 to 100 cycles plus the disruption on return.

`recvmmsg()` grabs up to 64 packets in one call. You pay that entry cost once instead of 64 times. At high packet rates that drops syscall overhead from a big chunk of CPU to noise.

### Warm up

Fresh process, cold caches. First packets miss on code, on buffers, on the book, on page tables. Cold can look 10x slower than steady state.

So `--warmup 10000` runs 10k packets through the normal path before timing starts. That pulls hot code into L1i, trains the branch predictor, fills TLB entries. After that the numbers show real processing cost.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
```

Expected output:
```
-- Configuring done (0.0s)
-- Generating done (0.0s)
-- Build files have been written to: /home/user/udp-cpp/build
[100%] Built target market_data_consumer
[100%] Built target mock_feed_generator
```

Two binaries are produced:
- `build/market_data_consumer` - the low-latency feed consumer
- `build/mock_feed_generator` - the synthetic data broadcaster

---

## Commands to show performance

All benchmarks use `--benchmark --warmup 10000` (time each packet after 10k warmup to settle caches, TLB, and branch predictor).

### 1 - Quick smoke test, verbose

Just checks end to end works and prints trades.

**Terminal 1:**
```bash
./build/market_data_consumer --port 20001
```

**Terminal 2:**
```bash
./build/mock_feed_generator --port 20001 --rate 10000
```

Expected output (Terminal 1):
```
TRADE: ticker=1002 qty=7431 price=58295
TRADE: ticker=1005 qty=9464 price=36059
...
```

Hit Ctrl-C on Terminal 1 to stop.

---

### 2 - Baseline benchmark, core 0, noisy

Core 0 takes kernel timer ticks, daemons, IPIs, so the tail jitters. Good for showing the problem.

```bash
taskset -c 0 ./build/market_data_consumer --benchmark --warmup 10000 --port 20002 &
sleep 0.3
taskset -c 1 ./build/mock_feed_generator --port 20002 --rate 100000 --warmup 10000
sleep 2
kill %1
```

Expected output:
```
  Latency (cycles):
    Min:     34 (7.6 ns)
    p50:     134 (29.9 ns)
    p95:     674 (150.6 ns)
    p99:     906 (202.5 ns)
    p99.9:   17462 (3.9 µs)
    Max:     41156 (9.2 µs)     <-- OS jitter on Core 0
  Avg (all packets):   291 cycles
  CLEAR: 20% at 22 cycles/clear
```

---

### 3 - Isolated core, core 3, clean

Same load, consumer pinned away from interrupt heavy cores. Max tail drops a lot.

```bash
taskset -c 3 ./build/market_data_consumer --benchmark --warmup 10000 --port 20003 &
sleep 0.3
taskset -c 1 ./build/mock_feed_generator --port 20003 --rate 100000 --warmup 10000
sleep 2
kill %1
```

Expected improvement:
```
  Max latency drops from ~41k cycles (9 µs) to ~2k-10k cycles (<3 µs)
  p99.9 drops from 17k cycles to ~2k cycles
```

---

### 4 - Optimized, isolated core plus recvmmsg

Adds `--batch` to pull up to 64 packets per `recvmmsg` call instead of one per `recvfrom`. Amortized syscall cost drops to about 1 cycle per packet.

```bash
taskset -c 3 ./build/market_data_consumer --benchmark --warmup 10000 --batch --port 20004 &
sleep 0.3
taskset -c 1 ./build/mock_feed_generator --port 20004 --rate 100000 --warmup 10000
sleep 2
kill %1
```

Expected improvement:
```
  p50:  ~80-100 cycles (vs 134 no-batch)
  p95:  ~200-400 cycles (vs 674 no-batch)
  Max:  ~500-2000 cycles (vs 41k on Core 0)
```

---

### 5 - Automated suite

Runs baseline, then isolated, then isolated plus batch, back to back:

```bash
./run_benchmarks.sh
```

---

### 6 - Multicast mode (IGMP)

Same as unicast but over multicast, closer to how NASDAQ ITCH and HKEX OMD ship feeds.

**Terminal 1:**
```bash
taskset -c 3 ./build/market_data_consumer --benchmark --warmup 10000 --multicast --port 20005
```

**Terminal 2:**
```bash
taskset -c 1 ./build/mock_feed_generator --port 20005 --rate 100000 --multicast
```

Consumer joins `224.0.0.1` with `IP_ADD_MEMBERSHIP`. Generator sends to the group instead of `127.0.0.1`.

---

### 7 - High throughput stress

Push 500k packets per sec with gap injection on:

**Terminal 1:**
```bash
taskset -c 3 ./build/market_data_consumer --benchmark --warmup 10000 --batch --port 20006
```

**Terminal 2:**
```bash
taskset -c 1 ./build/mock_feed_generator --port 20006 --rate 500000 --inject-gap
```

Expected:
```
  Packets processed: ~800000
  Sequence gaps:     ~800         (0.1% injected)
  Benchmark samples: ~790000
  CLEAR: 20% at ~22 cycles/clear
  Avg (non-CLEAR):  ~300-400 cycles
```

---

## CLI reference

### market_data_consumer

| Flag | Description |
|------|-------------|
| `--benchmark` | Enable per-packet `rdtscp` latency measurement |
| `--batch` | Use `recvmmsg` batching (up to 64 pkts/syscall) |
| `--multicast` | Join IGMP multicast group `224.0.0.1` |
| `--core N` | Pin consumer to CPU core N |
| `--port N` | Listen on port N (default: 20001) |
| `--warmup N` | Discard first N packets from measurement |

### mock_feed_generator

| Flag | Description |
|------|-------------|
| `--rate N` | Send N packets per second (default: 1000) |
| `--multicast` | Send to multicast group `224.0.0.1` |
| `--inject-gap` | Skip a sequence number every 1000 packets |
| `--warmup N` | Burst N packets at startup before rate-limiting |
| `--port N` | Target port (default: 20001) |

---

## Design

### 1 - Aligned struct, biggest fields first

Fields go `uint64_t`, then five `uint32_t`, then `uint8_t`. Compiler aligns each one with no extra padding except the 3 tail bytes to round to 32. That avoids the penalty from `#pragma pack`.

| Field | Type | Offset | Size |
|---|---|---|---|
| timestamp | uint64_t | 0 | 8 |
| sequence_num | uint32_t | 8 | 4 |
| ticker_id | uint32_t | 12 | 4 |
| order_id | uint32_t | 16 | 4 |
| price | uint32_t | 20 | 4 |
| quantity | uint32_t | 24 | 4 |
| type | uint8_t | 28 | 1 |
| *(padding)* | - | 29 | 3 |
| **Total** | | | **32** |

Receive buffer is `alignas(alignof(MDPMarketUpdate))`, so the cast lands aligned. No fix up stalls.

### 2 - Zero copy parsing

`reinterpret_cast<const MDPMarketUpdate*>(buffer)` treats the bytes as the struct. No deserialize, no field copy, no alloc per packet.

### 3 - Non blocking busy poll

Socket is `O_NONBLOCK`. Consumer spins in `while(running_)`. No blocking call in the hot path, no context switch to wait for data.

### 4 - recvmmsg batching, optional

With `--batch`, one `recvmmsg()` pulls up to 64 UDP datagrams. Helps a lot when the feed runs fast.

### 5 - No allocs on hot path

Static `OrderEntry[1_000_000]` lives in BSS. Zero `new` or `malloc` while polling.

### 6 - Thread affinity

`pthread_setaffinity_np` sticks the poll loop to one core. Keeps L1 and L2 hot and stops the OS bouncing the thread around.

### 7 - Gap check

Keeps last `sequence_num`, flags skips or reorders. Logging goes to stderr, and stays off in benchmark mode so IO does not pollute timings.

### 8 - Lazy clear with epoch

Old CLEAR did a full `memset` over 24 MB. On 3.0 GHz that was about 700 us, swamped everything else and made the mean useless.

Now each `OrderEntry` has an `epoch` alongside the order. Global `current_epoch_` starts at 1. On CLEAR I just do `current_epoch_++`. One instruction, touches nothing else.

Reads and writes check `entry.epoch == current_epoch_`. Stale entries look dead. CANCEL and MODIFY skip them. ADD overwrites the slot and stamps the new epoch. TRADE does not touch the book at all.

That turned a 700 us stall into a counter bump.

## Benchmark method

Start stamp is taken right after `recvfrom()` or `recvmmsg()` returns, end stamp after `process_packet()` finishes, both with `__rdtscp()`. So idle spin and generator sleep are outside the window. Pure processing only.

A few things I did to keep numbers honest:

- Per packet timing, not window averages. Each packet gets its own start and end, then I keep the full distribution.

- No IO while timing. Trade prints to `std::cout` and gap logs to `std::cerr` are off when `--benchmark` is set.

- Warm up first. `--warmup N` runs N packets unmeasured to fill icache, TLB, predictor, data cache.

- CLEARs timed separately. They are counted and averaged on their own. After the epoch fix they run about 28 cycles instead of 2M, and splitting them out makes that obvious.

- Percentiles, not just mean. All samples go in an array, sort at end, print p50, p95, p99, p99.9. Median tells you typical cost, tail tells you jitter.

### Latency histogram

Each sample lands in one of eight bins:

| Range | Label |
|---|---|
| < 100 cycles | `<100c` |
| 100-200 cycles | `100-200c` |
| 200-500 cycles | `200-500c` |
| 500-1000 cycles | `500-1kc` |
| 1000-10000 cycles | `1k-10kc` |
| 10000-100000 cycles | `10k-100kc` |
| 100000-1M cycles | `100k-1Mc` |
| >= 1M cycles | `>=1Mc` |

## Build results

Ran on a 3.0 GHz x86_64 core, pinned, generator at max rate. Generator sends uniform random updates (20% CLEAR, 20% ADD, 20% CANCEL, 20% MODIFY, 20% TRADE).

All runs: `--benchmark --warmup 10000 --core 0` (no batching).

### Before epoch (memset CLEAR)

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

### After epoch (lazy clearing)

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

### Side-by-side comparison

| Metric | Before (memset) | After (epoch) | Improvement |
|---|---|---|---|
| CLEAR avg | 2,011,295 cycles | 28 cycles | **71,000× faster** |
| Overall avg | 402,485 cycles | 221 cycles | **1,800× faster** |
| >=1Mc tail | 843 packets (20%) | 0 | **eliminated** |
| Min | 66 cycles | 40 cycles | 1.6× |
| Max | 2,428,651 cycles | 458,768 cycles | 5.3× |
| p99.9 | - | 9,806 cycles (2.2 µs) | - |

### What I take from this

- Heavy tail is gone. CLEARs went from 2M cycles to 28. The `>=1Mc` bin went from 843 hits to zero. That 20% chunk was the whole story.

- Rest looks tight. 97% of packets finish under 500 cycles now. Leftovers are scheduler noise and `recvfrom` cost, not CLEAR.

- p50 is 128 cycles (28 ns). That is a normal ADD with a book write.

- p99 is 666 cycles (147 ns). 99% under 150 ns on plain hardware is fine for my use.

- p99.9 is 9,806 cycles (2.2 us). That slice is mostly syscall return plus timer ticks. `--batch` helps here.

- Non CLEAR avg ticked up from 216 to 270 cycles because of the extra epoch write and order_id check in CANCEL and MODIFY. Worth it for the 1800x win on the mean.

### Ideas if I keep tuning

| Issue | Cost | Approach |
|---|---|---|
| recvfrom syscall | ~50-100 cycles | Always use `--batch` (recvmmsg) |
| Branch mispredicts | ~10-20 cycles | Switch to jump table or computed goto |
| Cache-line thrashing | variable | `alignas(64)` on `OrderEntry` (implemented) |
| Scheduler noise | ~1-10 µs | Pin to non-zero core via `--core N` or `taskset` |
| Kernel jitter | ~10-100 µs | Boot with `isolcpus=N` kernel parameter |
| Signal handler writes | ~1 µs | Replace `std::atomic<bool>` with `sig_atomic_t` |
| Cache misses | variable | Layout order book by ticker; NUMA-aware |

## Project structure

```
├── CMakeLists.txt
├── LICENSE
├── .gitignore
├── README.md
├── run_benchmarks.sh              # Automated benchmark suite
├── include/
│   └── market_data_parser.h      # Protocol definitions (aligned struct)
└── src/
    ├── market_data_consumer.h     # Consumer class header (OrderEntry alignas(64))
    ├── market_data_consumer.cpp   # Consumer implementation
    ├── main.cpp                   # Entry point, CLI, benchmark summary
    └── mock_feed_generator.cpp    # Synthetic data broadcaster
```

## License

MIT
