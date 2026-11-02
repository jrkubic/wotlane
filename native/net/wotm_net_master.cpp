// ABOUTME: Master server implementation: keeps the servers that announce themselves, answers SERVERS with the live ones and drops
// ABOUTME: those silent for longer than expireMs. No game code; runs in wotm_server --master and in the tests.
#include "wotm_net_master.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace wotm_net
{
    namespace
    {
        // a whole non-negative decimal number in [0, max]
        bool number(const std::string &s, int max, int &out)
        {
            if (s.empty() || s.size() > 9 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return false;
            out = std::atoi(s.c_str());
            return out <= max;
        }
    }

    bool Master::start(std::string &err)
    {
        if (!listener_.listen(cfg_.port))
        {
            err = "could not listen on TCP port " + std::to_string(cfg_.port);
            return false;
        }
        log("master listening on TCP port %u; servers leave the list %d s after their last announce", cfg_.port, cfg_.expireMs / 1000);
        return true;
    }

    void Master::log(const char *fmt, ...)
    {
        if (!cfg_.log)
            return;
        va_list ap;
        va_start(ap, fmt);
        std::printf("[master] ");
        std::vprintf(fmt, ap);
        std::printf("\n");
        std::fflush(stdout);
        va_end(ap);
    }

    void Master::poll(int ms)
    {
        bool busy = false;
        LineSocket s;
        std::string ip;
        while (listener_.accept(s, &ip))
        {
            auto c = std::make_unique<Conn>();
            c->socket = std::move(s);
            c->ip = ip;
            conns_.push_back(std::move(c));
            busy = true;
        }
        for (auto &c : conns_)
        {
            const bool alive = c->socket.pump();
            std::string line;
            while (c->socket.open() && c->socket.nextLine(line))
            {
                handleLine(*c, line);
                busy = true;
            }
            if (!alive)
                c->socket.close();
        }
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const std::unique_ptr<Conn> &c) { return !c->socket.open(); }),
                     conns_.end());
        expire();
        if (!busy && ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void Master::handleLine(Conn &c, const std::string &line)
    {
        const std::vector<std::string> w = proto::words(line);
        if (!w.empty() && w[0] == "ANNOUNCE")
            announce(c, w);
        else if (!w.empty() && w[0] == "SERVERS")
        {
            expire();
            for (const Entry &e : servers_)
                c.socket.sendLine("SERVER " + e.ip + " " + std::to_string(e.port) + " " + std::to_string(e.protocol) + " " +
                                  std::to_string(e.open) + " " + std::to_string(e.waiting) + " " + std::to_string(e.players) + " " +
                                  std::to_string(e.maxLobbies) + " " + e.name);
            c.socket.sendLine("END");
        }
        else
            c.socket.sendLine("ERROR unknown command (ANNOUNCE or SERVERS)");
    }

    void Master::announce(Conn &c, const std::vector<std::string> &w)
    {
        Entry e;
        e.ip = c.ip;
        const bool ok = w.size() == 8 && number(w[1], 65535, e.protocol) && number(w[2], 65535, e.port) && e.port > 0 &&
                        number(w[3], 1000, e.open) && number(w[4], 1000, e.waiting) && number(w[5], 100000, e.players) &&
                        number(w[6], 1000, e.maxLobbies) && !w[7].empty() && proto::cleanName(w[7]) == w[7];
        if (!ok)
        {
            c.socket.sendLine("ERROR ANNOUNCE <protocol> <port> <open> <waiting> <players> <max lobbies> <name>");
            return;
        }
        e.name = w[7];
        e.last = Clock::now();
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const Entry &x) { return x.ip == e.ip && x.port == e.port; });
        if (it == servers_.end())
        {
            log("server %s (%s:%d) announced", e.name.c_str(), e.ip.c_str(), e.port);
            servers_.push_back(e);
        }
        else
            *it = e;
        c.socket.sendLine("OK");
    }

    void Master::expire()
    {
        const auto cutoff = Clock::now() - std::chrono::milliseconds(cfg_.expireMs);
        for (const Entry &e : servers_)
            if (e.last < cutoff)
                log("server %s (%s:%d) left the list", e.name.c_str(), e.ip.c_str(), e.port);
        servers_.erase(std::remove_if(servers_.begin(), servers_.end(), [&](const Entry &e) { return e.last < cutoff; }), servers_.end());
    }
}
