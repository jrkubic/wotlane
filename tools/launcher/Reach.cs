// ABOUTME: What the host can learn from their own PC about friends reaching wotm_server over the internet: the public address from a
// ABOUTME: STUN server, its verdict against the router's UPnP address, and a TCP self-test through the router. Pure parts tested by LobbyTest.cs.
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

// RFC 5389 binding request/response, just enough to learn the address the internet sees (IPv4).
static class Stun
{
    public static readonly string[] Servers = { "stun.l.google.com:19302", "stun.cloudflare.com:3478", "stun1.l.google.com:19302" };
    static readonly byte[] Cookie = { 0x21, 0x12, 0xA4, 0x42 };

    // type 0x0001 (binding request), length 0, magic cookie, 12-byte transaction id
    public static byte[] Request(byte[] txn)
    {
        var r = new byte[20];
        r[1] = 1;
        Array.Copy(Cookie, 0, r, 4, 4);
        Array.Copy(txn, 0, r, 8, 12);
        return r;
    }

    // The IPv4 address in the binding success response to the request with transaction id txn: XOR-MAPPED-ADDRESS (0x0020) first,
    // MAPPED-ADDRESS (0x0001) otherwise. null for anything else (error response, another transaction, no IPv4 address, truncated).
    public static IPAddress ParseMappedAddress(byte[] reply, byte[] txn)
    {
        if (reply == null || reply.Length < 20 || reply[0] != 0x01 || reply[1] != 0x01) return null;
        for (int i = 0; i < 4; ++i) if (reply[4 + i] != Cookie[i]) return null;
        for (int i = 0; i < 12; ++i) if (reply[8 + i] != txn[i]) return null;
        int end = Math.Min(reply.Length, 20 + ((reply[2] << 8) | reply[3]));
        IPAddress plain = null;
        for (int i = 20; i + 4 <= end; )
        {
            int type = (reply[i] << 8) | reply[i + 1], len = (reply[i + 2] << 8) | reply[i + 3], v = i + 4;
            if (v + len > end) break;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && reply[v + 1] == 1)   // family 1 = IPv4
            {
                var b = new byte[4];
                Array.Copy(reply, v + 4, b, 0, 4);
                if (type == 0x0020)
                {
                    for (int k = 0; k < 4; ++k) b[k] ^= Cookie[k];
                    return new IPAddress(b);
                }
                if (plain == null) plain = new IPAddress(b);
            }
            i = v + len + (4 - len % 4) % 4;
        }
        return plain;
    }

    // One request to every server at once; the first valid answer wins. null when nobody answers within timeoutMs (offline, or UDP
    // out is blocked). Blocks for up to timeoutMs: call it off the UI thread.
    public static IPAddress PublicAddress(int timeoutMs) { return PublicAddress(Servers, timeoutMs); }

    public static IPAddress PublicAddress(string[] servers, int timeoutMs)
    {
        IPAddress found = null;
        int pending = servers.Length;
        var done = new ManualResetEvent(false);
        foreach (string s in servers)
        {
            string server = s;
            new Thread(delegate()
            {
                IPAddress a = Query(server, timeoutMs);
                lock (done) if (a != null && found == null) found = a;
                if (a != null || Interlocked.Decrement(ref pending) == 0) done.Set();
            }) { IsBackground = true }.Start();
        }
        done.WaitOne(timeoutMs + 500);
        lock (done) return found;
    }

    static IPAddress Query(string server, int timeoutMs)
    {
        try
        {
            int colon = server.LastIndexOf(':');
            var txn = new byte[12];
            Array.Copy(Guid.NewGuid().ToByteArray(), txn, 12);
            using (var udp = new UdpClient(AddressFamily.InterNetwork))
            {
                udp.Client.ReceiveTimeout = timeoutMs;
                udp.Connect(server.Substring(0, colon), int.Parse(server.Substring(colon + 1)));
                byte[] req = Request(txn);
                udp.Send(req, req.Length);
                var from = new IPEndPoint(IPAddress.Any, 0);
                return ParseMappedAddress(udp.Receive(ref from), txn);
            }
        }
        catch (Exception) { return null; }
    }
}

// The verdicts, as lines for the host's share box.
static class Reach
{
    // The line about the public address, from what the router said over UPnP (external, or null when the mapping failed) and what the
    // internet sees (stun, or null when no STUN server answered). shareAddress = what friends type (null = none known).
    public static string AddressVerdict(string external, string why, IPAddress stun, int port, out string shareAddress)
    {
        bool routerPublic = external != null && ServerHost.Kind(IPAddress.Parse(external)) == "public";
        string forward = "UPnP opened only the near router: forward port " + port + " (TCP and UDP) on the outer router to the near router's address, " +
            "or have everyone join your Tailscale network.";
        if (stun == null)
        {
            shareAddress = routerPublic ? external : null;
            if (external == null) return "Could not learn the public address (no answer from the STUN servers).";
            if (routerPublic) return "Internet address " + external + " (from the router; could not confirm it from outside: no answer from the STUN servers).";
            return "Your router has no public address (" + external + "): it sits behind another router or the provider's shared address (CGNAT). " + forward;
        }
        shareAddress = stun.ToString();
        if (external == null) return "Internet address " + stun + " (port " + port + " not opened on the router, see the Router line).";
        if (external == shareAddress) return "Internet address " + stun + ", confirmed from outside.";
        if (!routerPublic) return "Your router sits behind another router or the provider's shared address (router " + external + ", internet " + stun + "). " + forward;
        return "The router says " + external + " but the internet sees " + stun + ": there is another router or the provider's shared address in between. " + forward;
    }

    // TCP connect to addr:port, HELLO, one line back: any line, even an ERROR, proves wotm_server answered. Through the router's own
    // public address this is the NAT hairpin test; routers that cannot loop back give "no connection", which proves nothing.
    public static bool PortTest(IPAddress addr, int port, int timeoutMs, out string detail)
    {
        var watch = Stopwatch.StartNew();
        using (var tcp = new TcpClient(AddressFamily.InterNetwork))
        {
            try
            {
                IAsyncResult ar = tcp.BeginConnect(addr, port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(timeoutMs) || !tcp.Connected) { detail = "no connection"; return false; }
                tcp.EndConnect(ar);
                NetworkStream s = tcp.GetStream();
                s.ReadTimeout = timeoutMs;
                byte[] hello = Encoding.ASCII.GetBytes("HELLO 0 porttest\n");
                s.Write(hello, 0, hello.Length);
                var buf = new byte[256];
                if (s.Read(buf, 0, buf.Length) <= 0) { detail = "connected, but no answer"; return false; }
                detail = "answered in " + watch.ElapsedMilliseconds + " ms";
                return true;
            }
            catch (Exception e) { detail = e.Message; return false; }
        }
    }

    public static string PortTestLine(bool reachable, string addr, int port)
    {
        return reachable ? "Port test: friends can reach " + addr + ":" + port + " (tested through your router)."
            : "Port test: no answer at " + addr + ":" + port + " from inside. Some routers cannot test themselves, so ask a friend to try; " +
              "if they also get nothing, forward port " + port + " (TCP and UDP) on your router.";
    }
}
