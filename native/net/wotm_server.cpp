// ABOUTME: wotm_server: dedicated server for wotlane netcode (player-created lobbies with a code and optional password, lockstep matches),
// ABOUTME: no game code; or, with --master, the master server that lists the public ones. Forward the ports in the router (see usage).
#include "wotm_net_master.hpp"
#include "wotm_net_server.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    std::atomic<bool> g_stop{false};
    void onSignal(int) { g_stop = true; }

    int usage()
    {
        std::printf("usage: wotm_server [--port 7777] [--lobbies 16] [--players 8] [--delay <frames>] [--announce <host>[:7778] [--name <name>]]\n"
                    "       wotm_server --master [--port 7778]\n"
                    "  --port      TCP (lobby) and UDP (match) port; forward both in the router to play over the internet\n"
                    "              (with --master: the TCP port clients and servers reach the master on)\n"
                    "  --lobbies   how many lobbies can be open at the same time (default 16)\n"
                    "  --players   players per lobby (2 to 8; default 8)\n"
                    "  --delay     fixed input delay in frames (default: from each match's ping)\n"
                    "  --announce  list this server publicly on that master (every 10 s; players then see it with net_lobby.py --browse)\n"
                    "  --name      the name in the public list (letters, digits, - and _; up to 16; default wotlane)\n"
                    "  --master    run the master server instead: the list of servers that announce themselves\n");
        return 2;
    }
}

int main(int argc, char **argv)
{
    wotm_net::ServerConfig cfg;
    bool master = false, portGiven = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--port" && hasValue)
        {
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
            portGiven = true;
        }
        else if (a == "--lobbies" && hasValue)
            cfg.lobbies = std::atoi(argv[++i]);
        else if (a == "--players" && hasValue)
            cfg.maxPlayers = std::atoi(argv[++i]);
        else if (a == "--delay" && hasValue)
            cfg.forcedDelay = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--announce" && hasValue)
        {
            const std::string v = argv[++i];   // host or host:port
            const size_t colon = v.rfind(':');
            cfg.announceHost = v.substr(0, colon);
            if (colon != std::string::npos)
                cfg.announcePort = static_cast<uint16_t>(std::atoi(v.c_str() + colon + 1));
        }
        else if (a == "--name" && hasValue)
        {
            cfg.name = wotm_net::proto::cleanName(argv[++i]);
            if (cfg.name.empty())
                return usage();
        }
        else if (a == "--master")
            master = true;
        else
            return usage();
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::string err;
    if (master)
    {
        wotm_net::MasterConfig mc;
        if (portGiven)
            mc.port = cfg.port;
        wotm_net::Master m(mc);
        if (!m.start(err))
        {
            std::fprintf(stderr, "wotm_server: %s\n", err.c_str());
            return 1;
        }
        while (!g_stop)
            m.poll(5);
        std::printf("[master] shut down\n");
        return 0;
    }
    wotm_net::Server server(cfg);
    if (!server.start(err))
    {
        std::fprintf(stderr, "wotm_server: %s\n", err.c_str());
        return 1;
    }
    while (!g_stop)
        server.poll(5);
    std::printf("[server] shut down\n");
    return 0;
}
