# Host Reachability Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** When a player presses Host on this PC, the share box also says what address the internet sees for them, whether the router's UPnP address agrees with it, and whether a TCP connection through the router to that address reaches wotm_server.

**Architecture:** One new file `tools/launcher/Reach.cs` with two static classes: `Stun` (binding request, pure reply parser, parallel query of three public servers) and `Reach` (pure address verdict, TCP port self-test, the port-test line). `WotmLauncher.cs` runs STUN beside the UPnP mapping on the existing router thread, feeds the verdict into the share box, and runs the port test once the firewall verdict is good. `ServerHost.MapUpnp` stops judging the router's address itself and hands it back raw so the verdict can compare it with STUN.

**Tech Stack:** C# 5 on .NET Framework (Windows' built-in `csc`, no NuGet), `UdpClient`/`TcpClient`, the launcher's own test harness `tools/launcher/LobbyTest.cs` (run with `cmd /c tools\launcher\test.bat` from PowerShell; it needs `native\net\build\wotm_server.exe` from `tools\build_net.bat`).

Spec: `docs/superpowers/specs/2026-11-03-host-reachability-design.md`. One amendment made here: the verdict returns no `problem` flag (nothing in the share box can show it; the text carries the advice).

Conventions: every file starts with two `// ABOUTME:` comment lines; comments in English; never commit `WotM_Launcher.exe`. Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Run the build from PowerShell: `cmd /c tools\launcher\build.bat` (Git Bash mangles the path).

---

### Task 1: STUN request and reply parser (pure)

**Files:**
- Create: `tools/launcher/Reach.cs`
- Modify: `tools/launcher/build.bat`, `tools/launcher/test.bat` (add `"%~dp0Reach.cs"` after `"%~dp0FirewallCheck.cs"` in the csc source list; test.bat's ABOUTME list too)
- Test: `tools/launcher/LobbyTest.cs` (new `Run("stun_parse", ...)` before `Run("firewall_verdict", ...)`)

**Step 1: Write the failing test**

```csharp
            Run("stun_parse", delegate
            {
                byte[] txn = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
                byte[] req = Stun.Request(txn);
                Check(req.Length == 20 && req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 0 && req[4] == 0x21 && req[7] == 0x42 && req[19] == 12,
                    "binding request: type 1, length 0, cookie, transaction id");
                Func<byte[], byte[][], byte[]> reply = delegate(byte[] t, byte[][] attrs)
                {
                    var body = new List<byte>();
                    foreach (byte[] at in attrs) body.AddRange(at);
                    var r = new List<byte> { 0x01, 0x01, (byte)(body.Count >> 8), (byte)body.Count, 0x21, 0x12, 0xA4, 0x42 };
                    r.AddRange(t); r.AddRange(body);
                    return r.ToArray();
                };
                // XOR-MAPPED-ADDRESS 203.0.113.9:7777 (address and port XORed with the cookie), MAPPED-ADDRESS 198.51.100.7:7777
                byte[] xorAttr = { 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, (byte)(0x1E ^ 0x21), (byte)(0x61 ^ 0x12), (byte)(203 ^ 0x21), (byte)(0 ^ 0x12), (byte)(113 ^ 0xA4), (byte)(9 ^ 0x42) };
                byte[] plainAttr = { 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x1E, 0x61, 198, 51, 100, 7 };
                byte[] software = { 0x80, 0x22, 0x00, 0x05, (byte)'h', (byte)'e', (byte)'l', (byte)'l', (byte)'o', 0, 0, 0 };   // 5 bytes + 3 of padding
                byte[] v6 = { 0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
                IPAddress a = Stun.ParseMappedAddress(reply(txn, new[] { software, xorAttr }), txn);
                Check(a != null && a.ToString() == "203.0.113.9", "XOR-MAPPED-ADDRESS after a padded attribute");
                a = Stun.ParseMappedAddress(reply(txn, new[] { plainAttr }), txn);
                Check(a != null && a.ToString() == "198.51.100.7", "MAPPED-ADDRESS accepted");
                a = Stun.ParseMappedAddress(reply(txn, new[] { plainAttr, xorAttr }), txn);
                Check(a != null && a.ToString() == "203.0.113.9", "XOR form preferred over the plain one");
                Check(Stun.ParseMappedAddress(reply(txn, new[] { v6 }), txn) == null, "IPv6 mapped address is skipped");
                Check(Stun.ParseMappedAddress(reply(txn, new[] { software }), txn) == null, "no address attribute");
                byte[] other = (byte[])txn.Clone(); other[11] = 99;
                Check(Stun.ParseMappedAddress(reply(other, new[] { xorAttr }), txn) == null, "another transaction id");
                byte[] bad = reply(txn, new[] { xorAttr }); bad[5] = 0;
                Check(Stun.ParseMappedAddress(bad, txn) == null, "wrong magic cookie");
                byte[] err = reply(txn, new[] { xorAttr }); err[1] = 0x11;   // 0x0111 = binding error response
                Check(Stun.ParseMappedAddress(err, txn) == null, "error response");
                byte[] cut = new byte[26]; Array.Copy(reply(txn, new[] { xorAttr }), cut, 26);
                Check(Stun.ParseMappedAddress(cut, txn) == null, "truncated attribute");
                Check(Stun.ParseMappedAddress(new byte[5], txn) == null && Stun.ParseMappedAddress(null, txn) == null, "too short or null");
            });
```

**Step 2: Run the tests to verify they fail**

Run (PowerShell): `cmd /c tools\launcher\test.bat`
Expected: compile error `The name 'Stun' does not exist` (the test build fails before running).

**Step 3: Write the minimal implementation**

`tools/launcher/Reach.cs`:

```csharp
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
}
```

Add `"%~dp0Reach.cs"` to both bat files.

**Step 4: Run the tests to verify they pass**

Run: `cmd /c tools\launcher\test.bat`
Expected: `ok      stun_parse` and the final line `... 0 failure(s)`.

**Step 5: Commit**

```
git add tools/launcher/Reach.cs tools/launcher/LobbyTest.cs tools/launcher/build.bat tools/launcher/test.bat
git commit -m "launcher: STUN binding request and reply parser (Reach.cs)"
```

---

### Task 2: live STUN query

**Files:**
- Modify: `tools/launcher/Reach.cs` (inside `Stun`)
- Test: `tools/launcher/LobbyTest.cs` (`Run("stun_live", ...)` after `stun_parse`)

**Step 1: Write the failing test**

```csharp
            Run("stun_live", delegate
            {
                IPAddress a = Stun.PublicAddress(2500);
                Check(a != null && ServerHost.Kind(a) == "public", "a public address comes back (" + (a == null ? "none" : a.ToString()) + ")");
                Check(Stun.PublicAddress(new[] { "127.0.0.1:9" }, 300) == null, "a silent server gives null within the timeout");
            });
```

**Step 2: Run to verify it fails:** compile error `'Stun' does not contain a definition for 'PublicAddress'`.

**Step 3: Implementation** (append inside `Stun`):

```csharp
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
```

**Step 4: Run:** `ok      stun_live`.

**Step 5: Commit:** `git commit -am "launcher: the public address from three STUN servers, first answer wins"`

---

### Task 3: the address verdict, and MapUpnp hands the router's address back raw

**Files:**
- Modify: `tools/launcher/Reach.cs` (new `static class Reach`)
- Modify: `tools/launcher/ServerHost.cs` (`MapUpnp`)
- Test: `tools/launcher/LobbyTest.cs` (`Run("address_verdict", ...)`)

**Step 1: Write the failing test**

```csharp
            Run("address_verdict", delegate
            {
                IPAddress inet = IPAddress.Parse("203.0.113.9");
                string share;
                string v = Reach.AddressVerdict("203.0.113.9", null, inet, 7777, out share);
                Check(v.Contains("confirmed from outside") && share == "203.0.113.9", "router and internet agree");
                v = Reach.AddressVerdict("192.168.0.5", null, inet, 7777, out share);
                Check(v.Contains("behind another router") && v.Contains("192.168.0.5") && v.Contains("203.0.113.9") && v.Contains("forward port 7777") && share == "203.0.113.9",
                    "router has a private address: double NAT");
                v = Reach.AddressVerdict("100.70.1.2", null, inet, 7777, out share);
                Check(v.Contains("behind another router") && share == "203.0.113.9", "router has a carrier (CGNAT) address");
                v = Reach.AddressVerdict("198.51.100.7", null, inet, 7777, out share);
                Check(v.Contains("router says 198.51.100.7") && v.Contains("internet sees 203.0.113.9") && share == "203.0.113.9", "two different public addresses");
                v = Reach.AddressVerdict(null, "UPnP failed: timeout", inet, 7777, out share);
                Check(v.Contains("203.0.113.9") && v.Contains("not opened") && share == "203.0.113.9", "UPnP failed, STUN answered");
                v = Reach.AddressVerdict("203.0.113.9", null, null, 7777, out share);
                Check(v.Contains("could not confirm") && share == "203.0.113.9", "no STUN answer, router public: keep the router's address");
                v = Reach.AddressVerdict("192.168.0.5", null, null, 7777, out share);
                Check(v.Contains("no public address") && share == null, "no STUN answer, router private");
                v = Reach.AddressVerdict(null, "UPnP failed: timeout", null, 7777, out share);
                Check(v.Contains("Could not learn") && share == null, "nothing known");
            });
```

**Step 2: Run to verify it fails:** compile error (`Reach` missing).

**Step 3: Implementation.** Append to `Reach.cs`:

```csharp
// The verdicts, as lines for the host's share box.
static class Reach
{
    // The line about the public address, from what the router said over UPnP (external, or null when the mapping failed) and what the
    // internet sees (stun, or null when no STUN server answered). shareAddress = what friends type (null = none known).
    public static string AddressVerdict(string external, string why, IPAddress stun, int port, out string shareAddress)
    {
        string routerKind = external == null ? null : ServerHost.Kind(IPAddress.Parse(external));
        bool routerPublic = routerKind == "public";
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
}
```

In `ServerHost.MapUpnp`: delete the block

```csharp
            if (string.IsNullOrEmpty(external) || Kind(IPAddress.Parse(external)) != "public")
            {
                why = "the router has no public address (" + (external ?? "none") + "): likely CGNAT, use Tailscale";
                return null;
            }
```

and replace it with `if (string.IsNullOrEmpty(external)) { why = "the router did not report its address"; return null; }`; the method comment becomes "Returns the external address the router reports (public or not: Reach.AddressVerdict judges it), or null with the reason in why."

**Step 4: Run:** `ok      address_verdict`, and `addresses_for_friends` still ok.

**Step 5: Commit:** `git commit -am "launcher: verdict on the public address (router's UPnP address vs STUN)"`

---

### Task 4: the TCP port self-test and its line

**Files:**
- Modify: `tools/launcher/Reach.cs` (inside `Reach`)
- Test: `tools/launcher/LobbyTest.cs` (`Run("port_test", ...)`; `port` is the test server's port, in scope)

**Step 1: Write the failing test**

```csharp
            Run("port_test", delegate
            {
                string detail;
                Check(Reach.PortTest(IPAddress.Loopback, port, 2000, out detail) && detail.EndsWith(" ms"), "the test server answers HELLO (" + detail + ")");
                Check(!Reach.PortTest(IPAddress.Loopback, 1, 1000, out detail), "a closed port does not (" + detail + ")");
                Check(Reach.PortTestLine(true, "203.0.113.9", 7777).StartsWith("Port test: friends can reach 203.0.113.9:7777"), "positive line");
                string neg = Reach.PortTestLine(false, "203.0.113.9", 7777);
                Check(neg.Contains("no answer at 203.0.113.9:7777") && neg.Contains("ask a friend") && neg.Contains("forward port 7777"), "inconclusive line");
            });
```

**Step 2: Run to verify it fails:** compile error.

**Step 3: Implementation** (inside `Reach`):

```csharp
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
```

**Step 4: Run:** `ok      port_test`.

**Step 5: Commit:** `git commit -am "launcher: TCP port self-test through the router"`

---

### Task 5: wire it into the launcher

**Files:**
- Modify: `tools/launcher/WotmLauncher.cs` (fields near `hostedRouter`; `NetHost`; `MapRouterPort`; `CheckFirewall`'s BeginInvoke; `ShowShare`; `netShare` height)
- Modify: `tools/launcher/LobbyClient.cs` (the connection timeout message)

**Step 1: fields** (after `string hostedRouter = "";`):

```csharp
    // what the internet sees (Reach.cs): the public address line, the address itself, the port self-test line
    string hostedAddress = "", hostedPortTest = "";
    System.Net.IPAddress hostedStun;
    bool portTestBusy, portTestDone;
```

**Step 2: NetHost** reset, inside the `if (hosted == null || ...)` block right after `hostedPublic = ...;`:

```csharp
            hostedAddress = hostedPortTest = "";
            hostedStun = null;
            portTestDone = false;
```

**Step 3: MapRouterPort.** Replace the thread body so STUN runs beside UPnP and the verdict fills the lines:

```csharp
    void MapRouterPort(ServerHost h)
    {
        new System.Threading.Thread(delegate()
        {
            System.Net.IPAddress stun = null;
            var stunThread = new System.Threading.Thread(delegate() { stun = Stun.PublicAddress(2500); }) { IsBackground = true };
            stunThread.Start();
            string why;
            string external = h.MapUpnp(out why);
            stunThread.Join(3500);
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    if (hosted != h) return;
                    string share;
                    hostedAddress = Reach.AddressVerdict(external, why, stun, h.Port, out share);
                    hostedStun = stun;
                    hostedLines = ServerHost.ShareLines(ServerHost.LocalAddresses(), share, h.Port);
                    if (external != null) hostedRouter = "Router: port " + h.Port + " opened for TCP and UDP (UPnP).";
                    else
                    {
                        System.Net.IPAddress lan = ServerHost.RouterFacingAddress();
                        hostedRouter = "Router: " + why + ". For friends over the internet, forward port " + h.Port + " (TCP and UDP) to " +
                            (lan != null ? lan.ToString() : "this PC") + " in your router (friends then use the Internet address below), " +
                            "or have everyone join your Tailscale network.";
                    }
                    ShowShare();
                    TryPortTest();
                });
            }
            catch (InvalidOperationException) { }   // launcher already closed
        }) { IsBackground = true }.Start();
    }

    // The self-test through the router, once the public address is known and Windows Firewall lets wotm_server answer; a success is
    // final for this server, a failure is tried again when the firewall verdict turns good (CheckFirewall calls here).
    void TryPortTest()
    {
        if (hosted == null || hostedStun == null || portTestBusy || portTestDone || hostedFirewallProblem || hostedFirewall.Length == 0) return;
        portTestBusy = true;
        ServerHost h = hosted;
        System.Net.IPAddress addr = hostedStun;
        new System.Threading.Thread(delegate()
        {
            string detail;
            bool ok = Reach.PortTest(addr, h.Port, 3000, out detail);
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    portTestBusy = false;
                    if (hosted != h) return;
                    portTestDone = ok;
                    hostedPortTest = Reach.PortTestLine(ok, addr.ToString(), h.Port);
                    ShowShare();
                });
            }
            catch (InvalidOperationException) { }
        }) { IsBackground = true }.Start();
    }
```

**Step 4: CheckFirewall:** in its BeginInvoke, after `ShowShare();` add `TryPortTest();`.

**Step 5: ShowShare:** after `l.Add(hostedRouter);` add

```csharp
        if (hostedAddress.Length > 0) l.Add(hostedAddress);
        if (hostedPortTest.Length > 0) l.Add(hostedPortTest);
```

and raise `netShare`'s `Height = 92` to `Height = 124`.

**Step 6: LobbyClient.cs:** the connection timeout message becomes `"no answer from " + host + ":" + port + " (is wotm_server running? is the TCP port open? ask the host what the Port test line in their launcher says)"`.

**Step 7: Build and test.** `cmd /c tools\launcher\build.bat` then `cmd /c tools\launcher\test.bat`: 0 failures. Copy `WotM_Launcher.exe` to `C:\Projects\wotlane\` and run the scratchpad `win32_play_test.ps1`, extended first with:

```powershell
# 1c. the share box: public address confirmed and the port test positive (this router loops back)
$share = WaitFor 20 { foreach ($t in Texts $win) { if ($t -match '(?m)^Port test: ') { $t } } }
if ($share -and $share -match '(?m)^Internet address \d+\.\d+\.\d+\.\d+, confirmed from outside\.') { Say "address line ok" } else { Fail "no confirmed Internet address line"; if ($share) { Say "  box: $share" } }
if ($share -and $share -match '(?m)^Port test: friends can reach \d+\.\d+\.\d+\.\d+:7777') { Say "port test positive" } else { Fail "port test not positive"; if ($share) { Say "  box: $share" } }
```

Expected: `failures: 0`.

**Step 8: Commit:** `git commit -am "launcher: Host on this PC shows the address the internet sees and tests the port through the router"`

---

### Task 6: docs and TODO

**Files:**
- Modify: `docs/en/ONLINE.md` (section 2 "From the launcher" bullet; "Troubleshooting" bullet for "sem resposta do servidor")
- Modify: `tools/friends/README.txt` (step 3.4 and Problems)
- Modify: `TODO.md`, `docs/en/TODO.md` (the connection report leaves)

ONLINE.md, "From the launcher" bullet, after the UPnP sentence: "It also asks a public STUN server (one UDP packet) which address the internet sees for you and says whether the router's address agrees: when it does not, you are behind a second router or the provider's shared address and UPnP opened only the near router. Once Windows Firewall is in order it connects to your own public address through the router and reports **Port test: friends can reach ...**; routers that cannot loop back give an inconclusive line, and a friend's try settles it."

Troubleshooting bullet: add "Host: read the Internet address and Port test lines in the box."

friends README: step 3.4 gains the same two lines in plain words; in Problems, the "compare the Internet address with what is my ip" advice becomes "read the Internet address and Port test lines in the box: they say whether the internet reaches your port and what to do if not."

Run `python tools/doclint.py` (exit 0), commit: `docs: host reachability lines; the connection report leaves the TODO`.

### Task 7: ship

Push `host-reachability` to the fork; open PR #2 against `wotlane/mac` (title "launcher: the host sees the address the internet sees and tests the port through the router"; body: what, how tested, that it includes PR #1's commits until that merges; ends with the Claude Code line). Update the memory notes.
