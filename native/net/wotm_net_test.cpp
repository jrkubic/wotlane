// ABOUTME: Netcode tests without the game: the input timeline (lockstep), message encoding, and a real ENet server and client
// ABOUTME: on localhost. Exits with 0 if everything passes; each failure prints the file, line and expression.
#include "wotm_lockstep.hpp"
#include "wotm_monsters.hpp"
#include "wotm_net_client.hpp"
#include "wotm_net_master.hpp"
#include "wotm_net_proto.hpp"
#include "wotm_net_server.hpp"
#include "wotm_net_socket.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <functional>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0, g_checks = 0;

#define CHECK(expr)                                                                                                                  \
    do                                                                                                                               \
    {                                                                                                                                \
        ++g_checks;                                                                                                                  \
        if (!(expr))                                                                                                                 \
        {                                                                                                                            \
            ++g_failures;                                                                                                            \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                          \
        }                                                                                                                            \
    } while (0)

    struct Test
    {
        const char *name;
        std::function<void()> run;
    };
    std::vector<Test> &tests()
    {
        static std::vector<Test> t;
        return t;
    }
    struct Register
    {
        Register(const char *name, std::function<void()> run) { tests().push_back({name, std::move(run)}); }
    };
#define TEST(name)                                                                                                                   \
    void name();                                                                                                                     \
    Register reg_##name(#name, name);                                                                                                \
    void name()

    using wotm_net::InputTimeline;
    using wotm_net::PadState;

    PadState pressed(uint16_t mask)
    {
        PadState s;
        s.buttons = static_cast<uint16_t>(0xFFFFu & ~mask);
        return s;
    }

    TEST(timeline_first_frames_are_neutral_for_the_delay)
    {
        InputTimeline t(2, 3);
        CHECK(t.ready(0) && t.ready(1) && t.ready(2));
        CHECK(!t.ready(3));
        CHECK(t.get(0, 2) == PadState{});
        CHECK(t.nextMissing(0) == 3 && t.nextMissing(1) == 3);
    }

    TEST(timeline_frame_is_ready_only_with_every_player)
    {
        InputTimeline t(3, 2);
        t.set(0, 2, pressed(0x4000));
        t.set(2, 2, pressed(0x0010));
        CHECK(!t.ready(2));
        t.set(1, 2, PadState{});
        CHECK(t.ready(2));
        CHECK(t.get(0, 2) == pressed(0x4000));
        CHECK(t.get(2, 2) == pressed(0x0010));
    }

    TEST(timeline_next_missing_is_the_first_gap)
    {
        InputTimeline t(1, 1);
        t.set(0, 1, PadState{});
        t.set(0, 3, PadState{});
        CHECK(t.nextMissing(0) == 2);
        t.set(0, 2, PadState{});
        CHECK(t.nextMissing(0) == 4);
    }

    TEST(timeline_keeps_the_first_value_of_a_frame)
    {
        InputTimeline t(1, 0);
        t.set(0, 0, pressed(0x4000));
        t.set(0, 0, pressed(0x8000));   // a repeat (redundant packet) never replaces what already arrived
        CHECK(t.get(0, 0) == pressed(0x4000));
    }

    TEST(timeline_discard_drops_old_frames_but_keeps_progress)
    {
        InputTimeline t(1, 0);
        for (uint32_t f = 0; f < 10; ++f)
            t.set(0, f, pressed(static_cast<uint16_t>(f)));
        t.discardBefore(8);
        CHECK(t.nextMissing(0) == 10);
        CHECK(t.ready(9) && t.get(0, 9) == pressed(9));
        t.set(0, 3, pressed(0x1234));   // frame already discarded: ignored
        CHECK(t.nextMissing(0) == 10);
    }

    TEST(timeline_dropped_player_is_neutral_from_the_drop_frame)
    {
        InputTimeline t(2, 1);
        t.set(0, 1, pressed(1));
        t.set(1, 1, pressed(2));
        t.drop(1, 2);
        t.set(0, 2, pressed(3));
        CHECK(t.ready(2) && t.ready(500) == false);
        CHECK(t.get(1, 2) == PadState{});
        CHECK(t.get(1, 1) == pressed(2));
        t.set(1, 5, pressed(7));   // late input after the drop: ignored
        CHECK(t.get(1, 5) == PadState{});
    }

    TEST(local_delay_schedules_sampled_input_ahead)
    {
        InputTimeline t(2, 3);
        CHECK(t.scheduleLocal(1, 0, pressed(0x4000)) == 3u);   // the sample from frame 0 applies at frame 0 + delay
        CHECK(t.has(1, 3) && t.get(1, 3) == pressed(0x4000));
        CHECK(!t.has(0, 3));
    }

    // ---- protocol ----
    namespace p = wotm_net::proto;

    TEST(proto_join_roundtrip)
    {
        p::Join j;
        j.token = "a1b2c3";
        j.buildId = "build-xyz";
        j.configHash = "cfg-1";
        j.name = "Lane";
        const std::vector<uint8_t> bytes = p::encode(j);
        CHECK(p::typeOf(bytes) == p::Type::Join);
        p::Join back;
        CHECK(p::decode(bytes, back));
        CHECK(back.protocol == p::kProtocol && back.token == j.token && back.buildId == j.buildId && back.configHash == j.configHash &&
              back.name == j.name);
    }

    TEST(proto_inputs_roundtrip_with_repeated_states)
    {
        p::Inputs m;
        m.ackOwn = 41;
        p::InputRun r;
        r.player = 2;
        r.first = 37;
        for (int i = 0; i < 20; ++i)
            r.states.push_back(i < 10 ? pressed(0x4000) : PadState{});
        m.runs.push_back(r);
        const std::vector<uint8_t> bytes = p::encode(m);
        CHECK(bytes.size() == 6u + 6u + 20u + 2u * 6u);   // headers, 1 byte per state, 6 per new state (only 2 change)
        p::Inputs back;
        CHECK(p::decode(bytes, back));
        CHECK(back.ackOwn == 41u && back.runs.size() == 1u);
        CHECK(back.runs[0].player == 2 && back.runs[0].first == 37u && back.runs[0].states == r.states);
    }

    TEST(proto_input_roundtrip)
    {
        p::Input m;
        m.acks = {5, 9, 0, 12};
        m.own.player = 1;
        m.own.first = 9;
        m.own.states = {pressed(1), pressed(2), PadState{}};
        p::Input back;
        CHECK(p::decode(p::encode(m), back));
        CHECK(back.acks == m.acks && back.own.player == 1 && back.own.first == 9u && back.own.states == m.own.states);
    }

    TEST(proto_control_messages_roundtrip)
    {
        p::Welcome w{3, 4};
        p::Welcome wb;
        CHECK(p::decode(p::encode(w), wb) && wb.player == 3 && wb.players == 4);
        p::Start s;
        s.delay = 4;
        s.names = {"a", "b"};
        p::Start sb;
        CHECK(p::decode(p::encode(s), sb) && sb.delay == 4u && sb.names == s.names);
        p::Hash h{120, 0x1122334455667788ull};
        p::Hash hb;
        CHECK(p::decode(p::encode(h), hb) && hb.frame == 120u && hb.hash == h.hash);
        p::Drop d{1, 77};
        p::Drop db;
        CHECK(p::decode(p::encode(d), db) && db.player == 1 && db.frame == 77u);
        p::Desync x{90, "player 2 differs"};
        p::Desync xb;
        CHECK(p::decode(p::encode(x), xb) && xb.frame == 90u && xb.detail == x.detail);
        p::Reject r{"different version"};
        p::Reject rb;
        CHECK(p::decode(p::encode(r), rb) && rb.reason == r.reason);
    }

    TEST(proto_truncated_message_is_rejected)
    {
        p::Inputs m;
        p::InputRun r;
        r.states = {pressed(3), pressed(4)};
        m.runs.push_back(r);
        std::vector<uint8_t> bytes = p::encode(m);
        bytes.resize(bytes.size() - 2);
        p::Inputs back;
        CHECK(!p::decode(bytes, back));
        p::Join j;
        CHECK(!p::decode(bytes, j));   // wrong type
    }

    TEST(proto_delay_covers_the_relay_path)
    {
        // delay = one-way trip slowest player -> server plus server -> second slowest, in frames, plus 1 frame of slack
        CHECK(p::pickDelay({10, 10}, 16) == 2u);
        CHECK(p::pickDelay({100, 60}, 16) == 6u);    // (100 + 60) / 2 = 80 ms = 5 frames + 1
        CHECK(p::pickDelay({60, 100, 20}, 33) == 4u);   // 4 players, 33 ms frame
        CHECK(p::pickDelay({2000, 2000}, 16) == 15u);   // ceiling
        CHECK(p::pickDelay({0}, 16) == 2u);             // floor
    }

    TEST(lobby_lines_split_words_and_key_values)
    {
        const std::vector<std::string> w = p::words("JOIN 1 Lane 3");
        CHECK(w.size() == 4u && w[0] == "JOIN" && w[3] == "3");
        const std::map<std::string, std::string> kv = p::keyValues({"MATCH", "players=2", "seed=99", "monsters=3,1"});
        CHECK(kv.at("players") == "2" && kv.at("seed") == "99" && kv.at("monsters") == "3,1" && kv.count("MATCH") == 0u);
        CHECK(p::cleanName("La ne<>!_9-xyzxyzxyzxyzxyz") == "Lane_9-xyzxyzxyz");   // 16 characters at most
        CHECK(p::cleanName("   ").empty());
    }

    // ---- real server and client on localhost ----
    using wotm_net::ClientConfig;
    using wotm_net::LineSocket;
    using wotm_net::MatchClient;
    using wotm_net::Server;
    using wotm_net::ServerConfig;
    using Status = wotm_net::MatchClient::Status;

    uint16_t g_nextPort = 27770;

    // server running on a thread for the duration of the test
    struct RunningServer
    {
        Server server;
        std::atomic<bool> stop{false};
        std::thread thread;
        bool ok = false;
        explicit RunningServer(ServerConfig c) : server(withQuiet(c))
        {
            std::string err;
            ok = server.start(err);
            if (!ok)
                std::printf("  server failed to start: %s\n", err.c_str());
            thread = std::thread([this] {
                while (!stop)
                    server.poll(1);
            });
        }
        ~RunningServer()
        {
            stop = true;
            thread.join();
        }
        static ServerConfig withQuiet(ServerConfig c)
        {
            c.log = false;
            return c;
        }
    };

    // a port free on both TCP and UDP (an earlier test process that is still shutting down may be holding its ports)
    uint16_t freePort()
    {
        for (int tries = 0; tries < 200; ++tries)
        {
            const uint16_t port = g_nextPort++;
            wotm_net::Listener tcp;
            if (!tcp.listen(port))
                continue;
            const wotm_net::SocketHandle udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(port);
            const bool ok = udp != wotm_net::kNoSocket && bind(udp, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0;
            if (udp != wotm_net::kNoSocket)
                wotm_net::closeSocket(udp);
            if (ok)
                return port;
        }
        return g_nextPort++;
    }

    ServerConfig testConfig()
    {
        ServerConfig c;
        c.port = freePort();
        c.startWaitMs = 100;
        return c;
    }

    std::string readUntil(LineSocket &s, const std::string &prefix, int ms = 3000)
    {
        std::string line;
        while (s.readLine(line, ms))
            if (line.rfind(prefix, 0) == 0)
                return line;
        return {};
    }

    struct Member
    {
        LineSocket socket;
        std::map<std::string, std::string> match;
    };

    // connects and sends the HELLO for the current protocol; returns the WELCOME line
    std::string hello(LineSocket &s, uint16_t port, const std::string &build = "b1")
    {
        CHECK(s.connect("127.0.0.1", port, 2000));
        s.sendLine("HELLO " + std::to_string(p::kProtocol) + " " + build);
        return readUntil(s, "WELCOME");
    }

    // CREATE <lobby> <name> <monster> [password]: returns the lobby code (empty if the server refused)
    std::string createLobby(LineSocket &s, const std::string &lobby, const std::string &name, int monster, const std::string &password = "")
    {
        s.sendLine("CREATE " + lobby + " " + name + " " + std::to_string(monster) + (password.empty() ? "" : " " + password));
        const std::string created = readUntil(s, "CREATED");
        return created.size() > 8 ? created.substr(8) : std::string{};
    }

    // forms a match through the lobby: the first name creates the lobby (and is the host), the others join by code, and the host
    // picks the stage and the wins (SET) and starts it (START)
    std::vector<std::unique_ptr<Member>> formMatch(uint16_t port, const std::vector<std::string> &names, const std::vector<int> &monsters,
                                                   int stage = 1, const std::string &build = "b1", int wins = 0)
    {
        std::vector<std::unique_ptr<Member>> ms;
        std::string code;
        for (size_t i = 0; i < names.size(); ++i)
        {
            ms.push_back(std::make_unique<Member>());
            Member &m = *ms.back();
            CHECK(hello(m.socket, port, build).rfind("WELCOME", 0) == 0);
            if (i == 0)
            {
                code = createLobby(m.socket, "Lobby", names[i], monsters[i]);
                CHECK(code.size() == 4u);
                continue;
            }
            m.socket.sendLine("JOIN " + code + " " + names[i] + " " + std::to_string(monsters[i]));
            CHECK(readUntil(m.socket, "JOINED") == "JOINED " + code);
        }
        ms[0]->socket.sendLine("SET stage=" + std::to_string(stage) + " wins=" + std::to_string(wins));
        CHECK(readUntil(ms[0]->socket, "SETTINGS " + code + " stage=" + std::to_string(stage)).size() > 0u);
        ms[0]->socket.sendLine("START");
        for (auto &m : ms)
            m->match = p::keyValues(p::words(readUntil(m->socket, "MATCH")));
        return ms;
    }

    ClientConfig clientFor(uint16_t port, const Member &m, const std::string &config = "cfg")
    {
        ClientConfig c;
        c.host = "127.0.0.1";
        c.port = port;
        c.token = m.match.count("token") ? m.match.at("token") : "";
        c.buildId = "b1";
        c.configHash = config;
        c.name = "x";
        return c;
    }

    // whoever creates a lobby is its host: the server hands out a 4-letter code, and the list shows name, host, slots and whether it
    // has a password; the others join by code (upper or lower case); only the host changes the stage and wins (SETTINGS reaches
    // everyone) and starts; the MATCH line carries the code and the wins
    TEST(lobby_create_join_by_code_settings_and_start)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        CHECK(rs.ok);
        LineSocket watcher, a, b;
        CHECK(hello(watcher, cfg.port) == "WELCOME 0");
        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "") == "END");

        CHECK(hello(a, cfg.port) == "WELCOME 0");
        const std::string code = createLobby(a, "Lobby_1", "Ana", 3);
        CHECK(code.size() == 4u);
        CHECK(readUntil(a, "JOINED") == "JOINED " + code);
        CHECK(readUntil(a, "SETTINGS") == "SETTINGS " + code + " stage=1 wins=0");
        CHECK(readUntil(a, "MEMBERS") == "MEMBERS " + code + " Ana Ana:3");
        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "LOBBY") == "LOBBY " + code + " waiting 1/8 open Lobby_1 Ana Ana");
        CHECK(readUntil(watcher, "") == "END");

        CHECK(hello(b, cfg.port) == "WELCOME 1");
        std::string lower = code;
        for (char &ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        b.sendLine("JOIN " + lower + " Bia 1");
        CHECK(readUntil(b, "JOINED") == "JOINED " + code);
        CHECK(readUntil(b, "SETTINGS") == "SETTINGS " + code + " stage=1 wins=0");
        CHECK(readUntil(a, "MEMBERS " + code + " Ana Ana:3,") == "MEMBERS " + code + " Ana Ana:3,Bia:1");

        b.sendLine("SET wins=3");
        CHECK(readUntil(b, "ERROR").find("owner") != std::string::npos);
        b.sendLine("START");
        CHECK(readUntil(b, "ERROR").find("owner") != std::string::npos);
        a.sendLine("SET stage=5 wins=3");
        CHECK(readUntil(a, "SETTINGS") == "SETTINGS " + code + " stage=5 wins=3");
        CHECK(readUntil(b, "SETTINGS") == "SETTINGS " + code + " stage=5 wins=3");
        a.sendLine("SET wins=500");
        CHECK(readUntil(a, "SETTINGS") == "SETTINGS " + code + " stage=5 wins=99");

        a.sendLine("START");
        const auto ma = p::keyValues(p::words(readUntil(a, "MATCH"))), mb = p::keyValues(p::words(readUntil(b, "MATCH")));
        CHECK(ma.size() > 0u && mb.size() > 0u);
        if (ma.empty() || mb.empty())
            return;
        CHECK(ma.at("lobby") == code && ma.at("players") == "2" && mb.at("players") == "2");
        CHECK(ma.at("stage") == "5" && ma.at("wins") == "99" && mb.at("wins") == "99");
        CHECK(ma.at("monsters") == "3,1" && mb.at("monsters") == "3,1");
        CHECK(ma.at("seed") == mb.at("seed") && ma.at("seed") != "0");
        CHECK(ma.at("step") == "1" && ma.at("names") == "Ana,Bia");
        CHECK(ma.at("you") == "0" && mb.at("you") == "1");
        CHECK(ma.at("token") != mb.at("token") && ma.at("token").size() == 16u);

        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "LOBBY") == "LOBBY " + code + " playing 2/8 open Lobby_1 Ana Ana,Bia");
        LineSocket c;
        hello(c, cfg.port);
        c.sendLine("JOIN " + code + " Cai 2");
        CHECK(readUntil(c, "ERROR").find("match") != std::string::npos);
    }

    // password-protected lobby: shows as locked in the list and only someone sending the right password gets in; an unknown code is
    // refused
    TEST(lobby_password_and_unknown_code)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        LineSocket a, b, watcher;
        hello(a, cfg.port);
        hello(b, cfg.port);
        hello(watcher, cfg.port);
        const std::string code = createLobby(a, "Friends", "Ana", 3, "pw123");
        CHECK(code.size() == 4u);
        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "LOBBY") == "LOBBY " + code + " waiting 1/8 locked Friends Ana Ana");
        b.sendLine("JOIN " + code + " Bia 1");
        CHECK(readUntil(b, "ERROR").find("password") != std::string::npos);
        b.sendLine("JOIN " + code + " Bia 1 wrong");
        CHECK(readUntil(b, "ERROR").find("password") != std::string::npos);
        b.sendLine("JOIN " + code + " Bia 1 pw123");
        CHECK(readUntil(b, "JOINED") == "JOINED " + code);
        LineSocket c;
        hello(c, cfg.port);
        c.sendLine(code == "ZZZZ" ? "JOIN YYYY Cai 2" : "JOIN ZZZZ Cai 2");
        CHECK(readUntil(c, "ERROR").find("no such lobby") != std::string::npos);
    }

    // a leaving host (LEAVE or a dropped connection) passes the lobby to the next member; an empty lobby closes and leaves the list
    TEST(lobby_host_passes_on_and_empty_lobby_closes)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        LineSocket watcher, b;
        hello(watcher, cfg.port);
        std::string code;
        {
            LineSocket a;
            hello(a, cfg.port);
            code = createLobby(a, "Lobby", "Ana", 3);
            hello(b, cfg.port);
            b.sendLine("JOIN " + code + " Bia 1");
            CHECK(readUntil(b, "JOINED") == "JOINED " + code);
        }   // Ana's connection drops
        CHECK(readUntil(b, "MEMBERS " + code + " Bia") == "MEMBERS " + code + " Bia Bia:1");
        b.sendLine("SET wins=2");
        CHECK(readUntil(b, "SETTINGS") == "SETTINGS " + code + " stage=1 wins=2");
        b.sendLine("LEAVE");
        CHECK(readUntil(b, "LEFT") == "LEFT " + code);
        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "") == "END");
    }

    // members stay in the lobby during the match; once everyone leaves the match (QUIT closes the game), the lobby goes back to
    // waiting with the same members and the host can start another one
    TEST(lobby_members_stay_through_a_match_and_start_again)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1}, 2, "b1", 3);
        {
            MatchClient a, b;
            std::string err;
            CHECK(a.start(clientFor(cfg.port, *ms[0]), err) && b.start(clientFor(cfg.port, *ms[1]), err));
            CHECK(a.waitStarted(5000) && b.waitStarted(5000));
            a.stop();
            b.stop();
        }
        const std::string code = ms[0]->match.count("lobby") ? ms[0]->match.at("lobby") : "";
        CHECK(readUntil(ms[0]->socket, "MEMBERS " + code, 5000) == "MEMBERS " + code + " Ana Ana:3,Bia:1");
        LineSocket watcher;
        hello(watcher, cfg.port);
        watcher.sendLine("LIST");
        CHECK(readUntil(watcher, "LOBBY") == "LOBBY " + code + " waiting 2/8 open Lobby Ana Ana,Bia");
        ms[0]->socket.sendLine("START");
        const auto again = p::keyValues(p::words(readUntil(ms[1]->socket, "MATCH")));
        CHECK(again.count("wins") && again.at("wins") == "3" && again.at("stage") == "2");
        CHECK(again.count("token") && ms[1]->match.count("token") && again.at("token") != ms[1]->match.at("token"));
    }

    TEST(lobby_refuses_other_protocols_builds_and_full_lobbies)
    {
        ServerConfig cfg = testConfig();
        cfg.maxPlayers = 2;
        cfg.lobbies = 1;
        RunningServer rs(cfg);
        LineSocket old;
        CHECK(old.connect("127.0.0.1", cfg.port, 2000));
        old.sendLine("HELLO 1 b1");
        CHECK(readUntil(old, "ERROR").find("protocol") != std::string::npos);

        LineSocket a, b, c, d;
        hello(a, cfg.port);
        hello(b, cfg.port, "b2");
        hello(c, cfg.port);
        hello(d, cfg.port);
        const std::string code = createLobby(a, "Lobby", "Ana", 3);
        b.sendLine("JOIN " + code + " Bia 2");
        CHECK(readUntil(b, "ERROR").find("build") != std::string::npos);
        c.sendLine("JOIN " + code + " Cai 2");
        CHECK(readUntil(c, "JOINED") == "JOINED " + code);
        d.sendLine("JOIN " + code + " Dan 2");
        CHECK(readUntil(d, "ERROR").find("full") != std::string::npos);
        d.sendLine("CREATE Other Dan 2");
        CHECK(readUntil(d, "ERROR").find("lobbies") != std::string::npos);   // the server only has 1 lobby
    }

    // a monster with no file on the disc (spider 6, ogre 12, outside the table) or with no free costume (jelly has only 1) can't join
    // (or create a lobby): the game's load would hang on every PC
    TEST(lobby_refuses_monsters_the_disc_does_not_have)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        LineSocket a, b;
        hello(a, cfg.port);
        hello(b, cfg.port);
        for (const char *bad : {"6", "12", "0", "16"})
        {
            a.sendLine(std::string("CREATE Lobby Ana ") + bad);
            CHECK(readUntil(a, "ERROR").find("monster") != std::string::npos);
        }
        const std::string code = createLobby(a, "Lobby", "Ana", 14);
        CHECK(code.size() == 4u);
        for (const char *bad : {"6", "12"})
        {
            b.sendLine("JOIN " + code + " Bia " + bad);
            CHECK(readUntil(b, "ERROR").find("monster") != std::string::npos);
        }
        b.sendLine("JOIN " + code + " Bia 14");
        CHECK(readUntil(b, "ERROR").find("costume") != std::string::npos);
        b.sendLine("JOIN " + code + " Bia 3");
        CHECK(readUntil(b, "JOINED") == "JOINED " + code);
    }

    // costumes handed out the way userintMainDirect does: requested ones first, then the lowest free one (Goliath, 13, starts at 1:
    // costume 0 is a prototype)
    TEST(monster_list_fits_with_the_game_costume_rule)
    {
        int bad = -1;
        const int goliaths[] = {13, 13};
        CHECK(wotm_monsters::fits(goliaths, nullptr, 1));
        CHECK(!wotm_monsters::fits(goliaths, nullptr, 2, &bad));
        CHECK(bad == 1);
        const int goliathProto[] = {0, -1};
        CHECK(wotm_monsters::fits(goliaths, goliathProto, 2));   // the requested prototype plus the automatic 1
        const int jellies[] = {14, 14};
        CHECK(!wotm_monsters::fits(jellies, nullptr, 2));
        const int monkeys[] = {1, 1, 1, 1, 1};
        CHECK(wotm_monsters::fits(monkeys, nullptr, 5));   // the fifth repeats costume 0
        const int asked[] = {3, -1, -1};
        const int lizards[] = {3, 3, 3};
        CHECK(wotm_monsters::fits(lizards, asked, 3));   // 3 requested, the others get 0 and 1
        const int missing[] = {1, -1};
        const int jellyAsked[] = {14, 14};
        CHECK(!wotm_monsters::fits(jellyAsked, missing, 2, &bad));   // jelly only has costume 0
        CHECK(bad == 0);
        const int spider[] = {6};
        CHECK(!wotm_monsters::fits(spider, nullptr, 1));
    }

    TEST(lobby_refuses_a_second_goliath)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        LineSocket a, b;
        hello(a, cfg.port);
        hello(b, cfg.port);
        const std::string code = createLobby(a, "Lobby", "Ana", 13);
        CHECK(code.size() == 4u);
        b.sendLine("JOIN " + code + " Bia 13");
        CHECK(readUntil(b, "ERROR").find("costume") != std::string::npos);
    }

    // two match clients exchange frames of input and see the same timeline
    void exchange(MatchClient &a, MatchClient &b, uint32_t frames)
    {
        const uint32_t d = a.delay();
        for (uint32_t f = 0; f < frames; ++f)
        {
            a.pushLocal(f + d, pressed(static_cast<uint16_t>(f)));
            b.pushLocal(f + d, pressed(static_cast<uint16_t>(0x8000u | f)));
        }
        bool all = true;
        for (uint32_t f = 0; f < frames + d && all; ++f)
            all = a.waitFrame(f, 3000) && b.waitFrame(f, 3000);
        CHECK(all);
        bool same = true, right = true;
        for (uint32_t f = 0; f < frames + d; ++f)
            for (int pl = 0; pl < 2; ++pl)
            {
                same = same && a.input(pl, f) == b.input(pl, f);
                const PadState want = f < d ? PadState{} : pressed(static_cast<uint16_t>((pl ? 0x8000u : 0u) | (f - d)));
                right = right && a.input(pl, f) == want;
            }
        CHECK(same);
        CHECK(right);
    }

    // ---- master server (the list of public servers) ----

    using wotm_net::Master;
    using wotm_net::MasterConfig;

    struct RunningMaster
    {
        Master master;
        std::atomic<bool> stop{false};
        std::thread thread;
        explicit RunningMaster(MasterConfig c) : master(quiet(c))
        {
            std::string err;
            if (!master.start(err))
                std::printf("  master failed to start: %s\n", err.c_str());
            thread = std::thread([this] {
                while (!stop)
                    master.poll(1);
            });
        }
        ~RunningMaster()
        {
            stop = true;
            thread.join();
        }
        static MasterConfig quiet(MasterConfig c)
        {
            c.log = false;
            return c;
        }
    };

    // SERVERS: the SERVER lines up to END
    std::vector<std::string> serverRows(uint16_t masterPort)
    {
        LineSocket s;
        std::vector<std::string> rows;
        if (!s.connect("127.0.0.1", masterPort, 2000))
            return rows;
        s.sendLine("SERVERS");
        std::string line;
        while (s.readLine(line, 2000) && line != "END")
            if (line.rfind("SERVER ", 0) == 0)
                rows.push_back(line);
        return rows;
    }

    // waits up to 3 s for the master's list to be exactly `want`
    bool listBecomes(uint16_t masterPort, const std::vector<std::string> &want)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until)
        {
            if (serverRows(masterPort) == want)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    std::string masterReply(uint16_t masterPort, const std::string &line)
    {
        LineSocket s;
        std::string reply;
        if (s.connect("127.0.0.1", masterPort, 2000) && s.sendLine(line))
            s.readLine(reply, 2000);
        return reply;
    }

    // a server started with announce settings appears in the master's list under the address the master saw, with its port, name and
    // lobby counts (open lobbies, how many of them are waiting for players, players in lobbies), and the counts follow the server
    TEST(master_lists_announcing_servers_with_their_lobby_counts)
    {
        MasterConfig mc;
        mc.port = freePort();
        RunningMaster master(mc);
        ServerConfig sc = testConfig();
        sc.announceHost = "127.0.0.1";
        sc.announcePort = mc.port;
        sc.name = "LanesPC";
        sc.announceEveryMs = 50;
        RunningServer server(sc);
        const std::string row = "SERVER 127.0.0.1 " + std::to_string(sc.port) + " " + std::to_string(p::kProtocol) + " ";
        CHECK(listBecomes(mc.port, {row + "0 0 0 16 LanesPC"}));

        LineSocket ana, bia;
        hello(ana, sc.port);
        const std::string code = createLobby(ana, "Fun", "Ana", 3);
        CHECK(listBecomes(mc.port, {row + "1 1 1 16 LanesPC"}));
        hello(bia, sc.port);
        bia.sendLine("JOIN " + code + " Bia 1");
        CHECK(readUntil(bia, "JOINED") == "JOINED " + code);
        CHECK(listBecomes(mc.port, {row + "1 1 2 16 LanesPC"}));
        ana.close();
        bia.close();
        CHECK(listBecomes(mc.port, {row + "0 0 0 16 LanesPC"}));
    }

    // a server that stops announcing (closed, crashed, offline) leaves the list once its last announce is older than expireMs; one that
    // announces again comes back; two servers on the same address are told apart by their port
    TEST(master_forgets_servers_that_stop_announcing)
    {
        MasterConfig mc;
        mc.port = freePort();
        mc.expireMs = 300;
        RunningMaster master(mc);
        const std::string proto = std::to_string(p::kProtocol);
        CHECK(masterReply(mc.port, "ANNOUNCE " + proto + " 7777 2 1 3 16 Alpha") == "OK");
        CHECK(masterReply(mc.port, "ANNOUNCE " + proto + " 7790 0 0 0 4 Beta") == "OK");
        CHECK(listBecomes(mc.port, {"SERVER 127.0.0.1 7777 " + proto + " 2 1 3 16 Alpha", "SERVER 127.0.0.1 7790 " + proto + " 0 0 0 4 Beta"}));
        CHECK(listBecomes(mc.port, {}));
        CHECK(masterReply(mc.port, "ANNOUNCE " + proto + " 7777 1 0 2 16 Alpha") == "OK");
        CHECK(listBecomes(mc.port, {"SERVER 127.0.0.1 7777 " + proto + " 1 0 2 16 Alpha"}));
    }

    // malformed announces are refused and listed nowhere; servers of another protocol are listed with it (so a client can show them as
    // needing another version) instead of disappearing
    TEST(master_refuses_malformed_announces)
    {
        MasterConfig mc;
        mc.port = freePort();
        RunningMaster master(mc);
        CHECK(masterReply(mc.port, "ANNOUNCE 2 7777 1 1").rfind("ERROR", 0) == 0);
        CHECK(masterReply(mc.port, "ANNOUNCE 2 notaport 1 1 1 16 X").rfind("ERROR", 0) == 0);
        CHECK(masterReply(mc.port, "ANNOUNCE 2 7777 1 1 1 16 bad!name").rfind("ERROR", 0) == 0);
        CHECK(masterReply(mc.port, "HELLO").rfind("ERROR", 0) == 0);
        CHECK(masterReply(mc.port, "ANNOUNCE 9 7777 0 0 0 16 Future") == "OK");
        CHECK(listBecomes(mc.port, {"SERVER 127.0.0.1 7777 9 0 0 0 16 Future"}));
    }

    TEST(match_clients_share_every_frame_of_input)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1});
        MatchClient a, b;
        std::string err;
        CHECK(a.start(clientFor(cfg.port, *ms[0]), err));
        CHECK(b.start(clientFor(cfg.port, *ms[1]), err));
        CHECK(a.waitStarted(5000) && b.waitStarted(5000));
        CHECK(a.localPlayer() == 0 && b.localPlayer() == 1 && a.players() == 2);
        CHECK(a.delay() >= 2u && a.delay() == b.delay());
        CHECK(a.names() == std::vector<std::string>({"Ana", "Bia"}));
        exchange(a, b, 100);
        CHECK(a.status() == Status::Running && b.status() == Status::Running);
    }

    // up to 8 players (4 in the game's player slots and 4 in AI slots driven by the network): lobby, 30 Hz and everyone's inputs
    TEST(match_with_eight_players_shares_every_input)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        const std::vector<std::string> names = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
        auto ms = formMatch(cfg.port, names, {3, 1, 2, 5, 4, 7, 8, 9});
        CHECK(ms[7]->match.count("players") && ms[7]->match.at("players") == "8" && ms[7]->match.at("step") == "2");
        if (!ms[7]->match.count("you"))
            return;   // the eighth didn't get into the match: the rest can't run
        CHECK(ms[7]->match.at("you") == "7");
        std::vector<std::unique_ptr<MatchClient>> cs;
        std::string err;
        for (auto &m : ms)
        {
            cs.push_back(std::make_unique<MatchClient>());
            CHECK(cs.back()->start(clientFor(cfg.port, *m), err));
        }
        bool started = true;
        for (auto &c : cs)
            started = started && c->waitStarted(5000);
        CHECK(started);
        const uint32_t d = cs[0]->delay();
        for (uint32_t f = 0; f < 60; ++f)
            for (int p = 0; p < 8; ++p)
                cs[static_cast<size_t>(p)]->pushLocal(f + d, pressed(static_cast<uint16_t>((p << 8) | f)));
        bool all = true, same = true;
        for (auto &c : cs)
            all = all && c->waitFrame(d + 59, 5000);
        CHECK(all);
        for (uint32_t f = d; f < d + 60; ++f)
            for (int p = 0; p < 8; ++p)
                for (auto &c : cs)
                    same = same && c->input(p, f) == pressed(static_cast<uint16_t>((p << 8) | (f - d)));
        CHECK(same);
    }

    TEST(match_survives_lost_input_packets)
    {
        ServerConfig cfg = testConfig();
        cfg.dropInputPercent = 30;
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1});
        MatchClient a, b;
        std::string err;
        ClientConfig ca = clientFor(cfg.port, *ms[0]), cb = clientFor(cfg.port, *ms[1]);
        ca.dropInputPercent = cb.dropInputPercent = 30;
        CHECK(a.start(ca, err) && b.start(cb, err));
        CHECK(a.waitStarted(5000) && b.waitStarted(5000));
        exchange(a, b, 200);
    }

    TEST(match_rejects_unknown_tokens_and_other_configs)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1});
        MatchClient bad;
        std::string err;
        Member fake;
        fake.match["token"] = "0000000000000000";
        CHECK(bad.start(clientFor(cfg.port, fake), err));
        CHECK(!bad.waitStarted(1500) && bad.status() == Status::Rejected);
        CHECK(bad.message().find("token") != std::string::npos);

        MatchClient a, b;
        CHECK(a.start(clientFor(cfg.port, *ms[0], "cfgA"), err));
        // the server keeps the config of whoever joined first: b only starts after a has joined
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (a.status() != Status::Waiting && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(a.status() == Status::Waiting);
        CHECK(b.start(clientFor(cfg.port, *ms[1], "cfgB"), err));
        CHECK(!b.waitStarted(1500) && b.status() == Status::Rejected);
        CHECK(b.message().find("configuration") != std::string::npos);
    }

    TEST(match_drops_a_player_that_leaves_at_an_agreed_frame)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1});
        MatchClient a, b;
        std::string err;
        CHECK(a.start(clientFor(cfg.port, *ms[0]), err) && b.start(clientFor(cfg.port, *ms[1]), err));
        CHECK(a.waitStarted(5000) && b.waitStarted(5000));
        const uint32_t d = a.delay();
        for (uint32_t f = 0; f < 50; ++f)
            b.pushLocal(f + d, pressed(1));
        CHECK(b.waitAcked(3000));
        b.stop();
        for (uint32_t f = 0; f < 80; ++f)
            a.pushLocal(f + d, pressed(2));
        CHECK(a.waitFrame(d + 79, 5000));
        CHECK(a.dropFrame(1) == d + 50);
        CHECK(a.input(1, d + 49) == pressed(1) && a.input(1, d + 50) == PadState{});
    }

    TEST(match_stops_both_peers_on_a_state_hash_mismatch)
    {
        ServerConfig cfg = testConfig();
        RunningServer rs(cfg);
        auto ms = formMatch(cfg.port, {"Ana", "Bia"}, {3, 1});
        MatchClient a, b;
        std::string err;
        CHECK(a.start(clientFor(cfg.port, *ms[0]), err) && b.start(clientFor(cfg.port, *ms[1]), err));
        CHECK(a.waitStarted(5000) && b.waitStarted(5000));
        a.sendHash(30, 111);
        b.sendHash(30, 111);
        a.sendHash(60, 222);
        b.sendHash(60, 223);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((a.status() != Status::Desynced || b.status() != Status::Desynced) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(a.status() == Status::Desynced && b.status() == Status::Desynced);
        CHECK(a.desyncFrame() == 60u);
        CHECK(!a.waitFrame(a.delay(), 200));   // desynced: no new frame runs
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every line goes out immediately: a hung test shows where it stopped
    for (const Test &t : tests())
    {
        const int before = g_failures;
        try
        {
            t.run();
        }
        catch (const std::exception &e)   // an exception is a test failure, never an abort (which leaves the process stuck in the error report)
        {
            ++g_failures;
            std::printf("  EXCEPTION %s\n", e.what());
        }
        std::printf("%s %s\n", g_failures == before ? "ok     " : "FAILED ", t.name);
    }
    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
