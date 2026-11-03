// ABOUTME: Master server: the list of public wotm_servers. Game servers announce themselves every few seconds (ANNOUNCE) and drop
// ABOUTME: off when they stop; clients ask for the list (SERVERS). Text lines over TCP, like the lobby. Used by wotm_server --master.
#pragma once

#include "wotm_net_proto.hpp"
#include "wotm_net_socket.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wotm_net
{
    // Protocol (one line each way per request, the connection closes after it):
    //   ANNOUNCE <protocol> <port> <open lobbies> <waiting lobbies> <players> <max lobbies> <name>   -> OK | ERROR <why>
    //   SERVERS -> SERVER <ip> <port> <protocol> <open> <waiting> <players> <max lobbies> <name> ... END
    // <ip> is the address the master saw the announce come from (behind a router, the public one); <port> is the one the server
    // announced (where its lobby and match listen). Servers of another protocol stay listed, so a client can say they need another version.
    struct MasterConfig
    {
        uint16_t port = proto::kDefaultMasterPort;
        int expireMs = 30000;   // a server not heard from for this long leaves the list (wotm_server announces every 10 s)
        bool log = true;
    };

    class Master
    {
    public:
        explicit Master(MasterConfig c) : cfg_(c) {}
        Master(const Master &) = delete;
        Master &operator=(const Master &) = delete;

        bool start(std::string &err);
        // accepts and answers connections, forgets silent servers; waits up to `ms` when idle
        void poll(int ms);

    private:
        using Clock = std::chrono::steady_clock;

        struct Conn
        {
            LineSocket socket;
            std::string ip;
        };

        struct Entry
        {
            std::string ip, name;
            int port = 0, protocol = 0, open = 0, waiting = 0, players = 0, maxLobbies = 0;
            Clock::time_point last{};
        };

        void log(const char *fmt, ...);
        void handleLine(Conn &c, const std::string &line);
        void announce(Conn &c, const std::vector<std::string> &w);
        void expire();

        MasterConfig cfg_;
        Listener listener_;
        std::vector<std::unique_ptr<Conn>> conns_;
        std::vector<Entry> servers_;   // in the order they first announced
    };
}
