#pragma once

#include "market_data_parser.h"
#include <string>
#include <atomic>
#include <cstdint>
#include <sys/socket.h>

static constexpr size_t MAX_ORDERS = 1'000'000;

struct OrderEntry {
    uint32_t order_id;
    uint32_t ticker_id;
    uint32_t price;
    uint32_t quantity;
    bool active;
};

class MarketDataConsumer {
public:
    MarketDataConsumer();
    ~MarketDataConsumer();

    MarketDataConsumer(const MarketDataConsumer&) = delete;
    MarketDataConsumer& operator=(const MarketDataConsumer&) = delete;

    bool init(const std::string& ip, int port);
    void start_polling(bool benchmark, int warmup_packets = 0, bool use_batch = false);
    void stop();
    void set_cpu_affinity(int core_id);

    uint64_t packets_processed() const { return packets_processed_.load(); }
    uint64_t sequence_gaps() const { return sequence_gaps_.load(); }

    uint64_t bench_min_cycles() const { return bench_min_; }
    uint64_t bench_max_cycles() const { return bench_max_; }
    uint64_t bench_avg_cycles() const { return bench_count_ > 0 ? bench_total_ / bench_count_ : 0; }
    uint64_t bench_count() const { return bench_count_; }
    uint64_t bench_clear_count() const { return bench_clear_count_; }
    uint64_t bench_clear_cycles() const { return bench_clear_cycles_; }
    uint64_t bench_non_clear_avg() const {
        uint64_t non_clear = bench_count_ - bench_clear_count_;
        if (non_clear == 0) return 0;
        return (bench_total_ - bench_clear_cycles_) / non_clear;
    }
    const uint64_t* bench_hist() const { return bench_hist_; }

private:
    void process_packet(const char* buffer, ssize_t length, bool benchmark);
    void poll_batch(bool benchmark, int warmup_packets);

    int socket_fd_ = -1;
    alignas(alignof(MDPMarketUpdate)) char rx_buffer_[65536];
    bool running_ = false;

    uint32_t last_sequence_num_ = 0;
    bool has_last_sequence_ = false;
    std::atomic<uint64_t> packets_processed_{0};
    std::atomic<uint64_t> sequence_gaps_{0};

    static OrderEntry order_book_[MAX_ORDERS];

    uint64_t bench_min_ = UINT64_MAX;
    uint64_t bench_max_ = 0;
    uint64_t bench_total_ = 0;
    uint64_t bench_count_ = 0;
    uint64_t bench_clear_count_ = 0;
    uint64_t bench_clear_cycles_ = 0;
    static constexpr size_t HIST_BINS = 8;
    uint64_t bench_hist_[HIST_BINS] = {};

    static constexpr size_t BATCH_SIZE = 64;
    struct iovec batch_iov_[BATCH_SIZE];
    struct mmsghdr batch_msg_[BATCH_SIZE];
    alignas(alignof(MDPMarketUpdate)) char batch_bufs_[BATCH_SIZE][65536];
};
