#include "market_data_consumer.h"
#include <csignal>
#include <cstring>
#include <iostream>

static MarketDataConsumer* g_consumer = nullptr;

void signal_handler(int) {
    if (g_consumer) g_consumer->stop();
}

int main(int argc, char* argv[]) {
    bool benchmark = false;
    bool use_batch = false;
    bool use_multicast = false;
    int core_id = -1;
    int port = 20001;
    int warmup = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
        } else if (strcmp(argv[i], "--batch") == 0) {
            use_batch = true;
        } else if (strcmp(argv[i], "--multicast") == 0) {
            use_multicast = true;
        } else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
            core_id = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::stoi(argv[++i]);
        }
    }

    MarketDataConsumer consumer;
    g_consumer = &consumer;
    signal(SIGINT, signal_handler);

    const char* bind_ip = use_multicast ? "224.0.0.1" : "127.0.0.1";
    if (!consumer.init(bind_ip, port, use_multicast)) {
        std::cerr << "Failed to initialize consumer on port " << port << "\n";
        return 1;
    }

    if (core_id >= 0) consumer.set_cpu_affinity(core_id);

    std::cout << "Consumer listening on port " << port
              << (benchmark ? " (benchmark)" : "")
              << (use_batch ? " [batch]" : "")
              << (use_multicast ? " [multicast]" : "")
              << (core_id >= 0 ? " [core " + std::to_string(core_id) + "]" : "")
              << (warmup > 0 ? " [warmup " + std::to_string(warmup) + "]" : "")
              << "\n";

    consumer.start_polling(benchmark, warmup, use_batch);

    std::cout << "\nSummary:\n";
    std::cout << "  Packets processed: " << consumer.packets_processed() << "\n";
    std::cout << "  Sequence gaps:     " << consumer.sequence_gaps() << "\n";

    if (benchmark && consumer.bench_count() > 0) {
        double cpu_mhz = 0.0;
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                double mhz;
                if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) { cpu_mhz = mhz; break; }
            }
            fclose(f);
        }
        double ns_per_cycle = cpu_mhz > 0 ? (1000.0 / cpu_mhz) : 0.333;

        uint64_t cnt = consumer.bench_count();
        uint64_t clear_cnt = consumer.bench_clear_count();
        const uint64_t* hist = consumer.bench_hist();
        const char* bin_labels[] = {
            "<100c", "100-200c", "200-500c", "500-1kc",
            "1k-10kc", "10k-100kc", "100k-1Mc", ">=1Mc"
        };

        std::cout << "  Benchmark samples: " << cnt << "\n";
        std::cout << "  CLEAR operations:  " << clear_cnt << " ("
                  << (100.0 * clear_cnt / cnt) << "%) "
                  << "(avg " << (clear_cnt > 0 ? consumer.bench_clear_cycles() / clear_cnt : 0)
                  << " cycles/clear)\n";
        std::cout << "\n  Latency histogram (cycles):\n";
        for (int i = 0; i < 8; i++) {
            double pct = 100.0 * hist[i] / cnt;
            std::cout << "    " << bin_labels[i] << ": "
                      << hist[i] << " (" << pct << "%)";
            if (i >= 5 && hist[i] > 0 && hist[i] > cnt / 1000) std::cout << "  <-- heavy tail";
            std::cout << "\n";
        }

        consumer.finalize_bench();

        std::cout << "\n  Latency (cycles):\n";
        std::cout << "    Min:     " << consumer.bench_min_cycles()
                  << " (" << (consumer.bench_min_cycles() * ns_per_cycle) << " ns)\n";
        std::cout << "    p50:     " << consumer.bench_p50()
                  << " (" << (consumer.bench_p50() * ns_per_cycle) << " ns)\n";
        std::cout << "    p95:     " << consumer.bench_p95()
                  << " (" << (consumer.bench_p95() * ns_per_cycle) << " ns)\n";
        std::cout << "    p99:     " << consumer.bench_p99()
                  << " (" << (consumer.bench_p99() * ns_per_cycle) << " ns)\n";
        std::cout << "    p99.9:   " << consumer.bench_p99_9()
                  << " (" << (consumer.bench_p99_9() * ns_per_cycle) << " ns)\n";
        std::cout << "    Max:     " << consumer.bench_max_cycles()
                  << " (" << (consumer.bench_max_cycles() * ns_per_cycle) << " ns)\n";
        std::cout << "  Avg (all packets):   " << consumer.bench_avg_cycles()
                  << " cycles\n";
        std::cout << "  Avg (non-CLEAR):     " << consumer.bench_non_clear_avg()
                  << " cycles\n";
    }

    return 0;
}
