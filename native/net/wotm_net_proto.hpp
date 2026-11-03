// ABOUTME: Netcode protocol: binary match messages (ENet) and lobby text lines (TCP), with encoding and parsing.
// ABOUTME: No dependency on the game or on sockets; used by the client (wotm.exe), the server (wotm_server) and the tests.
#pragma once

#include "wotm_lockstep.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wotm_net::proto
{
    constexpr uint16_t kProtocol = 2;
    constexpr int kMaxPlayers = 8;   // 4 in the game's player slots and 4 in AI slots driven by the network (wotm_multi.inc)
    constexpr uint16_t kDefaultPort = 7777;
    constexpr uint16_t kDefaultMasterPort = 7778;   // the master server (list of public servers, wotm_net_master.hpp)
    constexpr size_t kMaxRun = 64;   // input frames per message (lockstep never leaves more than the delay pending)

    // ENet channels: reliable control (join, start, drop, hash, desync) and unacknowledged inputs (each packet resends whatever the
    // other side hasn't acknowledged yet, so losing one costs nothing)
    enum Channel : uint8_t
    {
        kControl = 0,
        kInputs = 1,
        kChannels = 2
    };

    enum class Type : uint8_t
    {
        None = 0,
        Join = 1,     // client -> server: lobby token and build identity
        Welcome,      // server -> client: player index and player count
        Reject,       // server -> client: reason, then the connection closes
        Start,        // server -> client: everyone arrived; input delay and names
        Input,        // client -> server: this player's inputs and what it has received from each player
        Inputs,       // server -> client: the others' inputs and what it has received from this client
        Hash,         // client -> server: state hash on a frame
        Desync,       // server -> client: the hashes for a frame differ; the match stops
        Drop,         // server -> client: a player left; neutral from a given frame
    };

    struct Join
    {
        uint16_t protocol = kProtocol;
        std::string token, buildId, configHash, name;
    };
    struct Welcome
    {
        uint8_t player = 0, players = 0;
    };
    struct Reject
    {
        std::string reason;
    };
    struct Start
    {
        uint32_t delay = 0;
        std::vector<std::string> names;
    };
    struct InputRun
    {
        uint8_t player = 0;
        uint32_t first = 0;
        std::vector<PadState> states;
    };
    struct Input
    {
        std::vector<uint32_t> acks;   // per player: first frame this client doesn't have yet
        InputRun own;
    };
    struct Inputs
    {
        uint32_t ackOwn = 0;          // first frame of this client's own input the server doesn't have yet
        std::vector<InputRun> runs;
    };
    struct Hash
    {
        uint32_t frame = 0;
        uint64_t hash = 0;
    };
    struct Desync
    {
        uint32_t frame = 0;
        std::string detail;
    };
    struct Drop
    {
        uint8_t player = 0;
        uint32_t frame = 0;
    };

    class Writer
    {
    public:
        explicit Writer(Type t) { u8(static_cast<uint8_t>(t)); }
        void u8(uint8_t v) { b_.push_back(v); }
        void u16(uint16_t v) { u8(static_cast<uint8_t>(v)), u8(static_cast<uint8_t>(v >> 8)); }
        void u32(uint32_t v) { u16(static_cast<uint16_t>(v)), u16(static_cast<uint16_t>(v >> 16)); }
        void u64(uint64_t v) { u32(static_cast<uint32_t>(v)), u32(static_cast<uint32_t>(v >> 32)); }
        void str(const std::string &s)
        {
            const size_t n = std::min<size_t>(s.size(), 255u);
            u8(static_cast<uint8_t>(n));
            b_.insert(b_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        }
        void pad(const PadState &s) { u16(s.buttons), u8(s.rx), u8(s.ry), u8(s.lx), u8(s.ly); }
        // a state equal to the previous one costs 1 byte (0); a new one, 1 + 6 bytes
        void run(const InputRun &r)
        {
            const size_t n = std::min(r.states.size(), kMaxRun);
            u8(r.player), u32(r.first), u8(static_cast<uint8_t>(n));
            for (size_t i = 0; i < n; ++i)
            {
                const bool same = i > 0 && r.states[i] == r.states[i - 1];
                u8(same ? 0u : 1u);
                if (!same)
                    pad(r.states[i]);
            }
        }
        std::vector<uint8_t> take() { return std::move(b_); }

    private:
        std::vector<uint8_t> b_;
    };

    class Reader
    {
    public:
        Reader(const std::vector<uint8_t> &b, Type expected) : p_(b.data()), e_(b.data() + b.size())
        {
            ok_ = u8() == static_cast<uint8_t>(expected);
        }
        bool ok() const { return ok_; }
        bool done() const { return ok_ && p_ == e_; }
        uint8_t u8()
        {
            if (p_ >= e_)
            {
                ok_ = false;
                return 0;
            }
            return *p_++;
        }
        uint16_t u16()
        {
            const uint16_t lo = u8();
            return static_cast<uint16_t>(lo | (u8() << 8));
        }
        uint32_t u32()
        {
            const uint32_t lo = u16();
            return lo | (static_cast<uint32_t>(u16()) << 16);
        }
        uint64_t u64()
        {
            const uint64_t lo = u32();
            return lo | (static_cast<uint64_t>(u32()) << 32);
        }
        std::string str()
        {
            const size_t n = u8();
            if (static_cast<size_t>(e_ - p_) < n)
            {
                ok_ = false;
                return {};
            }
            std::string s(reinterpret_cast<const char *>(p_), n);
            p_ += n;
            return s;
        }
        PadState pad()
        {
            PadState s;
            s.buttons = u16();
            s.rx = u8(), s.ry = u8(), s.lx = u8(), s.ly = u8();
            return s;
        }
        InputRun run()
        {
            InputRun r;
            r.player = u8();
            r.first = u32();
            const size_t n = u8();
            for (size_t i = 0; i < n && ok_; ++i)
            {
                const uint8_t kind = u8();
                if (kind == 0u && !r.states.empty())
                    r.states.push_back(r.states.back());
                else if (kind == 1u)
                    r.states.push_back(pad());
                else
                    ok_ = false;
            }
            return r;
        }

    private:
        const uint8_t *p_, *e_;
        bool ok_ = true;
    };

    inline Type typeOf(const std::vector<uint8_t> &b) { return b.empty() ? Type::None : static_cast<Type>(b[0]); }

    inline std::vector<uint8_t> encode(const Join &m)
    {
        Writer w(Type::Join);
        w.u16(m.protocol), w.str(m.token), w.str(m.buildId), w.str(m.configHash), w.str(m.name);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Join &m)
    {
        Reader r(b, Type::Join);
        m.protocol = r.u16(), m.token = r.str(), m.buildId = r.str(), m.configHash = r.str(), m.name = r.str();
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Welcome &m)
    {
        Writer w(Type::Welcome);
        w.u8(m.player), w.u8(m.players);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Welcome &m)
    {
        Reader r(b, Type::Welcome);
        m.player = r.u8(), m.players = r.u8();
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Reject &m)
    {
        Writer w(Type::Reject);
        w.str(m.reason);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Reject &m)
    {
        Reader r(b, Type::Reject);
        m.reason = r.str();
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Start &m)
    {
        Writer w(Type::Start);
        w.u32(m.delay), w.u8(static_cast<uint8_t>(m.names.size()));
        for (const std::string &n : m.names)
            w.str(n);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Start &m)
    {
        Reader r(b, Type::Start);
        m.delay = r.u32();
        const size_t n = r.u8();
        m.names.clear();
        for (size_t i = 0; i < n && r.ok(); ++i)
            m.names.push_back(r.str());
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Input &m)
    {
        Writer w(Type::Input);
        w.u8(static_cast<uint8_t>(m.acks.size()));
        for (uint32_t a : m.acks)
            w.u32(a);
        w.run(m.own);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Input &m)
    {
        Reader r(b, Type::Input);
        const size_t n = r.u8();
        m.acks.clear();
        for (size_t i = 0; i < n && r.ok(); ++i)
            m.acks.push_back(r.u32());
        m.own = r.run();
        return r.done() && n <= static_cast<size_t>(kMaxPlayers);
    }

    inline std::vector<uint8_t> encode(const Inputs &m)
    {
        Writer w(Type::Inputs);
        w.u32(m.ackOwn), w.u8(static_cast<uint8_t>(m.runs.size()));
        for (const InputRun &run : m.runs)
            w.run(run);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Inputs &m)
    {
        Reader r(b, Type::Inputs);
        m.ackOwn = r.u32();
        const size_t n = r.u8();
        m.runs.clear();
        for (size_t i = 0; i < n && r.ok(); ++i)
            m.runs.push_back(r.run());
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Hash &m)
    {
        Writer w(Type::Hash);
        w.u32(m.frame), w.u64(m.hash);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Hash &m)
    {
        Reader r(b, Type::Hash);
        m.frame = r.u32(), m.hash = r.u64();
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Desync &m)
    {
        Writer w(Type::Desync);
        w.u32(m.frame), w.str(m.detail);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Desync &m)
    {
        Reader r(b, Type::Desync);
        m.frame = r.u32(), m.detail = r.str();
        return r.done();
    }

    inline std::vector<uint8_t> encode(const Drop &m)
    {
        Writer w(Type::Drop);
        w.u8(m.player), w.u32(m.frame);
        return w.take();
    }
    inline bool decode(const std::vector<uint8_t> &b, Drop &m)
    {
        Reader r(b, Type::Drop);
        m.player = r.u8(), m.frame = r.u32();
        return r.done();
    }

    // Input delay (frames) for each player's round-trip time to the server (ms): a player's input goes to the server and from there
    // to the others, so the worst path is half the slowest round trip plus half the second slowest; plus 1 frame of slack for frame
    // phase. Clamped to 2..15.
    inline uint32_t pickDelay(std::vector<uint32_t> rttMs, uint32_t frameMs)
    {
        std::sort(rttMs.begin(), rttMs.end(), [](uint32_t a, uint32_t b) { return a > b; });
        const uint32_t a = rttMs.size() > 0 ? rttMs[0] : 0u, b = rttMs.size() > 1 ? rttMs[1] : 0u;
        const uint32_t oneWay = (a + b + 1u) / 2u;
        const uint32_t frames = frameMs ? (oneWay + frameMs - 1u) / frameMs + 1u : 2u;
        return std::clamp(frames, 2u, 15u);
    }

    // ---- lobby (TCP, one text line per message) ----
    // Whoever creates the lobby is the owner: only the owner changes the stage and wins and starts the match; if the owner leaves, the
    // next member becomes owner. The lobby has a 4-letter code (to join without the list) and may have a password. Members stay in it
    // during the match and, when the match ends (everyone has left it: QUIT closes the game), it goes back to waiting with the same
    // members. An empty lobby closes.
    // client: HELLO <protocol> <build> | LIST | CREATE <lobby> <name> <monster> [password] | JOIN <code> <name> <monster> [password] |
    //         SET [stage=<stage>] [wins=<wins>] | START | LEAVE
    // server: WELCOME <open lobbies> | LOBBY <code> <waiting|playing> <n>/<max> <open|locked> <lobby> <owner> <names,...|-> |
    //         END | CREATED <code> | JOINED <code> | LEFT <code> | SETTINGS <code> stage=<stage> wins=<wins> |
    //         MEMBERS <code> <owner> <name:monster,...> | MATCH <key=value>... | ERROR <text>
    constexpr int kMaxWins = 99;
    constexpr const char *kCodeLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no I, O, 0 or 1 (easily confused when reading the code aloud)
    inline std::vector<std::string> words(const std::string &line)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j])))
                ++j;
            if (j > i)
                out.push_back(line.substr(i, j - i));
            i = j;
        }
        return out;
    }

    inline std::map<std::string, std::string> keyValues(const std::vector<std::string> &w)
    {
        std::map<std::string, std::string> kv;
        for (const std::string &s : w)
            if (const size_t eq = s.find('='); eq != std::string::npos && eq > 0)
                kv[s.substr(0, eq)] = s.substr(eq + 1);
        return kv;
    }

    // player name: letters, digits, '_' and '-', up to 16
    inline std::string cleanName(const std::string &s)
    {
        std::string out;
        for (char c : s)
            if ((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') && out.size() < 16u)
                out += c;
        return out;
    }
}
