// ABOUTME: wotm_server lobby client for the launcher (C# 5, .NET Framework): line-based TCP connection, queue of received lines,
// ABOUTME: the wotm.exe environment for a MATCH line and the master server's public list (as net_lobby.py). Tested by LobbyTest.cs.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

class LobbyError : Exception
{
    public LobbyError(string m) : base(m) { }
}

// One connection to the lobby (line protocol from native/net/wotm_net_proto.hpp). A thread reads the lines into a queue; callers
// read with ReadLine (tests, request/response) or drain it with Poll (the launcher, from a UI Timer).
class LobbyClient : IDisposable
{
    public const int Protocol = 2;
    public readonly string Host;
    public readonly int Port;
    public int Lobbies { get; private set; }
    readonly TcpClient tcp;
    readonly StreamReader reader;
    readonly StreamWriter writer;
    readonly BlockingCollection<string> lines = new BlockingCollection<string>();
    readonly Thread thread;
    volatile bool closed;

    public LobbyClient(string host, int port, string build, int timeoutMs)
    {
        Host = host;
        Port = port;
        tcp = new TcpClient();
        IAsyncResult ar = tcp.BeginConnect(host, port, null, null);
        if (!ar.AsyncWaitHandle.WaitOne(timeoutMs) || !tcp.Connected)
        {
            tcp.Close();
            throw new LobbyError("no answer from " + host + ":" + port + " (is wotm_server running? is the TCP port open?)");
        }
        tcp.EndConnect(ar);
        tcp.NoDelay = true;
        NetworkStream s = tcp.GetStream();
        reader = new StreamReader(s, new UTF8Encoding(false));
        writer = new StreamWriter(s, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        thread = new Thread(ReadLoop) { IsBackground = true };
        thread.Start();
        Send("HELLO " + Protocol + " " + build);
        string w = Expect("WELCOME", timeoutMs);
        Lobbies = int.Parse(w.Split(' ')[1]);
    }

    void ReadLoop()
    {
        try
        {
            string l;
            while ((l = reader.ReadLine()) != null) lines.Add(l);
        }
        catch (Exception) { }
        closed = true;
        lines.Add(null);   // wakes up whoever is waiting
    }

    public bool Closed { get { return closed; } }

    public void Send(string line) { writer.WriteLine(line); }

    // next line within timeoutMs; null on timeout or if the connection dropped
    public string ReadLine(int timeoutMs)
    {
        string l;
        return lines.TryTake(out l, timeoutMs) ? l : null;
    }

    // lines already received, without waiting
    public List<string> Poll()
    {
        var r = new List<string>();
        string l;
        while (lines.TryTake(out l)) if (l != null) r.Add(l);
        return r;
    }

    // reads until a line that starts with prefix; ERROR becomes LobbyError
    public string Expect(string prefix, int timeoutMs)
    {
        DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (true)
        {
            int left = (int)(until - DateTime.UtcNow).TotalMilliseconds;
            string l = left > 0 ? ReadLine(left) : null;
            if (l == null) throw new LobbyError(closed ? "the server closed the connection" : "no answer from the server");
            if (l.StartsWith("ERROR")) throw new LobbyError(l.Length > 6 ? l.Substring(6) : l);
            if (l.StartsWith(prefix)) return l;
        }
    }

    public void Dispose()
    {
        try { tcp.Close(); } catch (Exception) { }
    }

    // LOBBY <code> <waiting|playing> <n>/<max> <open|locked> <lobby> <owner> <names,...|->
    public class LobbyInfo
    {
        public int Count, Max;
        public string Code, State, Name, Host;
        public bool Locked;
        public string[] Names;
    }

    public static LobbyInfo ParseLobby(string line)
    {
        string[] w = line.Split(' ');
        if (w.Length < 8 || w[0] != "LOBBY") return null;
        string[] n = w[3].Split('/');
        return new LobbyInfo
        {
            Code = w[1], State = w[2], Count = int.Parse(n[0]), Max = int.Parse(n[1]), Locked = w[4] == "locked", Name = w[5], Host = w[6],
            Names = w[7] == "-" ? new string[0] : w[7].Split(',')
        };
    }

    // MEMBERS <code> <owner> <name:monster,...>
    public class MemberInfo
    {
        public string Code, Host;
        public string[] Names;
    }

    public static MemberInfo ParseMembers(string line)
    {
        string[] w = line.Split(' ');
        if (w.Length < 4 || w[0] != "MEMBERS") return null;
        return new MemberInfo { Code = w[1], Host = w[2], Names = w[3] == "-" ? new string[0] : w[3].Split(',') };
    }

    // the lobby's stage, wins and modifiers (SETTINGS <code> stage=<n> wins=<n> mods=<list>), the latest that arrived through Create,
    // Join, Set or SetMods
    public Dictionary<string, string> Settings = new Dictionary<string, string>();

    // Creates a lobby (the creator is its owner) and joins it; returns the code
    public string Create(string lobby, string name, int monster, string password, int timeoutMs)
    {
        Send("CREATE " + lobby + " " + name + " " + monster + (string.IsNullOrEmpty(password) ? "" : " " + password));
        string code = Expect("CREATED", timeoutMs).Split(' ')[1];
        Joined(timeoutMs);
        return code;
    }

    public void Join(string code, string name, int monster, string password, int timeoutMs)
    {
        Send("JOIN " + code + " " + name + " " + monster + (string.IsNullOrEmpty(password) ? "" : " " + password));
        Joined(timeoutMs);
    }

    // JOINED and the SETTINGS the server sends right after it
    void Joined(int timeoutMs)
    {
        Expect("JOINED", timeoutMs);
        Settings = KeyValues(Expect("SETTINGS", timeoutMs));
    }

    // owner only: stage and wins of the next match; returns what the server stored
    public Dictionary<string, string> Set(int stage, int wins, int timeoutMs)
    {
        Send("SET stage=" + stage + " wins=" + wins);
        Settings = KeyValues(Expect("SETTINGS", timeoutMs));
        return Settings;
    }

    // owner only: the gameplay modifiers of the next match ("jump,turbo", "" = none); returns what the server stored
    public Dictionary<string, string> SetMods(string mods, int timeoutMs)
    {
        Send("SET mods=" + mods);
        Settings = KeyValues(Expect("SETTINGS", timeoutMs));
        return Settings;
    }

    // the gameplay modifiers (native/net/wotm_modifiers.hpp): lobby names in canonical order, and what the launcher shows
    public static readonly string[] ModifierNames = { "jump", "fast", "turbo" };
    public static readonly string[] ModifierLabels = { "Super Jump", "Fast Monsters", "Turbo" };

    // WOTM_MODIFIERS / SET mods= list of the modifiers that are on, in canonical order
    public static string ModifierList(bool jump, bool fast, bool turbo)
    {
        bool[] on = { jump, fast, turbo };
        var names = new List<string>();
        for (int i = 0; i < on.Length; ++i) if (on[i]) names.Add(ModifierNames[i]);
        return string.Join(",", names.ToArray());
    }

    // "Super Jump, Turbo" for "jump,turbo"; "no modifiers" for an empty list
    public static string ModifierLabel(string mods)
    {
        var labels = new List<string>();
        foreach (string n in mods.Split(','))
        {
            int i = Array.IndexOf(ModifierNames, n);
            if (i >= 0) labels.Add(ModifierLabels[i]);
        }
        return labels.Count > 0 ? string.Join(", ", labels.ToArray()) : "no modifiers";
    }

    public List<LobbyInfo> List(int timeoutMs)
    {
        Send("LIST");
        var r = new List<LobbyInfo>();
        while (true)
        {
            string l = Expect("", timeoutMs);
            if (l == "END") return r;
            LobbyInfo li = ParseLobby(l);
            if (li != null) r.Add(li);
        }
    }

    public static Dictionary<string, string> KeyValues(string line)
    {
        var kv = new Dictionary<string, string>();
        foreach (string w in line.Split(' '))
        {
            int eq = w.IndexOf('=');
            if (eq > 0) kv[w.Substring(0, eq)] = w.Substring(eq + 1);
        }
        return kv;
    }

    // wotm.exe environment variables for a MATCH line (same as match_env in tools/net_lobby.py)
    public static Dictionary<string, string> MatchEnv(Dictionary<string, string> m, string host)
    {
        int you = int.Parse(m["you"]);
        return new Dictionary<string, string>
        {
            { "WOTM_NET", host + ":" + m["port"] },
            { "WOTM_NETTOKEN", m["token"] },
            { "WOTM_NETNAME", m["names"].Split(',')[you] },
            { "WOTM_MATCH", m["monsters"] + "," + m["stage"] },
            { "WOTM_PLAYERS", m["players"] },
            { "WOTM_FIXEDSTEP", m["step"] },
            { "WOTM_SEED", m["seed"] },
            { "WOTM_KILLTARGET", m.ContainsKey("wins") ? m["wins"] : "0" },
            { "WOTM_MODIFIERS", m.ContainsKey("mods") ? m["mods"] : "" },
            { "WOTM_LOCALPLAYER", (you + 1).ToString() },
        };
    }

    // ---- public server list: the master server (wotm_server --master, native/net/wotm_net_master.hpp) ----
    public const int MasterPort = 7778;

    // SERVER <ip> <port> <protocol> <open lobbies> <waiting lobbies> <players> <max lobbies> <name>; Ping is measured from this PC
    // (milliseconds to open its lobby port), -1 when nothing answers
    public class ServerInfo
    {
        public string Ip, Name;
        public int Port, Protocol, Open, Waiting, Players, MaxLobbies;
        public int Ping = -1;
    }

    public static ServerInfo ParseServer(string line)
    {
        string[] w = line.Split(' ');
        int port, protocol, open, waiting, players, max;
        if (w.Length != 9 || w[0] != "SERVER" || !int.TryParse(w[2], out port) || !int.TryParse(w[3], out protocol) ||
            !int.TryParse(w[4], out open) || !int.TryParse(w[5], out waiting) || !int.TryParse(w[6], out players) ||
            !int.TryParse(w[7], out max))
            return null;
        return new ServerInfo
        {
            Ip = w[1], Port = port, Protocol = protocol, Open = open, Waiting = waiting, Players = players, MaxLobbies = max, Name = w[8]
        };
    }

    // "host" or "host:port" (the port defaults to defaultPort)
    public static void SplitAddress(string s, int defaultPort, out string host, out int port)
    {
        s = s.Trim();
        int colon = s.LastIndexOf(':');
        host = colon > 0 ? s.Substring(0, colon) : s;
        if (colon <= 0 || !int.TryParse(s.Substring(colon + 1), out port) || port <= 0 || port > 65535) port = defaultPort;
    }

    // the master's list (SERVERS ... END), each server pinged from here in parallel; LobbyError when the master doesn't answer
    public static List<ServerInfo> Browse(string master, int timeoutMs)
    {
        string host;
        int port;
        SplitAddress(master, MasterPort, out host, out port);
        var list = new List<ServerInfo>();
        using (var tcp = new TcpClient())
        {
            try
            {
                IAsyncResult ar = tcp.BeginConnect(host, port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(timeoutMs) || !tcp.Connected)
                    throw new LobbyError("no answer from the master server " + host + ":" + port);
                tcp.EndConnect(ar);
                tcp.ReceiveTimeout = timeoutMs;
                NetworkStream s = tcp.GetStream();
                var writer = new StreamWriter(s, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
                var reader = new StreamReader(s, new UTF8Encoding(false));
                writer.WriteLine("SERVERS");
                string l;
                while ((l = reader.ReadLine()) != null && l != "END")
                {
                    ServerInfo si = ParseServer(l);
                    if (si != null) list.Add(si);
                }
            }
            catch (SocketException e) { throw new LobbyError("master server " + host + ":" + port + ": " + e.Message); }
            catch (IOException e) { throw new LobbyError("master server " + host + ":" + port + ": " + e.Message); }
        }
        System.Threading.Tasks.Parallel.ForEach(list, delegate(ServerInfo si) { si.Ping = PingMs(si.Ip, si.Port, 1500); });
        return list;
    }

    // milliseconds to open a TCP connection to host:port, or -1 when nothing answers (offline, or a router that doesn't forward it)
    public static int PingMs(string host, int port, int timeoutMs)
    {
        var watch = System.Diagnostics.Stopwatch.StartNew();
        using (var tcp = new TcpClient())
        {
            try
            {
                IAsyncResult ar = tcp.BeginConnect(host, port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(timeoutMs) || !tcp.Connected) return -1;
                tcp.EndConnect(ar);
                return (int)watch.ElapsedMilliseconds;
            }
            catch (SocketException) { return -1; }
        }
    }

    // the last column of the server list: what stops a match on that server first (version, reachability), else the ping
    public static string ServerStatus(ServerInfo s)
    {
        if (s.Protocol != Protocol) return "needs another version (protocol " + s.Protocol + ")";
        return s.Ping < 0 ? "unreachable" : s.Ping + " ms";
    }

    // player name as the server accepts it: letters, digits, '_' and '-', up to 16
    public static string CleanName(string s)
    {
        var b = new StringBuilder();
        foreach (char c in s)
            if ((char.IsLetterOrDigit(c) && c < 128 || c == '_' || c == '-') && b.Length < 16) b.Append(c);
        return b.ToString();
    }
}
