// ABOUTME: Reads the Windows Firewall rules that name wotm_server.exe (HNetCfg.FwPolicy2) and tells the host whether friends can
// ABOUTME: reach it on the current network; Allow adds the rule through an elevated PowerShell. The verdict is pure, tested by LobbyTest.cs.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;

// One inbound firewall rule that names the server's exe, as the firewall stores it.
class FirewallRule
{
    public bool Allow, Enabled;
    public int Profiles;    // bit mask: 1 domain, 2 private, 4 public (0x7FFFFFFF = all)
    public int Protocol;    // 6 TCP, 17 UDP, 256 any
}

static class FirewallCheck
{
    public const int Domain = 1, Private = 2, Public = 4;
    public const int Tcp = 6, Udp = 17, AnyProtocol = 256;
    static readonly int[] Bits = { Domain, Private, Public };

    public static string ProfileName(int bit) { return bit == Domain ? "Domain" : bit == Private ? "Private" : bit == Public ? "Public" : "?"; }

    // The inbound rules that name exe, plus which profiles are in use now and which of them have the firewall on.
    // Throws when the firewall service does not answer (the caller reports that and moves on).
    public static List<FirewallRule> RulesFor(string exe, out int currentProfiles, out int enabledProfiles)
    {
        dynamic policy = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2"));
        currentProfiles = (int)policy.CurrentProfileTypes;
        enabledProfiles = 0;
        foreach (int bit in Bits)
            if ((bool)policy.FirewallEnabled(bit)) enabledProfiles |= bit;
        string want = Path.GetFullPath(exe);
        var rules = new List<FirewallRule>();
        foreach (dynamic r in policy.Rules)
        {
            if ((int)r.Direction != 1) continue;   // NET_FW_RULE_DIR_IN
            string app = r.ApplicationName as string;
            if (app == null || !string.Equals(Environment.ExpandEnvironmentVariables(app), want, StringComparison.OrdinalIgnoreCase)) continue;
            rules.Add(new FirewallRule { Allow = (int)r.Action == 1, Enabled = (bool)r.Enabled, Profiles = (int)r.Profiles, Protocol = (int)r.Protocol });
        }
        return rules;
    }

    // One line for the host about the profiles in use. problem = friends cannot get in until the rule changes. Block rules win over
    // allow rules, as in the firewall itself; an allow rule is only complete with TCP and UDP (the lobby and the match).
    public static string Verdict(IList<FirewallRule> rules, int currentProfiles, int enabledProfiles, out bool problem)
    {
        problem = false;
        var names = new List<string>();
        var blocked = new List<string>();
        var missing = new List<string>();
        var off = new List<string>();
        foreach (int bit in Bits)
        {
            if ((currentProfiles & bit) == 0) continue;
            names.Add(ProfileName(bit));
            if ((enabledProfiles & bit) == 0) { off.Add(ProfileName(bit)); continue; }
            bool block = false, tcp = false, udp = false;
            foreach (FirewallRule r in rules)
            {
                if (!r.Enabled || (r.Profiles & bit) == 0) continue;
                if (!r.Allow) block = true;
                else
                {
                    if (r.Protocol == Tcp || r.Protocol == AnyProtocol) tcp = true;
                    if (r.Protocol == Udp || r.Protocol == AnyProtocol) udp = true;
                }
            }
            if (block) blocked.Add(ProfileName(bit));
            else if (!(tcp && udp)) missing.Add(ProfileName(bit));
        }
        if (names.Count == 0) return "Windows Firewall: no network profile is active.";
        string net = "your network (" + string.Join(" + ", names.ToArray()) + ")";
        if (blocked.Count > 0)
        {
            problem = true;
            return "Windows Firewall BLOCKS wotm_server on " + net + " (someone pressed Cancel on the Windows prompt): " +
                "friends get no answer even with the port open.";
        }
        if (missing.Count > 0)
        {
            bool elsewhere = false;
            foreach (FirewallRule r in rules) if (r.Enabled && r.Allow) elsewhere = true;
            problem = true;
            return "Windows Firewall has no rule allowing wotm_server (TCP and UDP) on " + net +
                (elsewhere ? "; the existing rule covers other network types only" : "") + ": friends get no answer even with the port open.";
        }
        if (off.Count == names.Count) return "Windows Firewall is off on " + net + ".";
        return "Windows Firewall: wotm_server is allowed on " + net + ".";
    }

    // Adds an inbound allow rule for exe on every profile, TCP and UDP, after removing the rules the Windows prompt made for it,
    // through an elevated PowerShell (Windows asks for permission). false with why when refused or failed. Slow: call off the UI thread.
    public static bool Allow(string exe, out string why)
    {
        why = null;
        string p = Path.GetFullPath(exe).Replace("'", "''");
        string script =
            "$p='" + p + "'; " +
            "Get-NetFirewallApplicationFilter | Where-Object { [Environment]::ExpandEnvironmentVariables($_.Program) -ieq $p } | " +
            "Get-NetFirewallRule | Remove-NetFirewallRule -ErrorAction SilentlyContinue; " +
            "New-NetFirewallRule -DisplayName 'WotM wotm_server' -Direction Inbound -Program $p -Action Allow -Profile Any -Enabled True | Out-Null; " +
            "exit 0";
        var psi = new ProcessStartInfo("powershell.exe", "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \"" + script + "\"")
        {
            UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden
        };
        try
        {
            using (Process proc = Process.Start(psi))
            {
                if (!proc.WaitForExit(120000)) { why = "PowerShell did not finish"; return false; }
                if (proc.ExitCode != 0) { why = "PowerShell ended with code " + proc.ExitCode; return false; }
                return true;
            }
        }
        catch (Win32Exception e) { why = e.NativeErrorCode == 1223 ? "you refused the Windows permission prompt" : e.Message; return false; }
        catch (Exception e) { why = e.Message; return false; }
    }
}
