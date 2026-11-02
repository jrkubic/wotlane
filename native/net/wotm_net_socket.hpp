// ABOUTME: Line-based text TCP socket (the netcode lobby) and a non-blocking listener, for Windows (Winsock) and POSIX.
// ABOUTME: Used by the server (accepts and reads many connections) and by the tests and tools that talk to the lobby.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wotm_net
{
#ifdef _WIN32
    using SocketHandle = SOCKET;
    constexpr SocketHandle kNoSocket = INVALID_SOCKET;
    inline void closeSocket(SocketHandle s) { closesocket(s); }
    inline bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
    inline void setNonBlocking(SocketHandle s)
    {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }
    // Winsock reference-counts WSAStartup: each user calls it once
    inline bool socketsInit()
    {
        static const bool ok = [] {
            WSADATA d;
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        return ok;
    }
#else
    using SocketHandle = int;
    constexpr SocketHandle kNoSocket = -1;
    inline void closeSocket(SocketHandle s) { ::close(s); }
    inline bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
    inline void setNonBlocking(SocketHandle s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
    inline bool socketsInit() { return true; }
#endif

    // waits up to `ms` for the socket to become readable (or writable)
    inline bool waitSocket(SocketHandle s, int ms, bool write)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        const int n = select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
        return n > 0;
    }

    // Sending to a peer that already closed raises SIGPIPE on POSIX, which kills the process (wotm_server when a lobby member drops);
    // with the signal suppressed, send fails and sendLine closes the socket like any other error. Windows has no SIGPIPE.
#if defined(MSG_NOSIGNAL)
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif

    class LineSocket
    {
    public:
        LineSocket() = default;
        explicit LineSocket(SocketHandle s) : s_(s) { setup(); }
        LineSocket(const LineSocket &) = delete;
        LineSocket &operator=(const LineSocket &) = delete;
        LineSocket(LineSocket &&o) noexcept : s_(o.s_), in_(std::move(o.in_)) { o.s_ = kNoSocket; }
        LineSocket &operator=(LineSocket &&o) noexcept
        {
            if (this != &o)
            {
                close();
                s_ = o.s_, in_ = std::move(o.in_);
                o.s_ = kNoSocket;
            }
            return *this;
        }
        ~LineSocket() { close(); }

        bool open() const { return s_ != kNoSocket; }

        bool connect(const std::string &host, uint16_t port, int timeoutMs)
        {
            close();
            if (!socketsInit())
                return false;
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
                return false;
            s_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            bool ok = s_ != kNoSocket;
            if (ok)
            {
                setNonBlocking(s_);
                const int r = ::connect(s_, res->ai_addr, static_cast<int>(res->ai_addrlen));
                ok = r == 0 || (wouldBlock() && waitSocket(s_, timeoutMs, true) && connectedOk());
            }
            freeaddrinfo(res);
            if (!ok)
                close();
            else
                setup();
            return ok;
        }

        bool sendLine(const std::string &line)
        {
            if (!open())
                return false;
            const std::string s = line + "\n";
            size_t sent = 0;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (sent < s.size())
            {
                const int n = ::send(s_, s.data() + sent, static_cast<int>(s.size() - sent), kSendFlags);
                if (n > 0)
                    sent += static_cast<size_t>(n);
                else if (n < 0 && wouldBlock() && std::chrono::steady_clock::now() < until)
                    waitSocket(s_, 50, true);
                else
                {
                    close();
                    return false;
                }
            }
            return true;
        }

        // reads whatever has arrived without blocking; false if the connection dropped
        bool pump()
        {
            if (!open())
                return false;
            char buf[4096];
            for (;;)
            {
                const int n = ::recv(s_, buf, sizeof buf, 0);
                if (n > 0)
                {
                    in_.append(buf, static_cast<size_t>(n));
                    if (in_.size() > 65536u)   // absurdly long line: drop the connection
                    {
                        close();
                        return false;
                    }
                    continue;
                }
                if (n < 0 && wouldBlock())
                    return true;
                close();
                return false;
            }
        }

        // next complete line already received (without the \n and \r)
        bool nextLine(std::string &line)
        {
            const size_t nl = in_.find('\n');
            if (nl == std::string::npos)
                return false;
            line = in_.substr(0, nl);
            in_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }

        // waits up to `timeoutMs` for a line
        bool readLine(std::string &line, int timeoutMs)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                if (nextLine(line))
                    return true;
                if (!pump())
                    return nextLine(line);
                if (nextLine(line))
                    return true;
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
                if (left <= 0)
                    return false;
                waitSocket(s_, static_cast<int>(std::min<long long>(left, 50)), false);
            }
        }

        void close()
        {
            if (s_ != kNoSocket)
                closeSocket(s_);
            s_ = kNoSocket;
        }

        SocketHandle handle() const { return s_; }

    private:
        void setup()
        {
            setNonBlocking(s_);
            int one = 1;
            setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
#ifdef SO_NOSIGPIPE   // macOS: no MSG_NOSIGNAL, the socket option does the same
            setsockopt(s_, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&one), sizeof one);
#endif
        }
        bool connectedOk()
        {
            int err = 0;
            socklen_t len = sizeof err;
            return getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len) == 0 && err == 0;
        }

        SocketHandle s_ = kNoSocket;
        std::string in_;
    };

    class Listener
    {
    public:
        ~Listener() { close(); }
        bool listen(uint16_t port)
        {
            if (!socketsInit())
                return false;
            s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s_ == kNoSocket)
                return false;
            int one = 1;
            setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(port);
            if (bind(s_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(s_, 16) != 0)
            {
                close();
                return false;
            }
            setNonBlocking(s_);
            return true;
        }
        // a new connection, if there is one; `peerIp` (optional) gets the address it came from
        bool accept(LineSocket &out, std::string *peerIp = nullptr)
        {
            if (s_ == kNoSocket)
                return false;
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const SocketHandle c = ::accept(s_, reinterpret_cast<sockaddr *>(&from), &len);
            if (c == kNoSocket)
                return false;
            if (peerIp)
            {
                char buf[INET_ADDRSTRLEN] = {};
                *peerIp = inet_ntop(AF_INET, &from.sin_addr, buf, sizeof buf) ? buf : "";
            }
            out = LineSocket(c);
            return true;
        }
        void close()
        {
            if (s_ != kNoSocket)
                closeSocket(s_);
            s_ = kNoSocket;
        }

    private:
        SocketHandle s_ = kNoSocket;
    };
}
