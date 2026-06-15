#include "market_data_parser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>

static inline uint64_t nanos() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000 + ts.tv_nsec;
}

int main(int argc, char* argv[]) {
    int port = 20001;
    const char* ip = "127.0.0.1";
    int interval_us = 1000;
    bool inject_gap = false;
    int warmup = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            interval_us = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--inject-gap") == 0) {
            inject_gap = true;
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::stoi(argv[++i]);
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dest.sin_addr) <= 0) {
        perror("inet_pton"); close(sock); return 1;
    }

    std::srand(std::time(nullptr));

    MDPMarketUpdate update{};
    uint32_t seq = 0;

    std::cout << "Broadcasting to " << ip << ":" << port
              << " every " << interval_us << " us"
              << (inject_gap ? " (injecting gaps)" : "")
              << (warmup > 0 ? " [warmup " + std::to_string(warmup) + "]" : "")
              << "\n";

    for (int i = 0; i < warmup; i++) {
        update.timestamp = nanos();
        update.sequence_num = ++seq;
        update.ticker_id = 1001 + (std::rand() % 10);
        update.order_id = static_cast<uint32_t>(std::rand());
        update.price = 10000 + (std::rand() % 50000);
        update.quantity = 100 + (std::rand() % 10000);
        update.type = static_cast<MarketUpdateType>((std::rand() % 5) + 1);
        sendto(sock, &update, sizeof(update), 0, (struct sockaddr*)&dest, sizeof(dest));
    }

    while (true) {
        update.timestamp = nanos();
        update.sequence_num = ++seq;
        update.ticker_id = 1001 + (std::rand() % 10);
        update.order_id = static_cast<uint32_t>(std::rand());
        update.price = 10000 + (std::rand() % 50000);
        update.quantity = 100 + (std::rand() % 10000);
        update.type = static_cast<MarketUpdateType>((std::rand() % 5) + 1);

        ssize_t sent = sendto(sock, &update, sizeof(update), 0,
                              (struct sockaddr*)&dest, sizeof(dest));
        if (sent < 0) perror("sendto");

        if (inject_gap && seq % 1000 == 0) seq++;

        if (interval_us > 0) usleep(interval_us);
    }

    close(sock);
    return 0;
}
