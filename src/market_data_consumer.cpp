#include "market_data_consumer.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <x86intrin.h>

static inline uint64_t rdtscp() {
    unsigned int lo, hi, aux;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

OrderEntry MarketDataConsumer::order_book_[MAX_ORDERS];

MarketDataConsumer::MarketDataConsumer() {
    volatile auto* page = reinterpret_cast<volatile uint8_t*>(order_book_);
    for (size_t i = 0; i < sizeof(order_book_); i += 4096) {
        page[i] = 0;
    }
    for (size_t i = 0; i < BATCH_SIZE; i++) {
        batch_iov_[i].iov_base = batch_bufs_[i];
        batch_iov_[i].iov_len = sizeof(batch_bufs_[i]);
        batch_msg_[i].msg_hdr.msg_iov = &batch_iov_[i];
        batch_msg_[i].msg_hdr.msg_iovlen = 1;
    }
}

MarketDataConsumer::~MarketDataConsumer() {
    stop();
    if (socket_fd_ >= 0) close(socket_fd_);
}

bool MarketDataConsumer::init(const std::string& ip, int port, bool use_multicast) {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd_ < 0) { perror("socket"); return false; }

    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) { perror("fcntl"); return false; }

    int optval = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(socket_fd_); socket_fd_ = -1; return false;
    }

    if (use_multicast) {
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(ip.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            perror("IP_ADD_MEMBERSHIP");
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        use_multicast_ = true;
    }
    return true;
}

void MarketDataConsumer::set_cpu_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void MarketDataConsumer::stop() { running_ = false; }

void MarketDataConsumer::start_polling(bool benchmark, int warmup_packets, bool use_batch) {
    if (use_batch) { poll_batch(benchmark, warmup_packets); return; }

    running_ = true;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    int warmup_remaining = warmup_packets;

    while (running_) {
        ssize_t bytes = recvfrom(socket_fd_, rx_buffer_, sizeof(rx_buffer_), 0,
                                 (struct sockaddr*)&from_addr, &from_len);
        if (bytes <= 0) continue;

        if (warmup_remaining > 0) {
            process_packet(rx_buffer_, bytes, false);
            warmup_remaining--;
            continue;
        }

        uint64_t start = benchmark ? rdtscp() : 0;
        process_packet(rx_buffer_, bytes, benchmark);
        if (benchmark) {
            uint64_t cycles = rdtscp() - start;
            bench_samples_.push_back(cycles);
            if (cycles < bench_min_) bench_min_ = cycles;
            if (cycles > bench_max_) bench_max_ = cycles;
            bench_total_ += cycles;
            bench_count_++;
            size_t bin = cycles < 100 ? 0
                      : cycles < 200 ? 1
                      : cycles < 500 ? 2
                      : cycles < 1000 ? 3
                      : cycles < 10000 ? 4
                      : cycles < 100000 ? 5
                      : cycles < 1000000 ? 6
                      : 7;
            if (bin < HIST_BINS) bench_hist_[bin]++;
        }
    }
}

void MarketDataConsumer::poll_batch(bool benchmark, int warmup_packets) {
    running_ = true;
    int warmup_remaining = warmup_packets;

    while (running_) {
        int ret = recvmmsg(socket_fd_, batch_msg_, BATCH_SIZE, 0, nullptr);
        if (ret <= 0) continue;

        for (int i = 0; i < ret; i++) {
            const char* buf = static_cast<const char*>(
                batch_msg_[i].msg_hdr.msg_iov->iov_base);
            ssize_t len = batch_msg_[i].msg_len;

            if (warmup_remaining > 0) {
                process_packet(buf, len, false);
                warmup_remaining--;
                continue;
            }

            uint64_t start = benchmark ? rdtscp() : 0;
            process_packet(buf, len, benchmark);
            if (benchmark) {
                uint64_t cycles = rdtscp() - start;
                bench_samples_.push_back(cycles);
                if (cycles < bench_min_) bench_min_ = cycles;
                if (cycles > bench_max_) bench_max_ = cycles;
                bench_total_ += cycles;
                bench_count_++;
                size_t bin = cycles < 100 ? 0
                          : cycles < 200 ? 1
                          : cycles < 500 ? 2
                          : cycles < 1000 ? 3
                          : cycles < 10000 ? 4
                          : cycles < 100000 ? 5
                          : cycles < 1000000 ? 6
                          : 7;
                if (bin < HIST_BINS) bench_hist_[bin]++;
            }
        }
    }
}

void MarketDataConsumer::process_packet(const char* buffer, ssize_t length, bool benchmark) {
    if (length < static_cast<ssize_t>(sizeof(MDPMarketUpdate))) return;

    const auto* update = reinterpret_cast<const MDPMarketUpdate*>(buffer);

    if (has_last_sequence_) {
        uint32_t expected = last_sequence_num_ + 1;
        if (update->sequence_num != expected) {
            sequence_gaps_++;
            if (!benchmark) {
                std::cerr << "Sequence gap: expected " << expected
                          << ", got " << update->sequence_num << "\n";
            }
        }
    }
    last_sequence_num_ = update->sequence_num;
    has_last_sequence_ = true;
    packets_processed_++;

    switch (update->type) {
        case MarketUpdateType::ADD_ORDER: {
            size_t idx = update->order_id % MAX_ORDERS;
            order_book_[idx].last_update_ts = update->timestamp;
            order_book_[idx].order_id = update->order_id;
            order_book_[idx].ticker_id = update->ticker_id;
            order_book_[idx].price = update->price;
            order_book_[idx].quantity = update->quantity;
            order_book_[idx].epoch = current_epoch_;
            order_book_[idx].active = true;
            break;
        }
        case MarketUpdateType::CANCEL_ORDER: {
            size_t idx = update->order_id % MAX_ORDERS;
            if (order_book_[idx].epoch == current_epoch_ &&
                order_book_[idx].order_id == update->order_id) {
                order_book_[idx].active = false;
            }
            break;
        }
        case MarketUpdateType::MODIFY_ORDER: {
            size_t idx = update->order_id % MAX_ORDERS;
            if (order_book_[idx].epoch == current_epoch_ &&
                order_book_[idx].order_id == update->order_id) {
                order_book_[idx].price = update->price;
                order_book_[idx].quantity = update->quantity;
            }
            break;
        }
        case MarketUpdateType::TRADE:
            break;
        case MarketUpdateType::CLEAR:
        {
            uint64_t cs = benchmark ? rdtscp() : 0;
            current_epoch_++;
            if (benchmark) {
                uint64_t ce = rdtscp() - cs;
                bench_clear_count_++;
                bench_clear_cycles_ += ce;
            }
            break;
        }
    }

    if (!benchmark && update->type == MarketUpdateType::TRADE) {
        std::cout << "TRADE: ticker=" << update->ticker_id
                  << " qty=" << update->quantity
                  << " price=" << update->price << "\n";
    }
}

uint64_t MarketDataConsumer::bench_percentile(double p) const {
    if (bench_samples_.empty()) return 0;
    if (!bench_sorted_) {
        std::sort(bench_samples_.begin(), bench_samples_.end());
        bench_sorted_ = true;
    }
    size_t idx = std::min(
        static_cast<size_t>(p / 100.0 * bench_samples_.size()),
        bench_samples_.size() - 1
    );
    return bench_samples_[idx];
}

void MarketDataConsumer::finalize_bench() const {
    bench_percentile(50.0);
}
