// ABOUTME: Netcode server implementation: TCP lobby (text lines) and ENet lockstep matches (input relay, delay from ping,
// ABOUTME: state hashes, drops). No game code; runs in wotm_server and in the tests.
#include "wotm_net_server.hpp"
#include "wotm_monsters.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace wotm_net
{
    namespace
    {
        // peer->data holds the match's (lobby, slot): 1 + lobby * 64 + slot (0 = no token yet)
        void *peerTag(int lobby, int slot) { return reinterpret_cast<void *>(static_cast<uintptr_t>(1 + lobby * 64 + slot)); }

        std::string hex64(uint64_t v)
        {
            char b[17];
            std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
            return b;
        }

        std::string join(const std::vector<std::string> &v, const char *sep)
        {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i)
                s += (i ? sep : "") + v[i];
            return s;
        }
    }

    Server::Server(ServerConfig c) : cfg_(c), rng_(std::random_device{}()) { lobbies_.resize(static_cast<size_t>(std::max(1, c.lobbies))); }

    Server::~Server()
    {
        {
            std::lock_guard<std::mutex> lk(announceMu_);
            stopAnnouncing_ = true;
        }
        announceCv_.notify_all();
        if (announcer_.joinable())
            announcer_.join();
        if (host_)
            enet_host_destroy(host_);
        if (enetUp_)
            enet_deinitialize();
    }

    void Server::log(const char *fmt, ...)
    {
        if (!cfg_.log)
            return;
        va_list ap;
        va_start(ap, fmt);
        std::printf("[server] ");
        std::vprintf(fmt, ap);
        std::printf("\n");
        std::fflush(stdout);
        va_end(ap);
    }

    bool Server::start(std::string &err)
    {
        cfg_.maxPlayers = std::clamp(cfg_.maxPlayers, 1, proto::kMaxPlayers);
        if (!listener_.listen(cfg_.port))
        {
            err = "could not listen on TCP port " + std::to_string(cfg_.port);
            return false;
        }
        if (enet_initialize() != 0)
        {
            err = "enet_initialize failed";
            return false;
        }
        enetUp_ = true;
        ENetAddress a;
        a.host = ENET_HOST_ANY;
        a.port = cfg_.port;
        host_ = enet_host_create(&a, static_cast<size_t>(cfg_.lobbies * cfg_.maxPlayers + 16), proto::kChannels, 0, 0);
        if (!host_)
        {
            err = "could not listen on UDP port " + std::to_string(cfg_.port);
            return false;
        }
        log("listening on port %u (TCP lobby, UDP match), up to %d lobbies of up to %d players", cfg_.port, cfg_.lobbies, cfg_.maxPlayers);
        if (!cfg_.announceHost.empty())
        {
            log("announcing as %s to the master %s:%u every %d s", cfg_.name.c_str(), cfg_.announceHost.c_str(), cfg_.announcePort,
                cfg_.announceEveryMs / 1000);
            announcer_ = std::thread([this] { announceLoop(); });
        }
        return true;
    }

    void Server::poll(int ms)
    {
        if (!host_)   // start failed or was never called
            return;
        acceptAndReadLobby();
        serviceEnet(ms);
        tickMatches();
        updateCounts();
    }

    // ---- public listing (master server) ----

    void Server::updateCounts()
    {
        int open = 0, waiting = 0, players = 0;
        for (const Lobby &l : lobbies_)
        {
            if (!l.open)
                continue;
            ++open;
            waiting += l.match ? 0 : 1;
            players += static_cast<int>(l.match ? l.match->slots.size() : l.members.size());
        }
        open_ = open, waiting_ = waiting, players_ = players;
    }

    // On its own thread, so a slow or unreachable master never holds up the lobby or a match. A failed announce is retried at the
    // next interval (the master keeps a server listed for 3 intervals).
    void Server::announceLoop()
    {
        const std::string name = proto::cleanName(cfg_.name).empty() ? "wotlane" : proto::cleanName(cfg_.name);
        bool warned = false;
        std::unique_lock<std::mutex> lk(announceMu_);
        while (!stopAnnouncing_)
        {
            lk.unlock();
            LineSocket s;
            std::string reply;
            const bool ok = s.connect(cfg_.announceHost, cfg_.announcePort, 2000) &&
                            s.sendLine("ANNOUNCE " + std::to_string(proto::kProtocol) + " " + std::to_string(cfg_.port) + " " +
                                       std::to_string(open_.load()) + " " + std::to_string(waiting_.load()) + " " +
                                       std::to_string(players_.load()) + " " + std::to_string(cfg_.lobbies) + " " + name) &&
                            s.readLine(reply, 2000) && reply == "OK";
            if (ok == warned)   // log the first failure and the recovery, not every attempt
            {
                warned = !ok;
                log(ok ? "master %s:%u reachable again" : "could not announce to the master %s:%u (%s)", cfg_.announceHost.c_str(),
                    cfg_.announcePort, ok ? "" : (reply.empty() ? "no answer" : reply.c_str()));
            }
            lk.lock();
            announceCv_.wait_for(lk, std::chrono::milliseconds(cfg_.announceEveryMs), [this] { return stopAnnouncing_; });
        }
    }

    // ---- lobby ----

    void Server::acceptAndReadLobby()
    {
        LineSocket s;
        while (listener_.accept(s))
        {
            auto c = std::make_unique<Conn>();
            c->socket = std::move(s);
            auto free = std::find(conns_.begin(), conns_.end(), nullptr);
            if (free != conns_.end())
                *free = std::move(c);
            else
                conns_.push_back(std::move(c));
        }
        for (size_t i = 0; i < conns_.size(); ++i)
        {
            if (!conns_[i])
                continue;
            const bool alive = conns_[i]->socket.pump();
            std::string line;
            while (conns_[i] && conns_[i]->socket.nextLine(line))
                handleLine(static_cast<int>(i), line);
            if (conns_[i] && (!alive || !conns_[i]->socket.open()))
            {
                leaveLobby(static_cast<int>(i));
                conns_[i].reset();
            }
        }
    }

    int Server::openLobbies() const
    {
        int n = 0;
        for (const Lobby &l : lobbies_)
            n += l.open ? 1 : 0;
        return n;
    }

    int Server::lobbyByCode(const std::string &code) const
    {
        std::string up = code;
        for (char &ch : up)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (size_t i = 0; i < lobbies_.size(); ++i)
            if (lobbies_[i].open && lobbies_[i].code == up)
                return static_cast<int>(i);
        return -1;
    }

    std::string Server::newCode()
    {
        const std::string letters = proto::kCodeLetters;
        for (;;)
        {
            std::string code;
            for (int i = 0; i < 4; ++i)
                code += letters[static_cast<size_t>(rng_() % letters.size())];
            if (lobbyByCode(code) < 0)
                return code;
        }
    }

    std::string Server::lobbyLine(int i) const
    {
        const Lobby &l = lobbies_[static_cast<size_t>(i)];
        std::vector<std::string> names;
        if (l.match)
            for (const Slot &s : l.match->slots)
                names.push_back(s.name);
        else
            for (int m : l.members)
                names.push_back(conns_[static_cast<size_t>(m)]->name);
        const std::string host = l.host >= 0 && conns_[static_cast<size_t>(l.host)] ? conns_[static_cast<size_t>(l.host)]->name : "-";
        return "LOBBY " + l.code + (l.match ? " playing " : " waiting ") + std::to_string(names.size()) + "/" + std::to_string(cfg_.maxPlayers) +
               (l.password.empty() ? " open " : " locked ") + l.name + " " + host + " " + (names.empty() ? "-" : join(names, ","));
    }

    void Server::handleLine(int ci, const std::string &line)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        const std::vector<std::string> w = proto::words(line);
        if (w.empty())
            return;
        const std::string &cmd = w[0];
        if (cmd == "HELLO")
        {
            if (w.size() < 3 || w[1] != std::to_string(proto::kProtocol))
            {
                c.socket.sendLine("ERROR different protocol (the server speaks " + std::to_string(proto::kProtocol) + "; update wotlane)");
                return;
            }
            c.hello = true;
            c.build = w[2];
            c.socket.sendLine("WELCOME " + std::to_string(openLobbies()));
            return;
        }
        if (!c.hello)
        {
            c.socket.sendLine("ERROR send HELLO first");
            return;
        }
        if (cmd == "LIST")
        {
            for (size_t i = 0; i < lobbies_.size(); ++i)
                if (lobbies_[i].open)
                    c.socket.sendLine(lobbyLine(static_cast<int>(i)));
            c.socket.sendLine("END");
        }
        else if (cmd == "CREATE" && w.size() >= 4)
            createLobby(ci, w);
        else if (cmd == "JOIN" && w.size() >= 4)
            joinLobby(ci, w);
        else if (cmd == "SET")
            setLobby(ci, w);
        else if (cmd == "LEAVE")
        {
            const std::string code = c.lobby >= 0 ? lobbies_[static_cast<size_t>(c.lobby)].code : "";
            leaveLobby(ci);   // the last one out closes the lobby (and the code goes away)
            if (!code.empty())
                c.socket.sendLine("LEFT " + code);
        }
        else if (cmd == "START")
        {
            if (c.lobby < 0)
                return void(c.socket.sendLine("ERROR join a lobby first"));
            if (lobbies_[static_cast<size_t>(c.lobby)].host != ci)
                return void(c.socket.sendLine("ERROR only the lobby owner can start the match"));
            startMatch(c.lobby);
        }
        else
            c.socket.sendLine("ERROR unknown command: " + cmd);
    }

    // the monster exists on the disc and still has a free costume given the other members' (costumes as the match hands them out:
    // wotm_monsters::fits)
    bool Server::monsterFits(const Lobby &l, int monster, std::string &err) const
    {
        if (wotm_monsters::costumes(monster) == 0)
        {
            err = "invalid monster (no files on the disc)";
            return false;
        }
        int list[wotm_monsters::kMaxList], n = 0;
        for (int m : l.members)
            if (n < wotm_monsters::kMaxList - 1)
                list[n++] = conns_[static_cast<size_t>(m)]->monster;
        list[n++] = monster;
        if (!wotm_monsters::fits(list, nullptr, n))
        {
            err = "monster has no free costume in the lobby (all its costumes are taken)";
            return false;
        }
        return true;
    }

    // CREATE <lobby> <name> <monster> [password]
    void Server::createLobby(int ci, const std::vector<std::string> &w)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        const std::string lobbyName = proto::cleanName(w[1]), name = proto::cleanName(w[2]);
        const int monster = std::atoi(w[3].c_str());
        const std::string password = w.size() >= 5 ? proto::cleanName(w[4]) : "";
        if (lobbyName.empty() || name.empty())
            return void(c.socket.sendLine("ERROR empty name (use letters, digits, _ or -)"));
        int li = -1;
        for (size_t i = 0; i < lobbies_.size() && li < 0; ++i)
            if (!lobbies_[i].open && !lobbies_[i].match)
                li = static_cast<int>(i);
        if (li < 0)
            return void(c.socket.sendLine("ERROR the server already has " + std::to_string(lobbies_.size()) + " lobbies open; join one of them"));
        Lobby probe;
        std::string err;
        if (!monsterFits(probe, monster, err))
            return void(c.socket.sendLine("ERROR " + err));
        leaveLobby(ci);
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        l.open = true;
        l.code = newCode();
        l.name = lobbyName;
        l.password = password;
        l.host = -1;
        l.stage = 1;
        l.wins = 0;
        l.members.clear();
        l.build = c.build;
        c.socket.sendLine("CREATED " + l.code);
        log("lobby %s (%s): created by %s%s", l.code.c_str(), l.name.c_str(), name.c_str(), password.empty() ? "" : ", with password");
        addMember(ci, li, name, monster);
    }

    // JOIN <code> <name> <monster> [password]
    void Server::joinLobby(int ci, const std::vector<std::string> &w)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        const int li = lobbyByCode(w[1]);
        const std::string name = proto::cleanName(w[2]);
        const int monster = std::atoi(w[3].c_str());
        const std::string password = w.size() >= 5 ? w[4] : "";
        if (li < 0)
            return void(c.socket.sendLine("ERROR no such lobby (check the code)"));
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        if (c.lobby == li)
            return void(c.socket.sendLine("ERROR you are already in this lobby"));
        if (name.empty())
            return void(c.socket.sendLine("ERROR empty name (use letters, digits, _ or -)"));
        if (!l.password.empty() && password != l.password)
            return void(c.socket.sendLine("ERROR wrong password (the lobby has a password)"));
        if (l.match)
            return void(c.socket.sendLine("ERROR lobby is in a match; wait for it to end"));
        if (static_cast<int>(l.members.size()) >= cfg_.maxPlayers)
            return void(c.socket.sendLine("ERROR lobby is full"));
        if (l.build != c.build)
            return void(c.socket.sendLine("ERROR build differs from the lobby's (" + l.build + "); everyone needs the same wotlane"));
        for (int m : l.members)
            if (conns_[static_cast<size_t>(m)]->name == name)
                return void(c.socket.sendLine("ERROR name already taken in the lobby"));
        std::string err;
        if (!monsterFits(l, monster, err))
            return void(c.socket.sendLine("ERROR " + err));
        leaveLobby(ci);
        addMember(ci, li, name, monster);
    }

    void Server::addMember(int ci, int li, const std::string &name, int monster)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        l.members.push_back(ci);
        if (l.host < 0)
            l.host = ci;
        c.lobby = li, c.name = name, c.monster = monster;
        c.socket.sendLine("JOINED " + l.code);
        c.socket.sendLine("SETTINGS " + l.code + " stage=" + std::to_string(l.stage) + " wins=" + std::to_string(l.wins));
        log("lobby %s: %s joined (monster %d), %zu player(s)", l.code.c_str(), name.c_str(), monster, l.members.size());
        sendMembers(li);
    }

    // SET [stage=<stage>] [wins=<wins>]: owner only
    void Server::setLobby(int ci, const std::vector<std::string> &w)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        if (c.lobby < 0)
            return void(c.socket.sendLine("ERROR join a lobby first"));
        Lobby &l = lobbies_[static_cast<size_t>(c.lobby)];
        if (l.host != ci)
            return void(c.socket.sendLine("ERROR only the lobby owner can change the stage and wins"));
        const std::map<std::string, std::string> kv = proto::keyValues(w);
        if (auto it = kv.find("stage"); it != kv.end())
            l.stage = std::max(0, std::atoi(it->second.c_str()));
        if (auto it = kv.find("wins"); it != kv.end())
            l.wins = std::clamp(std::atoi(it->second.c_str()), 0, proto::kMaxWins);
        sendSettings(c.lobby);
    }

    void Server::leaveLobby(int ci)
    {
        Conn &c = *conns_[static_cast<size_t>(ci)];
        if (c.lobby < 0)
            return;
        Lobby &l = lobbies_[static_cast<size_t>(c.lobby)];
        const int li = c.lobby;
        l.members.erase(std::remove(l.members.begin(), l.members.end(), ci), l.members.end());
        c.lobby = -1;
        if (l.host == ci)
            l.host = l.members.empty() ? -1 : l.members.front();
        log("lobby %s: %s left", l.code.c_str(), c.name.c_str());
        if (!closeIfEmpty(li))
            sendMembers(li);
    }

    // a lobby with no members and no match closes (drops off the list and frees the slot); returns whether it closed
    bool Server::closeIfEmpty(int li)
    {
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        if (!l.open || !l.members.empty() || l.match)
            return false;
        log("lobby %s: empty, closed", l.code.c_str());
        l.open = false;
        l.code.clear(), l.name.clear(), l.password.clear(), l.build.clear();
        l.host = -1;
        return true;
    }

    void Server::sendMembers(int li)
    {
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        std::vector<std::string> v;
        for (int m : l.members)
            v.push_back(conns_[static_cast<size_t>(m)]->name + ":" + std::to_string(conns_[static_cast<size_t>(m)]->monster));
        const std::string host = l.host >= 0 ? conns_[static_cast<size_t>(l.host)]->name : "-";
        const std::string line = "MEMBERS " + l.code + " " + host + " " + (v.empty() ? "-" : join(v, ","));
        for (int m : l.members)
            conns_[static_cast<size_t>(m)]->socket.sendLine(line);
    }

    void Server::sendSettings(int li)
    {
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        const std::string line = "SETTINGS " + l.code + " stage=" + std::to_string(l.stage) + " wins=" + std::to_string(l.wins);
        for (int m : l.members)
            conns_[static_cast<size_t>(m)]->socket.sendLine(line);
    }

    // members stay in the lobby during the match (the launcher goes back to it when the game closes)
    void Server::startMatch(int li)
    {
        Lobby &l = lobbies_[static_cast<size_t>(li)];
        if (l.match || l.members.empty())
            return;
        auto m = std::make_unique<Match>();
        m->lobby = li;
        m->created = Clock::now();
        const int players = static_cast<int>(l.members.size());
        const uint32_t seed = static_cast<uint32_t>(rng_() % 0x7FFFFFFEull) + 1u;
        const int step = players >= 3 ? 2 : 1;   // 3+ players: the game switches to its own 30 Hz mode
        std::vector<std::string> monsters, names;
        for (int ci : l.members)
        {
            const Conn &c = *conns_[static_cast<size_t>(ci)];
            monsters.push_back(std::to_string(c.monster));
            names.push_back(c.name);
            Slot s;
            s.token = hex64(rng_());
            s.name = c.name;
            s.need.assign(static_cast<size_t>(players), 0u);
            m->slots.push_back(std::move(s));
        }
        for (int i = 0; i < players; ++i)
        {
            Conn &c = *conns_[static_cast<size_t>(l.members[static_cast<size_t>(i)])];
            c.socket.sendLine("MATCH lobby=" + l.code + " players=" + std::to_string(players) + " stage=" + std::to_string(l.stage) +
                              " wins=" + std::to_string(l.wins) + " monsters=" + join(monsters, ",") + " names=" + join(names, ",") +
                              " seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " you=" + std::to_string(i) +
                              " token=" + m->slots[static_cast<size_t>(i)].token + " port=" + std::to_string(cfg_.port));
        }
        log("lobby %s: match with %d player(s) (%s), stage %d, %d win(s), seed %u", l.code.c_str(), players, join(names, ",").c_str(),
            l.stage, l.wins, seed);
        l.match = std::move(m);
    }

    // ---- match ----

    void Server::send(ENetPeer *peer, uint8_t channel, const std::vector<uint8_t> &data, bool reliable)
    {
        if (!peer)
            return;
        if (!reliable && cfg_.dropInputPercent > 0 && static_cast<int>(rng_() % 100u) < cfg_.dropInputPercent)
            return;
        ENetPacket *p = enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        enet_peer_send(peer, channel, p);
    }

    Server::Match *Server::matchOf(ENetPeer *peer, int &slot)
    {
        const uintptr_t tag = reinterpret_cast<uintptr_t>(peer->data);
        if (tag == 0)
            return nullptr;
        const int li = static_cast<int>((tag - 1) / 64);
        slot = static_cast<int>((tag - 1) % 64);
        if (li >= static_cast<int>(lobbies_.size()) || !lobbies_[static_cast<size_t>(li)].match)
            return nullptr;
        Match *m = lobbies_[static_cast<size_t>(li)].match.get();
        return slot < static_cast<int>(m->slots.size()) && m->slots[static_cast<size_t>(slot)].peer == peer ? m : nullptr;
    }

    void Server::serviceEnet(int ms)
    {
        ENetEvent ev;
        int wait = ms;
        while (enet_host_service(host_, &ev, static_cast<enet_uint32>(wait)) > 0)
        {
            wait = 0;
            switch (ev.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                ev.peer->data = nullptr;
                enet_peer_ping_interval(ev.peer, 100);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
            {
                const std::vector<uint8_t> data(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                onReceive(ev.peer, data);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                onDisconnect(ev.peer);
                break;
            default:
                break;
            }
        }
    }

    void Server::onReceive(ENetPeer *peer, const std::vector<uint8_t> &data)
    {
        int si = 0;
        Match *m = matchOf(peer, si);
        switch (proto::typeOf(data))
        {
        case proto::Type::Join:
        {
            proto::Join j;
            if (proto::decode(data, j))
                onJoin(peer, j);
            break;
        }
        case proto::Type::Input:
        {
            proto::Input in;
            if (!m || !m->started || !proto::decode(data, in))
                break;
            Slot &s = m->slots[static_cast<size_t>(si)];
            for (size_t q = 0; q < in.acks.size() && q < s.need.size(); ++q)
                s.need[q] = std::max(s.need[q], in.acks[q]);
            const uint32_t before = m->inputs.nextMissing(si);
            for (size_t k = 0; k < in.own.states.size(); ++k)
                m->inputs.set(si, in.own.first + static_cast<uint32_t>(k), in.own.states[k]);
            if (m->inputs.nextMissing(si) != before)
                for (Slot &o : m->slots)
                    o.dirty = true;
            else
                s.dirty = true;   // ack again (the packet may be a resend because the ack was lost)
            break;
        }
        case proto::Type::Hash:
        {
            proto::Hash h;
            if (!m || !m->started || !proto::decode(data, h))
                break;
            auto &v = m->hashes[h.frame];
            v.resize(m->slots.size());
            v[static_cast<size_t>(si)] = h.hash;
            checkHashes(*m);
            break;
        }
        default:
            break;
        }
    }

    void Server::onJoin(ENetPeer *peer, const proto::Join &j)
    {
        auto reject = [&](const std::string &why) {
            send(peer, proto::kControl, proto::encode(proto::Reject{why}), true);
            enet_peer_disconnect_later(peer, 0);
            log("match: rejected (%s)", why.c_str());
        };
        if (j.protocol != proto::kProtocol)
            return reject("different protocol");
        for (size_t li = 0; li < lobbies_.size(); ++li)
        {
            Match *m = lobbies_[li].match.get();
            if (!m)
                continue;
            for (size_t si = 0; si < m->slots.size(); ++si)
            {
                Slot &s = m->slots[si];
                if (s.token != j.token)
                    continue;
                if (s.joined)
                    return reject("token already used");
                if (lobbies_[li].build != j.buildId && !lobbies_[li].build.empty())
                    return reject("different build (" + j.buildId + " vs " + lobbies_[li].build + ")");
                if (!m->config.empty() && m->config != j.configHash)
                    return reject("game configuration differs from the others (" + j.configHash + " vs " + m->config + ")");
                if (m->config.empty())
                    m->config = j.configHash;
                s.joined = true;
                s.peer = peer;
                peer->data = peerTag(static_cast<int>(li), static_cast<int>(si));
                send(peer, proto::kControl, proto::encode(proto::Welcome{static_cast<uint8_t>(si), static_cast<uint8_t>(m->slots.size())}), true);
                log("lobby %zu: %s reached the match (player %zu)", li, s.name.c_str(), si + 1);
                return;
            }
        }
        reject("unknown token (the match ended or it belongs to another server)");
    }

    void Server::onDisconnect(ENetPeer *peer)
    {
        int si = 0;
        if (Match *m = matchOf(peer, si))
        {
            m->slots[static_cast<size_t>(si)].peer = nullptr;
            dropSlot(*m, si);
        }
        peer->data = nullptr;
    }

    // player gone: neutral from the first frame of theirs the server doesn't have (everyone gets the earlier ones)
    void Server::dropSlot(Match &m, int si)
    {
        Slot &s = m.slots[static_cast<size_t>(si)];
        if (s.gone)
            return;
        s.gone = true;
        const uint32_t frame = m.started ? m.inputs.nextMissing(si) : 0u;
        if (m.started)
            m.inputs.drop(si, frame);
        for (Slot &o : m.slots)
            if (o.peer && !o.gone)
            {
                send(o.peer, proto::kControl, proto::encode(proto::Drop{static_cast<uint8_t>(si), frame}), true);
                o.dirty = true;
            }
        if (!m.started)
            m.hashes.clear();
        log("lobby %s: %s left the match (neutral from frame %u)", lobbies_[static_cast<size_t>(m.lobby)].code.c_str(), s.name.c_str(), frame);
        checkHashes(m);
    }

    void Server::tickMatches()
    {
        const Clock::time_point now = Clock::now();
        for (Lobby &l : lobbies_)
        {
            Match *m = l.match.get();
            if (!m)
                continue;
            if (!m->started)
            {
                bool all = true;
                for (Slot &s : m->slots)
                    all = all && (s.joined || s.gone);
                if (!all && now - m->created > std::chrono::milliseconds(cfg_.joinTimeoutMs))
                    for (size_t i = 0; i < m->slots.size(); ++i)
                        if (!m->slots[i].joined)
                            dropSlot(*m, static_cast<int>(i));
                if (all && !m->allJoinedSet)
                    m->allJoinedSet = true, m->allJoined = now;
                if (all && now - m->allJoined >= std::chrono::milliseconds(cfg_.startWaitMs))
                {
                    std::vector<uint32_t> rtt;
                    proto::Start st;
                    for (Slot &s : m->slots)
                    {
                        if (s.peer && !s.gone)
                            rtt.push_back(s.peer->roundTripTime);
                        st.names.push_back(s.name);
                    }
                    const uint32_t frameMs = m->slots.size() >= 3 ? 33u : 16u;
                    st.delay = cfg_.forcedDelay ? cfg_.forcedDelay : proto::pickDelay(rtt, frameMs);
                    m->delay = st.delay;
                    m->inputs = InputTimeline(static_cast<int>(m->slots.size()), st.delay);
                    for (size_t i = 0; i < m->slots.size(); ++i)
                        if (m->slots[i].gone)
                            m->inputs.drop(static_cast<int>(i), 0);
                    m->started = true;
                    std::string pings;
                    for (uint32_t r : rtt)
                        pings += (pings.empty() ? "" : ",") + std::to_string(r);
                    log("lobby %s: start, delay %u frame(s) (ping ms: %s)", l.code.c_str(), st.delay, pings.c_str());
                    for (Slot &s : m->slots)
                        if (s.peer && !s.gone)
                        {
                            send(s.peer, proto::kControl, proto::encode(st), true);
                            for (size_t q = 0; q < m->slots.size(); ++q)
                                if (m->slots[q].gone)
                                    send(s.peer, proto::kControl, proto::encode(proto::Drop{static_cast<uint8_t>(q), 0}), true);
                        }
                }
            }
            else if (!m->desynced)
            {
                for (size_t i = 0; i < m->slots.size(); ++i)
                {
                    Slot &s = m->slots[i];
                    if (!s.peer || s.gone)
                        continue;
                    bool behind = m->inputs.nextMissing(static_cast<int>(i)) != s.lastAckOwn;
                    for (size_t q = 0; q < m->slots.size(); ++q)
                        behind = behind || (q != i && s.need[q] < m->inputs.nextMissing(static_cast<int>(q)));
                    if (s.dirty || (behind && now - s.lastSend > std::chrono::milliseconds(16)))
                        sendInputs(*m, static_cast<int>(i));
                }
                uint32_t oldest = 0xFFFFFFFFu;
                for (const Slot &s : m->slots)
                    if (!s.gone)
                        for (uint32_t n : s.need)
                            oldest = std::min(oldest, n);
                if (oldest != 0xFFFFFFFFu)
                    m->inputs.discardBefore(oldest);
            }
            bool everyoneGone = true;
            for (const Slot &s : m->slots)
                everyoneGone = everyoneGone && s.gone;
            if (everyoneGone)
            {
                const int li = m->lobby;
                log("lobby %s: match over; lobby waiting again with %zu member(s)", l.code.c_str(), l.members.size());
                l.match.reset();
                if (!closeIfEmpty(li))
                    sendMembers(li);   // tells the members (each one's launcher) the lobby is waiting again
            }
        }
    }

    void Server::sendInputs(Match &m, int si)
    {
        Slot &s = m.slots[static_cast<size_t>(si)];
        proto::Inputs out;
        out.ackOwn = m.inputs.nextMissing(si);
        for (size_t q = 0; q < m.slots.size(); ++q)
        {
            if (static_cast<int>(q) == si)
                continue;
            const uint32_t from = s.need[q], to = std::min(m.inputs.nextMissing(static_cast<int>(q)), from + static_cast<uint32_t>(proto::kMaxRun));
            if (from >= to)
                continue;
            proto::InputRun r;
            r.player = static_cast<uint8_t>(q);
            r.first = from;
            for (uint32_t f = from; f < to; ++f)
                r.states.push_back(m.inputs.get(static_cast<int>(q), f));
            out.runs.push_back(std::move(r));
        }
        send(s.peer, proto::kInputs, proto::encode(out), false);
        s.lastAckOwn = out.ackOwn;
        s.lastSend = Clock::now();
        s.dirty = false;
    }

    void Server::checkHashes(Match &m)
    {
        for (auto it = m.hashes.begin(); it != m.hashes.end();)
        {
            const uint32_t frame = it->first;
            auto &v = it->second;
            v.resize(m.slots.size());
            bool complete = true;
            std::optional<uint64_t> first;
            std::string firstName;
            std::vector<std::string> differ;
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (m.slots[i].gone || m.inputs.dropped(static_cast<int>(i), frame))
                    continue;
                if (!v[i])
                {
                    complete = false;
                    break;
                }
                if (!first)
                    first = v[i], firstName = m.slots[i].name;
                else if (*v[i] != *first)
                    differ.push_back(m.slots[i].name);
            }
            if (!complete)
            {
                ++it;
                continue;
            }
            if (!differ.empty() && !m.desynced)
            {
                m.desynced = true;
                std::string detail = "state differs on frame " + std::to_string(frame) + ": " + firstName + " vs " + join(differ, ",");
                log("lobby %s: DESYNC %s", lobbies_[static_cast<size_t>(m.lobby)].code.c_str(), detail.c_str());
                for (Slot &s : m.slots)
                    if (s.peer && !s.gone)
                        send(s.peer, proto::kControl, proto::encode(proto::Desync{frame, detail}), true);
            }
            it = m.hashes.erase(it);
        }
    }
}
