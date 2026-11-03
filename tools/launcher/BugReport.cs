// ABOUTME: The bug report zip a friend sends instead of five separate files: report.txt (build, PC, GPU and driver, mods, what the
// ABOUTME: launcher knows about the network) plus the game, server and launcher logs and settings. Writing is tested by LobbyTest.cs.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Management;
using System.Text;

// One file to put in the zip: its name inside the zip and where it is; longer files go in from the end (the newest lines).
class ReportFile
{
    public string Name, Path;
    public long MaxBytes = 4 << 20;
    public ReportFile(string name, string path) { Name = name; Path = path; }
}

static class BugReport
{
    // What the PC is: Windows, CPU, memory, every video adapter with its driver version and date. WMI; a query that fails becomes
    // one line saying so, the report still comes out.
    public static List<KeyValuePair<string, string>> SystemInfo()
    {
        var info = new List<KeyValuePair<string, string>>();
        Query(info, "Windows", "Win32_OperatingSystem", "Caption", "Version", "OSArchitecture");
        Query(info, "CPU", "Win32_Processor", "Name", "NumberOfCores", "NumberOfLogicalProcessors");
        Query(info, "Memory", "Win32_ComputerSystem", "TotalPhysicalMemory");
        Query(info, "GPU", "Win32_VideoController", "Name", "DriverVersion", "DriverDate", "AdapterRAM",
            "CurrentHorizontalResolution", "CurrentVerticalResolution", "CurrentRefreshRate");
        return info;
    }

    static void Query(List<KeyValuePair<string, string>> info, string label, string cls, params string[] props)
    {
        try
        {
            using (var s = new ManagementObjectSearcher("select " + string.Join(",", props) + " from " + cls))
                foreach (ManagementObject o in s.Get())
                {
                    var parts = new List<string>();
                    foreach (string p in props)
                    {
                        object v = o[p];
                        if (v == null) continue;
                        string t = v.ToString();
                        if (p == "DriverDate" && t.Length >= 8) t = t.Substring(0, 4) + "-" + t.Substring(4, 2) + "-" + t.Substring(6, 2);
                        else if (p == "TotalPhysicalMemory" || p == "AdapterRAM") t = (Convert.ToUInt64(v) >> 20) + " MB";
                        parts.Add(p == props[0] ? t : p + " " + t);
                    }
                    info.Add(new KeyValuePair<string, string>(label, string.Join(", ", parts.ToArray())));
                }
        }
        catch (Exception e) { info.Add(new KeyValuePair<string, string>(label, "(not available: " + e.Message + ")")); }
    }

    // report.txt: one "key: value" line per fact, then the files that went in and the ones that were missing.
    public static string ReportText(IEnumerable<KeyValuePair<string, string>> info, IEnumerable<string> included, IEnumerable<string> missing)
    {
        var sb = new StringBuilder();
        sb.AppendLine("wotlane bug report, " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + " (local time)");
        foreach (KeyValuePair<string, string> kv in info) sb.AppendLine(kv.Key + ": " + kv.Value);
        sb.AppendLine();
        sb.AppendLine("Files in this zip:");
        foreach (string f in included) sb.AppendLine("  " + f);
        foreach (string f in missing) sb.AppendLine("  (missing) " + f);
        return sb.ToString();
    }

    // Writes the zip: report.txt plus the files that exist (the last MaxBytes of each; opened for shared reading, the game or the
    // server may still be writing them). Returns the names of the files that were missing or unreadable.
    public static List<string> Write(string zipPath, IEnumerable<KeyValuePair<string, string>> info, IList<ReportFile> files)
    {
        var included = new List<string>();
        var missing = new List<string>();
        if (File.Exists(zipPath)) File.Delete(zipPath);
        using (ZipArchive zip = ZipFile.Open(zipPath, ZipArchiveMode.Create))
        {
            foreach (ReportFile f in files)
            {
                if (f.Path == null || !File.Exists(f.Path)) { missing.Add(f.Name); continue; }
                try
                {
                    long length = new FileInfo(f.Path).Length;
                    using (var src = new FileStream(f.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    using (Stream dst = zip.CreateEntry(f.Name, CompressionLevel.Optimal).Open())
                    {
                        if (length > f.MaxBytes) src.Seek(-f.MaxBytes, SeekOrigin.End);
                        src.CopyTo(dst);
                    }
                    string cap = f.MaxBytes >= (1 << 20) ? (f.MaxBytes >> 20) + " MB" : (f.MaxBytes >> 10) + " KB";
                    included.Add(f.Name + (length > f.MaxBytes ? " (last " + cap + " of " + (length >> 10) + " KB)" : ""));
                }
                catch (IOException e) { missing.Add(f.Name + " (" + e.Message + ")"); }
            }
            using (var w = new StreamWriter(zip.CreateEntry("report.txt", CompressionLevel.Optimal).Open(), new UTF8Encoding(false)))
                w.Write(ReportText(info, included, missing));
        }
        return missing;
    }
}
