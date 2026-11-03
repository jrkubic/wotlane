// ABOUTME: Player-as-host for the launcher: runs wotm_server hidden as a child process (optionally announced on a master server),
// ABOUTME: lists the addresses friends can use (internet, Tailscale, LAN) and opens the port via UPnP. Tested by LobbyTest.cs.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;

// The same wotm_server as the dedicated server, as a child of this launcher: it closes along with it. If the port already has a
// server (another launcher, the wotm_server window), uses that one and does not close it.
class ServerHost : IDisposable
{
    public readonly int Port;
    readonly Process proc;
    readonly StreamWriter log;
    readonly List<string> mapped = new List<string>();

    public bool Owned { get { return proc != null; } }
    public bool Running { get { return proc != null ? !proc.HasExited : PortOpen(Port, 500); } }

    ServerHost(int port, Process p, StreamWriter w)
    {
        Port = port;
        proc = p;
        log = w;
    }

    // announce/name: list this server publicly on that master server (wotm_server --announce <master> --name <name>); null = private
    public static ServerHost Start(string exe, int port, string logPath, int timeoutMs, string announce = null, string name = null)
    {
        if (PortOpen(port, 300)) return new ServerHost(port, null, null);
        if (!File.Exists(exe)) throw new LobbyError("could not find " + exe + " (tools\\build_net.bat)");
        StreamWriter w = null;
        if (logPath != null) w = new StreamWriter(logPath, false) { AutoFlush = true };
        string args = "--port " + port;
        if (!string.IsNullOrEmpty(announce))
        {
            args += " --announce " + announce.Trim();
            string n = LobbyClient.CleanName(name ?? "");
            if (n.Length > 0) args += " --name " + n;
        }
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(exe)
        };
        Process p = Process.Start(psi);
        // always read both outputs: with a full pipe the server would hang mid-match
        DataReceivedEventHandler write = delegate(object s, DataReceivedEventArgs e)
        {
            if (e.Data == null || w == null) return;
            try { lock (w) w.WriteLine(e.Data); } catch (ObjectDisposedException) { }   // last lines after Dispose
        };
        p.OutputDataReceived += write;
        p.ErrorDataReceived += write;
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        var h = new ServerHost(port, p, w);
        for (int waited = 0; waited < timeoutMs; waited += 100)
        {
            if (p.HasExited) { h.Dispose(); throw new LobbyError("wotm_server exited while opening port " + port + " (another program on it?)"); }
            if (PortOpen(port, 100)) return h;
            Thread.Sleep(100);
        }
        h.Dispose();
        throw new LobbyError("wotm_server did not open port " + port);
    }

    public static bool PortOpen(int port, int timeoutMs)
    {
        using (var c = new TcpClient())
        {
            try
            {
                IAsyncResult ar = c.BeginConnect(IPAddress.Loopback, port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(timeoutMs)) return false;
                c.EndConnect(ar);
                return c.Connected;
            }
            catch (SocketException) { return false; }
        }
    }

    // "Tailscale" (100.64.0.0/10), "LAN" (private addresses), "public"; null for loopback and link-local (useless to a friend)
    public static string Kind(IPAddress a)
    {
        if (a.AddressFamily != AddressFamily.InterNetwork) return null;
        byte[] b = a.GetAddressBytes();
        if (b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] == 0) return null;
        if (b[0] == 100 && (b[1] & 0xC0) == 64) return "Tailscale";
        if (b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168)) return "LAN";
        return "public";
    }

    // lines to show the host, ordered by how likely each is to work for a friend: internet (UPnP), Tailscale, LAN
    public static List<string> ShareLines(IEnumerable<IPAddress> local, string external, int port)
    {
        var lines = new List<string>();
        if (!string.IsNullOrEmpty(external)) lines.Add("Internet: " + external + ":" + port);
        foreach (string kind in new[] { "Tailscale", "public", "LAN" })
            foreach (IPAddress a in local)
                if (Kind(a) == kind) lines.Add((kind == "public" ? "Internet" : kind) + ": " + a + ":" + port);
        return lines.Distinct().ToList();
    }

    // Tailscale from any adapter; the others only from adapters with a gateway (the WSL/Hyper-V one has none, useless to a friend)
    public static List<IPAddress> LocalAddresses()
    {
        var list = new List<IPAddress>();
        foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != OperationalStatus.Up) continue;
            IPInterfaceProperties ip = ni.GetIPProperties();
            foreach (UnicastIPAddressInformation u in ip.UnicastAddresses)
                if (Kind(u.Address) == "Tailscale" || (Kind(u.Address) != null && HasGateway(ip))) list.Add(u.Address);
        }
        return list;
    }

    static bool HasGateway(IPInterfaceProperties ip)
    {
        return ip.GatewayAddresses.Any(g => g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any));
    }

    // the LAN address of the adapter with a gateway (the router forwards the port to it); null if there is none
    public static IPAddress RouterFacingAddress()
    {
        foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != OperationalStatus.Up) continue;
            IPInterfaceProperties ip = ni.GetIPProperties();
            if (!HasGateway(ip)) continue;
            foreach (UnicastIPAddressInformation u in ip.UnicastAddresses)
                if (Kind(u.Address) == "LAN") return u.Address;
        }
        return null;
    }

    // Opens the port (TCP and UDP) on the router through Windows UPnP (HNetCfg.NATUPnP). Returns the external address the router reports
    // (public or not: Reach.AddressVerdict judges it), or null with the reason in why. Takes a few seconds: call it off the UI thread.
    public string MapUpnp(out string why)
    {
        why = null;
        IPAddress lan = RouterFacingAddress();
        if (lan == null) { why = "no LAN address with a router"; return null; }
        try
        {
            dynamic nat = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.NATUPnP"));
            dynamic maps = nat.StaticPortMappingCollection;
            if (maps == null) { why = "the router does not offer UPnP (or it is off)"; return null; }
            string external = null;
            foreach (string proto in new[] { "TCP", "UDP" })
            {
                dynamic m = maps.Add(Port, proto, Port, lan.ToString(), true, "WotM wotm_server");
                lock (mapped) mapped.Add(proto);
                if (m != null && external == null) external = m.ExternalIPAddress;
            }
            if (string.IsNullOrEmpty(external)) { why = "the router did not report its address"; return null; }
            return external;
        }
        catch (Exception e) { why = "UPnP failed: " + e.Message; return null; }
    }

    void UnmapUpnp()
    {
        string[] protos;
        lock (mapped) { protos = mapped.ToArray(); mapped.Clear(); }
        if (protos.Length == 0) return;
        try
        {
            dynamic nat = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.NATUPnP"));
            dynamic maps = nat.StaticPortMappingCollection;
            if (maps != null) foreach (string proto in protos) maps.Remove(Port, proto);
        }
        catch (Exception) { }
    }

    public void Dispose()
    {
        UnmapUpnp();
        if (proc != null)
        {
            try { if (!proc.HasExited) { proc.Kill(); proc.WaitForExit(2000); } } catch (Exception) { }
            proc.Dispose();
        }
        if (log != null) lock (log) log.Dispose();
    }
}
