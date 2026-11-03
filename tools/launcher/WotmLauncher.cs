// Launcher for the live game: a window with checkboxes for the environment flags (WOTM_*, PS2X_*) and a button that runs the
// runtime's run_live.ps1 with them. To add a flag, add a line to Flags. The state is kept in WotM_Launcher.ini, next to the
// executable. Build: tools\launcher\build.bat (.NET Framework csc, C# 5).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

class Flag
{
    public string Group, Key, Label, Tip;
    public bool Default;
    public Flag(string group, string key, bool def, string label, string tip)
    {
        Group = group; Key = key; Default = def; Label = label; Tip = tip;
    }
}

// GroupBox with border and title drawn here in the dark theme: with visual styles the title takes the theme color, ignoring ForeColor.
class DarkGroupBox : GroupBox
{
    protected override void OnPaint(PaintEventArgs e)
    {
        if (!Launcher.Dark) { base.OnPaint(e); return; }
        Graphics g = e.Graphics;
        Size sz = TextRenderer.MeasureText(g, Text, Font);
        int y = sz.Height / 2;
        g.Clear(BackColor);
        using (var pen = new Pen(Color.FromArgb(85, 85, 85))) g.DrawRectangle(pen, 0, y, Width - 1, Height - y - 1);
        var r = new Rectangle(7, 0, sz.Width, sz.Height);
        using (var b = new SolidBrush(BackColor)) g.FillRectangle(b, r);
        TextRenderer.DrawText(g, Text, Font, r, ForeColor);
    }
}

// CheckBox drawn here in the dark theme (dark box; when checked, blue with a white tick): the Windows drawing has no dark version
// that respects the text color.
class DarkCheckBox : CheckBox
{
    bool hover;
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        if (!Launcher.Dark) { base.OnPaint(e); return; }
        Graphics g = e.Graphics;
        g.Clear(BackColor);
        const int box = 14;
        var r = new Rectangle(0, (Height - box) / 2, box, box);
        Color fill, border;
        if (Checked)
        {
            fill = Enabled ? (hover ? Color.FromArgb(120, 185, 255) : Color.FromArgb(96, 165, 250)) : Color.FromArgb(70, 90, 115);
            border = fill;
        }
        else
        {
            fill = Color.FromArgb(45, 45, 45);
            border = !Enabled ? Color.FromArgb(75, 75, 75) : (hover ? Color.FromArgb(200, 200, 200) : Color.FromArgb(150, 150, 150));
        }
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        using (var b = new SolidBrush(fill)) g.FillRectangle(b, r);
        using (var p = new Pen(border)) g.DrawRectangle(p, r.X, r.Y, box - 1, box - 1);
        if (Checked)
            using (var p = new Pen(Enabled ? Color.FromArgb(20, 20, 20) : Color.FromArgb(150, 150, 150), 2f))
                g.DrawLines(p, new[] { new PointF(r.X + 3f, r.Y + 7f), new PointF(r.X + 6f, r.Y + 10f), new PointF(r.X + 11f, r.Y + 4f) });
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
        var tr = new Rectangle(box + 4, 0, Width - box - 4, Height);
        TextRenderer.DrawText(g, Text, Font, tr, Enabled ? ForeColor : Color.FromArgb(120, 120, 120),
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
    }
}

class Launcher : Form
{
    // Keys starting with ':' are run_live.ps1 parameters; the others become environment variables (=1 when checked, removed when not).
    // Diagnostics with no checkbox still work via their variable (WOTM_RATEMIN=0, WOTM_VU0_INTERP, PS2X_PAD_DEADZONE; table in NATIVO.md).
    // Checkboxes of closed investigations are gone (holes in monsters: NOSKIN/NOVCOL/OPAQUE/ONEPASS; self-turning: TURNLOG/PS2X_PADLOG).
    static readonly Flag[] Flags = {
        new Flag("Run", ":Auto", false, "Scripted input up to a stage", "-Auto 1: a scripted pad plays up to a stage (~50 s). Unchecked: you play with the keyboard."),
        new Flag("Run", "WOTM_FOTOS", false, "Debug photos (F7 pauses, F6 steps, F8 photo with a note)", "F8 saves the frame (a VU1 vs native check snapshot, ~40 MB), a PNG of the window and your note in <runtime>\\fotos\\<date>. Delete the photos once the issue is fixed."),
        new Flag("Run", "WOTM_AIVSAI", false, "Player controlled by AI", "Player 1's monster becomes an AI (playerInit -> aiInit): AI vs AI fight. For batches without the menu use tools\\lote.ps1."),
        new Flag("Run", "WOTM_WATCHDOG=0", false, "Turn off the freeze watchdog", "By default, g_frame stuck for 8 s (outside movies) writes [watchdog] to live.err.log and ram_/vram_vigia_<g_frame>.bin to discroot; game frames below 25/s log a [watchdog] SLOW warning."),
        new Flag("Run", "WOTM_FMV_MUTE", false, "Mute movies", ""),

        new Flag("Rendering diagnostics", "WOTM_NOFX", false, "No particles/effects", "Turns off the fx[] particles and the wotm_fx effects."),
        new Flag("Rendering diagnostics", "WOTM_NOSHADOW", false, "No shadows", "Turns off blocker shadows (monsters and the like)."),

        new Flag("Logs (live.err.log)", ":Stats", false, "Statistics", "-Stats 1: [native]/[ui]/[time] lines."),
        new Flag("Logs (live.err.log)", "WOTM_FIELDLOG", false, "Fields per frame", "One [field] line per game frame: fields, updateRate, EE cycles, host time and the most expensive loops."),
        new Flag("Logs (live.err.log)", "WOTM_CAMLOG", false, "Camera on every draw", "One [cam] line per draw: t, g_frame, fov, position and direction."),
        new Flag("Logs (live.err.log)", "PS2X_SND_LOG", false, "Native sound", "989snd calls."),
    };

    readonly Dictionary<Flag, CheckBox> boxes = new Dictionary<Flag, CheckBox>();
    readonly string exeDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
    string IniPath { get { return Path.Combine(exeDir, "WotM_Launcher.ini"); } }
    readonly Label status = new Label();
    readonly Button play = new Button { Text = "Play", AutoSize = true };   // offline; online play starts with Start match in the Online group

    Launcher()
    {
        Text = "WotM - options";
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }   // the exe's own (native\wotm.ico)
        Font = new Font("Segoe UI", 9f);
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;

        var tips = new ToolTip { AutoPopDelay = 30000 };
        var root = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Padding = new Padding(10), WrapContents = false };
        // two columns: match and cheats on the left; the Flags checkboxes on the right
        var cols = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        var left = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
        var right = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
        cols.Controls.Add(left);
        cols.Controls.Add(right);
        root.Controls.Add(cols);
        Dictionary<string, string> saved = LoadIni();
        left.Controls.Add(MatchGroup(saved, tips));
        left.Controls.Add(OnlineGroup(saved, tips));
        left.Controls.Add(CheatsGroup(saved, tips));
        GroupBox gb = null;
        FlowLayoutPanel inner = null;
        foreach (Flag f in Flags)
        {
            if (gb == null || gb.Text != f.Group)
            {
                gb = new DarkGroupBox { Text = f.Group, AutoSize = true, Width = 420, Padding = new Padding(8) };
                inner = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
                gb.Controls.Add(inner);
                right.Controls.Add(gb);
            }
            var cb = new DarkCheckBox { Text = f.Label, AutoSize = true };
            string v;
            cb.Checked = saved.TryGetValue(f.Key, out v) ? v == "1" : f.Default;
            if (f.Tip.Length > 0) tips.SetToolTip(cb, f.Tip);
            inner.Controls.Add(cb);
            boxes[f] = cb;
        }
        UpdateMatch();
        int wide = 0;
        foreach (Control c in right.Controls) wide = Math.Max(wide, c.PreferredSize.Width);
        foreach (Control c in right.Controls) c.MinimumSize = new Size(wide, 0);

        var buttons = new FlowLayoutPanel { AutoSize = true };
        play.Click += delegate { Play(); };
        tips.SetToolTip(play, "Starts the game on this PC (offline) and closes the launcher. Online matches start with Start match in the " +
            "Online group: the game opens by itself, so do not press Play while you are in a lobby.");
        var log = new Button { Text = "Open live.err.log", AutoSize = true };
        log.Click += delegate { OpenLog(); };
        var reset = new Button { Text = "Defaults", AutoSize = true };
        reset.Click += delegate
        {
            foreach (var kv in boxes) kv.Value.Checked = kv.Key.Default;
            matchOn.Checked = false;
            matchPlayer.SelectedIndex = 0;
            matchPlayerSkin.SelectedIndex = 0;
            matchLevel.SelectedIndex = 0;
            for (int i = 0; i < matchAIs.Length; ++i) { matchAIs[i].SelectedIndex = i == 0 ? 1 : 0; matchAISkins[i].SelectedIndex = 0; }
            foreach (CheckBox c in cheatBoxes) c.Checked = false;
            foreach (CheckBox c in modBoxes) c.Checked = false;
            cheatStamina.SelectedIndex = 0;
            cheatWin.Value = 0;
        };
        buttons.Controls.Add(play);
        buttons.Controls.Add(log);
        var modkit = new Button { Text = "Mod kit", AutoSize = true };
        modkit.Click += delegate { OpenModKit(); };
        tips.SetToolTip(modkit, "Extract textures and sounds from the disc, edit them and pack them into mods\\<mod>\\ (docs\\passos\\MODS.md).");
        buttons.Controls.Add(modkit);
        buttons.Controls.Add(reset);
        root.Controls.Add(buttons);
        status.AutoSize = true;
        status.MaximumSize = new Size(420, 0);
        root.Controls.Add(status);
        Controls.Add(root);
        if (Dark) ApplyDark(this);
        AcceptButton = play;
        FormClosing += delegate { SaveIni(); };
    }

    // Direct match (WOTM_MATCH=<player>[:<skin>][,<ai>[:<skin>]...],<stage>; no AI = alone on the stage): indices from GLOSSARIO.md. The
    // runtime accepts up to 7 AIs (8 monsters). Skin = costume 0..3 (files <PREFIX>0-3 in disc\MON); "Random" (monster or skin) is drawn on Play.
    // Left out of the list: Spider (6) and Ogre (12), which were cut, and the bosses (13..15).
    static readonly int[] MonsterIds = { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11 };
    static readonly string[] MonsterNames = { "Congar", "Robo-47", "Togera", "Ultra V", "Preytor", "Kineticlops", "Magmo", "Raptros", "Agamo", "Zorgulon" };
    static readonly int[] LevelIds = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16 };
    static readonly string[] LevelNames = { "Midtown Park", "Gamblers Gulch", "Rosdale Canyon", "Metro City", "Century Airfield", "Atomic Island",
        "Baytown", "Club Caldera", "Tsunopolis", "UFO", "Capitol", "Lake Powell", "Mini Baytown", "Volcano", "Tokyo Deathmatch" };
    const int MatchMaxAIs = 7;
    const int RandomId = -1;   // id saved in the ini for "Random" (monster and skin)
    static readonly Random rng = new Random();
    // In-game name and MON\ file prefix of each monster in MonsterIds (skin.json accepts either)
    static readonly string[] MonsterGameNames = { "congar", "robo-47", "togera", "ultra-v", "preytor", "kineticlops", "magmo", "raptros", "agamo", "zorgulon" };
    static readonly string[] MonsterPrefixes = { "MONKEY", "ROBOT", "LIZARD", "SHOGUN", "MANTIS", "ENERGY", "LAVA", "DRAGON", "ROCK", "ALIEN" };

    readonly CheckBox matchOn = new DarkCheckBox { Text = "Direct match, no menu", AutoSize = true };
    readonly ComboBox matchPlayer = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 };
    readonly ComboBox matchLevel = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 };
    readonly ComboBox[] matchAIs = new ComboBox[MatchMaxAIs];
    readonly ComboBox matchPlayerSkin = SkinBox(120);
    readonly ComboBox[] matchAISkins = new ComboBox[MatchMaxAIs];
    // gameplay modifiers, in LobbyClient.ModifierNames order
    readonly CheckBox[] modBoxes = new CheckBox[3];

    string ModsValue() { return LobbyClient.ModifierList(modBoxes[0].Checked, modBoxes[1].Checked, modBoxes[2].Checked); }

    // Skin box item: the text and the WOTM_MATCH value ("0".."3", the mod skin id, "random")
    class SkinItem
    {
        public string Label, Value;
        public SkinItem(string label, string value) { Label = label; Value = value; }
        public override string ToString() { return Label; }
    }
    const string RandomSkin = "random";

    // Mod skins (discroot\mods\<mod>\skins\<folder>\skin.json, MODS.md) by index in MonsterIds: (name, id)
    readonly List<KeyValuePair<string, string>>[] modSkins = new List<KeyValuePair<string, string>>[10];

    static string Norm(string s)
    {
        var b = new System.Text.StringBuilder();
        foreach (char c in s) if (char.IsLetterOrDigit(c)) b.Append(char.ToLowerInvariant(c));
        return b.ToString();
    }
    static string JsonString(string json, string key)
    {
        var m = System.Text.RegularExpressions.Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
        return m.Success ? m.Groups[1].Value : null;
    }

    // Same rule as the runtime (wotm_mods.hpp): folders starting with '_' or '.' are skipped; without an id, the folder name is used;
    // with the same monster and id in two mods, the last one in alphabetical order wins.
    void LoadModSkins()
    {
        for (int k = 0; k < modSkins.Length; ++k) modSkins[k] = new List<KeyValuePair<string, string>>();
        string mods = ModsDir();
        if (mods == null) return;
        var dirs = new List<string>(Directory.GetDirectories(mods));
        dirs.Sort(StringComparer.OrdinalIgnoreCase);
        var byKey = new SortedDictionary<string, KeyValuePair<int, KeyValuePair<string, string>>>(StringComparer.Ordinal);
        foreach (string mod in dirs)
        {
            string n = Path.GetFileName(mod);
            if (n.StartsWith("_") || n.StartsWith(".")) continue;
            string skinsDir = Path.Combine(mod, "skins");
            if (!Directory.Exists(skinsDir)) continue;
            foreach (string dir in Directory.GetDirectories(skinsDir))
            {
                string js = Path.Combine(dir, "skin.json");
                if (!File.Exists(js)) continue;
                string text;
                try { text = File.ReadAllText(js); } catch (IOException) { continue; }
                string mon = JsonString(text, "monster");
                if (mon == null) continue;
                int k = -1;
                for (int i = 0; i < MonsterGameNames.Length; ++i)
                    if (Norm(mon) == Norm(MonsterGameNames[i]) || Norm(mon) == Norm(MonsterPrefixes[i])) k = i;
                if (k < 0) continue;
                string id = (JsonString(text, "id") ?? Path.GetFileName(dir)).ToLowerInvariant();
                string name = JsonString(text, "name") ?? id.ToUpperInvariant();
                byKey[k + "/" + id] = new KeyValuePair<int, KeyValuePair<string, string>>(k, new KeyValuePair<string, string>(name, id));
            }
        }
        foreach (var kv in byKey) modSkins[kv.Value.Key].Add(kv.Value.Value);
    }

    // Custom monsters (discroot\mods\<mod>\monsters\<folder>\monster.json, MODS.md): (name, id), in id order. Player only: a custom
    // boss as an AI would run the boss AI, which needs its own stage.
    readonly List<KeyValuePair<string, string>> modCustoms = new List<KeyValuePair<string, string>>();
    void LoadModCustoms()
    {
        modCustoms.Clear();
        string mods = ModsDir();
        if (mods == null) return;
        var dirs = new List<string>(Directory.GetDirectories(mods));
        dirs.Sort(StringComparer.OrdinalIgnoreCase);
        var byId = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (string mod in dirs)
        {
            string n = Path.GetFileName(mod);
            if (n.StartsWith("_") || n.StartsWith(".")) continue;
            string monDir = Path.Combine(mod, "monsters");
            if (!Directory.Exists(monDir)) continue;
            foreach (string dir in Directory.GetDirectories(monDir))
            {
                string js = Path.Combine(dir, "monster.json");
                if (!File.Exists(js)) continue;
                string text;
                try { text = File.ReadAllText(js); } catch (IOException) { continue; }
                if (JsonString(text, "base") == null) continue;
                string id = (JsonString(text, "id") ?? Path.GetFileName(dir)).ToLowerInvariant();
                byId[id] = JsonString(text, "name") ?? id.ToUpperInvariant();
            }
        }
        foreach (var kv in byId) modCustoms.Add(new KeyValuePair<string, string>(kv.Value, kv.Key));
    }
    // Index of the custom monster in the player box (after the 10 and Random), or -1
    int CustomIndex(int sel) { int c = sel - MonsterIds.Length - 1; return c >= 0 && c < modCustoms.Count ? c : -1; }

    static ComboBox SkinBox(int width)
    {
        return new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = width, DropDownWidth = 180 };
    }

    // Skin box items for monster k (index in MonsterIds; outside it = Random: only the 4 from the disc)
    void FillSkins(ComboBox cb, int k, string keep)
    {
        cb.BeginUpdate();
        cb.Items.Clear();
        for (int i = 0; i < 4; ++i) cb.Items.Add(new SkinItem("Skin " + (i + 1), i.ToString()));
        if (k >= 0 && k < modSkins.Length && modSkins[k] != null)
            foreach (var s in modSkins[k]) cb.Items.Add(new SkinItem(s.Key, s.Value));
        cb.Items.Add(new SkinItem("Random", RandomSkin));
        cb.EndUpdate();
        cb.SelectedIndex = 0;
        if (keep == RandomId.ToString()) keep = RandomSkin;   // ini from before mod skins existed
        for (int i = 0; i < cb.Items.Count; ++i)
            if (((SkinItem)cb.Items[i]).Value == keep) cb.SelectedIndex = i;
    }

    static string SkinId(ComboBox cb) { return cb.SelectedItem == null ? "0" : ((SkinItem)cb.SelectedItem).Value; }

    // Monster picked in a box (index in MonsterIds; Random = MonsterIds.Length)
    void WireSkins(ComboBox monster, ComboBox skin, int offset, string saved)
    {
        FillSkins(skin, monster.SelectedIndex - offset, saved);
        monster.SelectedIndexChanged += delegate { FillSkins(skin, monster.SelectedIndex - offset, SkinId(skin)); };
    }
    readonly Label matchInfo = new Label { AutoSize = true, MaximumSize = new Size(400, 0) };

    GroupBox MatchGroup(Dictionary<string, string> saved, ToolTip tips)
    {
        var gb = new DarkGroupBox { Text = "Match", AutoSize = true, Width = 420, Padding = new Padding(8) };
        var inner = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        gb.Controls.Add(inner);
        string v;

        matchOn.Checked = saved.TryGetValue(":MatchOn", out v) && v == "1";
        tips.SetToolTip(matchOn, "Skips the menu and goes straight to the fight (1 player against the CPU, 3 lives, no win target: " +
            "the fight goes on without the end-of-match dialog). With no AI you play alone on the stage. Forces -Auto 0, since the scripted pad would press buttons mid-fight. " +
            "With 'Player controlled by AI' (Run) it becomes AI vs AI.");
        inner.Controls.Add(matchOn);

        matchPlayer.Items.AddRange(MonsterNames);
        matchPlayer.Items.Add("Random");
        LoadModCustoms();
        foreach (var c in modCustoms) matchPlayer.Items.Add(c.Key + " (mod)");
        matchPlayer.SelectedIndex = saved.TryGetValue(":MatchPlayer", out v) && ParseInt(v) == RandomId ? MonsterNames.Length
            : IndexOrZero(MonsterIds, saved, ":MatchPlayer");
        if (v != null)
            for (int i = 0; i < modCustoms.Count; ++i)
                if (modCustoms[i].Value == v) matchPlayer.SelectedIndex = MonsterNames.Length + 1 + i;
        tips.SetToolTip(matchPlayer, "'Random' draws one of the ten monsters when you press Play. After it, the custom monsters of " +
            "discroot\\mods\\<mod>\\monsters\\ (MODS.md), with their own costume.");
        LoadModSkins();
        WireSkins(matchPlayer, matchPlayerSkin, 0, saved.TryGetValue(":MatchPlayerSkin", out v) ? v : "0");
        tips.SetToolTip(matchPlayerSkin, SkinTip);
        var you = Row("Your monster", matchPlayer);
        you.Controls.Add(matchPlayerSkin);
        inner.Controls.Add(you);

        foreach (int id in LevelIds) matchLevel.Items.Add(id + "  " + LevelNames[Array.IndexOf(LevelIds, id)]);
        matchLevel.SelectedIndex = IndexOrZero(LevelIds, saved, ":MatchLevel");
        tips.SetToolTip(matchLevel, "Number = stage index in GLOSSARIO.md. Every stage fits 8 monsters: the monsters load above 32 MB (64 MB runtime); " +
            "Metro City with 8 still has ~21 MB to spare.");
        inner.Controls.Add(Row("Stage", matchLevel));

        var modsRow = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        modsRow.Controls.Add(new Label { Text = "Modifiers:", AutoSize = true, Margin = new Padding(3, 6, 3, 0) });
        string[] modTips = {
            "Moon gravity: everyone jumps about twice as high and thrown monsters float.",
            "Monsters walk, run and dash 1.5x faster; animations and attacks keep their speed.",
            "The whole game runs at 2x speed (sound stays at normal speed)." };
        for (int i = 0; i < modBoxes.Length; ++i)
        {
            var cb = new DarkCheckBox { Text = LobbyClient.ModifierLabels[i], AutoSize = true };
            cb.Checked = saved.TryGetValue(":Mod_" + LobbyClient.ModifierNames[i], out v) && v == "1";
            tips.SetToolTip(cb, modTips[i] + " Offline, or the host's choice for the lobby (sent to the server; joiners play the host's). " +
                "With all three off, offline uses the in-game panel's MODIFIERS tab.");
            cb.CheckedChanged += delegate { if (lobbyOwner) NetSend("SET mods=" + ModsValue()); };
            modBoxes[i] = cb;
            modsRow.Controls.Add(cb);
        }
        inner.Controls.Add(modsRow);

        inner.Controls.Add(new Label { Text = "AI opponents (up to 7; none = play solo on the stage):", AutoSize = true, Margin = new Padding(3, 6, 3, 0) });
        var foes = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, MaximumSize = new Size(420, 0), WrapContents = true };
        for (int i = 0; i < MatchMaxAIs; ++i)
        {
            var cb = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 112 };
            cb.Items.Add("-");
            cb.Items.AddRange(MonsterNames);
            cb.Items.Add("Random");
            int id = saved.TryGetValue(":MatchAI" + (i + 1), out v) ? ParseInt(v) : (i == 0 ? MonsterIds[0] : 0);
            int k = Array.IndexOf(MonsterIds, id);
            cb.SelectedIndex = id == RandomId ? MonsterNames.Length + 1 : (k >= 0 ? k + 1 : 0);
            cb.SelectedIndexChanged += delegate { UpdateMatch(); };
            tips.SetToolTip(cb, "'-' = empty slot. 'Random' draws one of the ten monsters when you press Play.");
            matchAIs[i] = cb;
            var sk = SkinBox(80);
            WireSkins(cb, sk, 1, saved.TryGetValue(":MatchAI" + (i + 1) + "Skin", out v) ? v : "0");
            tips.SetToolTip(sk, SkinTip);
            matchAISkins[i] = sk;
            var pair = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 6, 0) };
            pair.Controls.Add(cb);
            pair.Controls.Add(sk);
            foes.Controls.Add(pair);
        }
        inner.Controls.Add(foes);
        matchInfo.ForeColor = DimColor;
        inner.Controls.Add(matchInfo);

        matchOn.CheckedChanged += delegate { UpdateMatch(); };
        matchPlayer.SelectedIndexChanged += delegate { UpdateMatch(); };
        matchLevel.SelectedIndexChanged += delegate { UpdateMatch(); };
        UpdateMatch();
        return gb;
    }

    const string SkinTip = "Costume (the game's 4 per monster; Agamo's Skin 4 is Mecha Sweet Tooth), then the mod skins of that monster " +
        "(discroot\\mods\\<mod>\\skins\\, MODS.md). Locked costumes work too. If the same monster already has a disc skin in the fight, it gets " +
        "the next free one, as in the menu; mod skins can repeat. 'Random' is drawn when you press Play.";

    int MatchFoes()
    {
        int n = 0;
        foreach (ComboBox cb in matchAIs) if (cb.SelectedIndex > 0) ++n;
        return n;
    }

    void UpdateMatch()
    {
        bool on = matchOn.Checked;
        // monster and stage also apply to the network match (Online group); the AIs and costumes only to the direct match
        matchPlayerSkin.Enabled = on && CustomIndex(matchPlayer.SelectedIndex) < 0;   // the custom monster has its own costume
        for (int i = 0; i < matchAIs.Length; ++i)
        {
            matchAIs[i].Enabled = on;
            if (matchAISkins[i] != null) matchAISkins[i].Enabled = on && matchAIs[i].SelectedIndex > 0;
        }
        int total = 1 + MatchFoes();
        string txt = total == 1 ? "Solo: just you on the stage." : total + " monsters in the fight.";
        if (total > 4) txt += " Without the enemy bars (Recompiled options, in the main menu), only 4 enemies get an off-screen health marker.";
        matchInfo.Text = txt;
    }

    // "<monster>:<skin>", with any "Random" drawn here (k = index in MonsterIds, MonsterIds.Length = Random). Random skin: one of
    // the 4 from the disc and the mod skins of the drawn monster.
    string MatchEntry(int k, ComboBox skin)
    {
        int c = CustomIndex(k);
        if (c >= 0) return modCustoms[c].Value;   // the runtime reads the id (WOTM_MATCH=goliath,...)
        if (k >= MonsterIds.Length) k = rng.Next(MonsterIds.Length);
        string s = SkinId(skin);
        if (s == RandomSkin)
        {
            int n = rng.Next(4 + modSkins[k].Count);
            s = n < 4 ? n.ToString() : modSkins[k][n - 4].Value;
        }
        return MonsterIds[k] + ":" + s;
    }

    string MatchValue()
    {
        var parts = new List<string> { MatchEntry(matchPlayer.SelectedIndex, matchPlayerSkin) };
        for (int i = 0; i < matchAIs.Length; ++i)
            if (matchAIs[i].SelectedIndex > 0) parts.Add(MatchEntry(matchAIs[i].SelectedIndex - 1, matchAISkins[i]));
        parts.Add(LevelIds[matchLevel.SelectedIndex].ToString());
        return string.Join(",", parts.ToArray());
    }

    // Network match through wotm_server (native/net): the launcher talks to the lobby (LobbyClient.cs). Whoever creates a lobby owns it
    // (4-letter code, optional password): only the owner picks the stage (the "Stage" of the Match group) and the wins, and starts. The
    // others join by code or from the list. During the match the launcher stays minimized and in the lobby; when the game closes (QUIT
    // in the game) it comes back, in the same lobby. "Host on this PC" runs this folder's wotm_server hidden, as a child of the launcher
    // (ServerHost.cs; tools\build_net.bat builds it), creates a lobby on it and shows the addresses for friends; tries UPnP on the router.
    readonly TextBox netServer = new TextBox { Width = 200 };
    readonly TextBox netName = new TextBox { Width = 160 };
    readonly TextBox netLobbyName = new TextBox { Width = 160 };
    readonly TextBox netPassword = new TextBox { Width = 110 };
    readonly TextBox netCode = new TextBox { Width = 70, CharacterCasing = CharacterCasing.Upper, MaxLength = 4 };
    readonly ListBox netList = new ListBox { Width = 390, Height = 80 };
    readonly NumericUpDown netWins = new NumericUpDown { Minimum = 0, Maximum = 99, Width = 60 };
    readonly Button netStart = new Button { Text = "Start match", AutoSize = true, Enabled = false };
    readonly Label netInfo = new Label { AutoSize = true, MaximumSize = new Size(400, 0) };
    readonly TextBox netShare = new TextBox { Width = 390, Height = 124, Multiline = true, ReadOnly = true, Visible = false, ScrollBars = ScrollBars.Vertical };
    // public server list: a master server (wotm_server --master) lists the servers that announce themselves to it
    readonly TextBox netMaster = new TextBox { Width = 160 };
    readonly Button netBrowse = new Button { Text = "Browse public servers", AutoSize = true };
    readonly ListBox netServers = new ListBox { Width = 390, Height = 64 };
    readonly List<LobbyClient.ServerInfo> netServerInfos = new List<LobbyClient.ServerInfo>();
    readonly CheckBox netPublic = new DarkCheckBox { Text = "List publicly as", AutoSize = true };
    readonly TextBox netPublicName = new TextBox { Width = 120 };

    // under the share box: what Windows Firewall says about wotm_server on this network, and the button that fixes it
    readonly Label netHostCheck = new Label { AutoSize = true, MaximumSize = new Size(400, 0), Visible = false };
    readonly Button netAllowFw = new Button { Text = "Allow in Windows Firewall", AutoSize = true, Visible = false };
    readonly Timer fwTimer = new Timer { Interval = 3000 };
    string serverExe = "", hostedFirewall = "";
    bool hostedFirewallProblem, fwBusy;
    ServerHost hosted;
    string hostedPublic = "";   // "<master> <name>" the hosted server announces to, "" when private
    List<string> hostedLines = new List<string>();
    string hostedRouter = "";
    // what the internet sees (Reach.cs): the public address line, the address itself, the port self-test line
    string hostedAddress = "", hostedPortTest = "";
    System.Net.IPAddress hostedStun;
    bool portTestBusy, portTestDone;
    readonly Timer netTimer = new Timer { Interval = 200 };
    readonly List<LobbyClient.LobbyInfo> netLobbies = new List<LobbyClient.LobbyInfo>();
    LobbyClient lobby;
    string lobbyHost = "", lobbyCode = "";
    bool lobbyOwner;
    Process onlineGame;

    GroupBox OnlineGroup(Dictionary<string, string> saved, ToolTip tips)
    {
        var gb = new DarkGroupBox { Text = "Online (wotm_server)", AutoSize = true, Width = 420, Padding = new Padding(8) };
        var inner = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        gb.Controls.Add(inner);
        string v;
        netServer.Text = saved.TryGetValue(":NetServer", out v) && v.Length > 0 ? v : "127.0.0.1:7777";
        tips.SetToolTip(netServer, "host:port of a wotm_server (your own with 'Host on this PC', a friend's public IP, or a VPN address). " +
            "The host forwards this port, TCP and UDP, on the router.");
        inner.Controls.Add(Row("Server", netServer));
        netName.Text = saved.TryGetValue(":NetName", out v) && v.Length > 0 ? v : LobbyClient.CleanName(Environment.UserName);
        inner.Controls.Add(Row("Your name", netName));

        netMaster.Text = saved.TryGetValue(":NetMaster", out v) ? v : "";
        tips.SetToolTip(netMaster, "host or host:port of a master server (default port " + LobbyClient.MasterPort + "): the list of public servers.");
        tips.SetToolTip(netBrowse, "Lists the public servers on the master server, with a ping from this PC. Pick one to use it as Server.");
        tips.SetToolTip(netServers, "Pick a server: it becomes the Server above and its lobbies show in the list below.");
        var masterRow = Row("Master server", netMaster);
        masterRow.Controls.Add(netBrowse);
        inner.Controls.Add(masterRow);
        inner.Controls.Add(netServers);

        netLobbyName.Text = saved.TryGetValue(":NetLobbyName", out v) && v.Length > 0 ? v : "Friends";
        var hostLobby = new Button { Text = "Host lobby", AutoSize = true };
        tips.SetToolTip(hostLobby, "Creates a lobby on the server; you are its host. Friends join with the 4-letter code or from the list.");
        tips.SetToolTip(netPassword, "Optional: friends then need this password to join (letters, digits, _ or -). Not real security.");
        var hostRow = Row("Lobby name", netLobbyName);
        hostRow.Controls.Add(hostLobby);
        inner.Controls.Add(hostRow);
        inner.Controls.Add(Row("Password", netPassword));

        var joinCode = new Button { Text = "Join by code", AutoSize = true };
        var codeRow = Row("Code", netCode);
        codeRow.Controls.Add(joinCode);
        inner.Controls.Add(codeRow);

        var listRow = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = true, MaximumSize = new Size(400, 0) };
        var refresh = new Button { Text = "Refresh list", AutoSize = true };
        var joinSel = new Button { Text = "Join selected", AutoSize = true };
        listRow.Controls.Add(refresh);
        listRow.Controls.Add(joinSel);
        inner.Controls.Add(netList);
        inner.Controls.Add(listRow);

        netWins.Value = saved.TryGetValue(":NetWins", out v) ? Math.Min(Math.Max(ParseInt(v), 0), 99) : 0;
        tips.SetToolTip(netWins, "Host only: wins needed to end the match (0 = endless). The winner's screen offers PLAY AGAIN, REBUILD LEVEL or QUIT.");
        var winsRow = Row("Wins to end", netWins);
        winsRow.Controls.Add(netStart);
        inner.Controls.Add(winsRow);

        var row = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = true, MaximumSize = new Size(400, 0) };
        var leave = new Button { Text = "Leave lobby", AutoSize = true };
        var host = new Button { Text = "Host on this PC", AutoSize = true };
        tips.SetToolTip(netStart, "Host only: starts the match for everyone in your lobby, on the stage of the Match group. The game opens on every PC.");
        tips.SetToolTip(host, "Runs the server on this PC (hidden; it stops when you close the launcher, so keep it open while you play), " +
            "hosts a lobby on it and shows the address to give your friends. Tries to open the port on your router (UPnP).");
        row.Controls.Add(leave);
        row.Controls.Add(host);
        inner.Controls.Add(row);
        netPublic.Checked = saved.TryGetValue(":NetPublic", out v) && v == "1";
        netPublicName.Text = saved.TryGetValue(":NetPublicName", out v) && v.Length > 0 ? v : netName.Text;
        tips.SetToolTip(netPublic, "With 'Host on this PC': your server shows up in the public list of the Master server, under this name. " +
            "Players still need your port forwarded (or UPnP) to reach it.");
        var publicRow = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        publicRow.Controls.Add(netPublic);
        publicRow.Controls.Add(netPublicName);
        inner.Controls.Add(publicRow);

        hostLobby.Click += delegate { NetCreate(); };
        joinCode.Click += delegate { NetJoin(netCode.Text.Trim()); };
        // Enter inside the Online boxes is not Play (the form's AcceptButton): in Code it joins, in Lobby name it hosts, in the list it
        // joins the selected lobby; in the others it does nothing. An offline Play would close the launcher, and with it the server and lobby.
        AcceptOn(netCode, joinCode);
        AcceptOn(netLobbyName, hostLobby);
        AcceptOn(netList, joinSel);
        foreach (Control c in new Control[] { netServer, netName, netPassword, netWins }) AcceptOn(c, null);
        joinSel.Click += delegate
        {
            int i = netList.SelectedIndex;
            if (i < 0 || i >= netLobbies.Count) { netInfo.Text = "Pick a lobby in the list (Refresh list first)."; return; }
            NetJoin(netLobbies[i].Code);
        };
        netList.DoubleClick += delegate { joinSel.PerformClick(); };
        refresh.Click += delegate { NetRefresh(); };
        netStart.Click += delegate { NetStart(); };
        netWins.ValueChanged += delegate { if (lobbyOwner) NetSend("SET wins=" + (int)netWins.Value); };
        matchLevel.SelectedIndexChanged += delegate { if (lobbyOwner) NetSend("SET stage=" + LevelIds[matchLevel.SelectedIndex]); };
        leave.Click += delegate { NetSend("LEAVE"); };
        host.Click += delegate { NetHost(); };
        netBrowse.Click += delegate { NetBrowse(); };
        netServers.SelectedIndexChanged += delegate { NetPickServer(); };
        netInfo.ForeColor = DimColor;
        netInfo.Text = "Pick your monster and stage in Match, then host a lobby or join one. Online play uses the build in this folder.";
        inner.Controls.Add(netInfo);
        tips.SetToolTip(netShare, "Copy this to your friends: they type the address as Server and the code in Code.");
        inner.Controls.Add(netShare);
        tips.SetToolTip(netAllowFw, "Adds a Windows Firewall rule that lets wotm_server receive connections on every network type (TCP and UDP) " +
            "and removes the rules the Windows prompt made for it. Windows asks for permission.");
        netAllowFw.Click += delegate { AllowFirewall(); };
        inner.Controls.Add(netHostCheck);
        inner.Controls.Add(netAllowFw);
        netTimer.Tick += delegate { NetPoll(); };
        fwTimer.Tick += delegate { if (hosted == null) fwTimer.Stop(); else CheckFirewall(); };
        FormClosed += delegate
        {
            netTimer.Stop();
            fwTimer.Stop();
            if (lobby != null) lobby.Dispose();
            if (hosted != null) hosted.Dispose();
        };
        return gb;
    }

    // wotm.buildid next to the runtime's wotm.exe (the same one the game sends to the server)
    string BuildId()
    {
        string rt = RuntimeDir();
        string f = rt == null ? null : Path.Combine(rt, @"build_dev\ps2xRuntime\wotm.buildid");
        try { if (f != null && File.Exists(f)) return File.ReadAllText(f).Trim(); } catch (IOException) { }
        return "sem-buildid";
    }

    bool NetConnect()
    {
        if (lobby != null && !lobby.Closed) return true;
        string s = netServer.Text.Trim();
        int colon = s.LastIndexOf(':');
        string h = colon > 0 ? s.Substring(0, colon) : s;
        int port = colon > 0 ? ParseInt(s.Substring(colon + 1)) : 7777;
        try
        {
            lobby = new LobbyClient(h, port, BuildId(), 3000);
            lobbyHost = h;
            netTimer.Start();
            return true;
        }
        catch (Exception e)
        {
            netInfo.Text = "Server: " + e.Message;
            lobby = null;
            return false;
        }
    }

    void NetSend(string line)
    {
        if (!NetConnect()) return;
        try { lobby.Send(line); } catch (Exception e) { netInfo.Text = "Server: " + e.Message; }
    }

    void NetRefresh()
    {
        netLobbies.Clear();
        netList.Items.Clear();
        NetSend("LIST");
    }

    // this player's name and monster, or null with a message saying what is missing
    string NetMe(out int monster)
    {
        monster = 0;
        string name = LobbyClient.CleanName(netName.Text);
        if (name.Length == 0) { netInfo.Text = "Type a name (letters, digits, _ or -)."; return null; }
        netName.Text = name;
        int k = matchPlayer.SelectedIndex;
        if (k < 0 || k >= MonsterIds.Length) { netInfo.Text = "Online needs a monster from the disc (not Random or a mod monster)."; return null; }
        monster = MonsterIds[k];
        return name;
    }

    string NetPassword()
    {
        string p = LobbyClient.CleanName(netPassword.Text);
        netPassword.Text = p;
        return p.Length > 0 ? " " + p : "";
    }

    void NetCreate()
    {
        int monster;
        string name = NetMe(out monster);
        if (name == null) return;
        string lobbyName = LobbyClient.CleanName(netLobbyName.Text);
        if (lobbyName.Length == 0) lobbyName = "Lobby";
        netLobbyName.Text = lobbyName;
        NetSend("CREATE " + lobbyName + " " + name + " " + monster + NetPassword());
    }

    void NetJoin(string code)
    {
        int monster;
        string name = NetMe(out monster);
        if (name == null) return;
        if (code.Length != 4) { netInfo.Text = "Type the 4-letter lobby code."; return; }
        NetSend("JOIN " + code + " " + name + " " + monster + NetPassword());
    }

    // the owner's stage, wins and modifiers for the lobby
    string NetSetLine()
    {
        return "SET stage=" + LevelIds[matchLevel.SelectedIndex] + " wins=" + (int)netWins.Value + " mods=" + ModsValue();
    }

    // the owner sends the current settings, then starts
    void NetStart()
    {
        if (!lobbyOwner) { netInfo.Text = "Only the lobby host starts the match."; return; }
        NetSend(NetSetLine());
        NetSend("START");
    }

    // player as host: starts the server (or uses the one already on the port), connects to it on 127.0.0.1 and creates the lobby; UPnP
    // runs on a thread (it takes seconds) and fills in the addresses when it returns
    void NetHost()
    {
        if (hosted != null && hosted.Running && lobbyCode.Length > 0) { netInfo.Text = "You are already hosting lobby " + lobbyCode + "."; return; }
        string exe = Path.Combine(exeDir, @"native\net\build\wotm_server.exe");
        string s = netServer.Text.Trim();
        int colon = s.LastIndexOf(':');
        int port = colon > 0 ? ParseInt(s.Substring(colon + 1)) : 7777;
        if (port <= 0 || port > 65535) port = 7777;
        string master = netMaster.Text.Trim(), publicName = LobbyClient.CleanName(netPublicName.Text);
        if (netPublic.Checked && master.Length == 0) { netInfo.Text = "Type the Master server's address to list your server publicly."; return; }
        if (netPublic.Checked && publicName.Length == 0) publicName = LobbyClient.CleanName(netName.Text);
        netPublicName.Text = publicName;
        string wantPublic = netPublic.Checked ? master + " " + publicName : "";
        if (hosted == null || !hosted.Running || hosted.Port != port || (hosted.Owned && hostedPublic != wantPublic))
        {
            if (hosted != null) hosted.Dispose();
            try
            {
                hosted = ServerHost.Start(exe, port, Path.Combine(exeDir, "wotm_server.log"), 3000, netPublic.Checked ? master : null, publicName);
            }
            catch (LobbyError e) { hosted = null; netInfo.Text = "Failed to start the server: " + e.Message; return; }
            hostedPublic = hosted.Owned ? wantPublic : "";
            hostedAddress = hostedPortTest = "";
            hostedStun = null;
            portTestDone = false;
            hostedRouter = hosted.Owned ? "Router: trying to open port " + port + " (UPnP)..." : "A server was already running on port " + port + ": using it" +
                (netPublic.Checked ? " (it isn't listed publicly by this launcher)." : ".");
            if (hosted.Owned) MapRouterPort(hosted);
        }
        if (lobby != null && lobby.Host != "127.0.0.1") { lobby.Dispose(); lobby = null; }
        netServer.Text = "127.0.0.1:" + port;
        hostedLines = ServerHost.ShareLines(ServerHost.LocalAddresses(), null, port);
        ShowShare();
        serverExe = exe;
        CheckFirewall();
        NetCreate();
    }

    // Reads the Windows Firewall rules for the server's exe on a thread and shows the verdict under the share box. Windows creates the
    // rules only when the host answers its prompt, which comes after the server starts, so it is checked again every few seconds
    // (fwTimer) until the answer is good; the timer also catches the Allow button's change.
    void CheckFirewall()
    {
        if (fwBusy || serverExe.Length == 0) return;
        fwBusy = true;
        string exe = serverExe;
        new System.Threading.Thread(delegate()
        {
            string text;
            bool problem = false;
            try
            {
                int current, enabled;
                List<FirewallRule> rules = FirewallCheck.RulesFor(exe, out current, out enabled);
                text = FirewallCheck.Verdict(rules, current, enabled, out problem);
            }
            catch (Exception e) { text = "Could not read the Windows Firewall rules (" + e.Message + ")."; }
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    fwBusy = false;
                    hostedFirewall = text;
                    hostedFirewallProblem = problem;
                    if (problem || !text.StartsWith("Windows Firewall")) fwTimer.Start(); else fwTimer.Stop();
                    ShowShare();
                    TryPortTest();
                });
            }
            catch (InvalidOperationException) { }   // launcher already closed
        }) { IsBackground = true }.Start();
    }

    // The button: adds the allow rule through an elevated PowerShell (UAC prompt), then checks again.
    void AllowFirewall()
    {
        if (serverExe.Length == 0) return;
        string exe = serverExe;
        netAllowFw.Enabled = false;
        netHostCheck.Text = "Windows is asking for permission to change the firewall...";
        new System.Threading.Thread(delegate()
        {
            string why;
            bool ok = FirewallCheck.Allow(exe, out why);
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    netAllowFw.Enabled = true;
                    if (ok) { CheckFirewall(); return; }
                    hostedFirewall = "Firewall rule not added: " + why + ".";
                    hostedFirewallProblem = true;
                    ShowShare();
                });
            }
            catch (InvalidOperationException) { }   // launcher already closed
        }) { IsBackground = true }.Start();
    }

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
            catch (InvalidOperationException) { }   // launcher already closed
        }) { IsBackground = true }.Start();
    }

    // public servers from the master server; the pings take up to a second or two, so the list is fetched on a thread
    void NetBrowse()
    {
        string master = netMaster.Text.Trim();
        if (master.Length == 0) { netInfo.Text = "Type the Master server's address (host or host:" + LobbyClient.MasterPort + ")."; return; }
        netBrowse.Enabled = false;
        netInfo.Text = "Asking the master server " + master + "...";
        new System.Threading.Thread(delegate()
        {
            List<LobbyClient.ServerInfo> list = null;
            string error = null;
            try { list = LobbyClient.Browse(master, 3000); }
            catch (LobbyError e) { error = e.Message; }
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    netBrowse.Enabled = true;
                    netServerInfos.Clear();
                    netServers.Items.Clear();
                    if (error != null) { netInfo.Text = "Master server: " + error; return; }
                    foreach (LobbyClient.ServerInfo si in list)
                    {
                        netServerInfos.Add(si);
                        netServers.Items.Add(si.Name + "   " + si.Ip + ":" + si.Port + "   " + si.Open + (si.Open == 1 ? " lobby" : " lobbies") +
                            " (" + si.Waiting + " waiting)   " + si.Players + (si.Players == 1 ? " player" : " players") + "   " +
                            LobbyClient.ServerStatus(si));
                    }
                    netInfo.Text = list.Count == 0 ? "No public servers right now." : "Pick a server to see its lobbies.";
                });
            }
            catch (InvalidOperationException) { }   // launcher already closed
        }) { IsBackground = true }.Start();
    }

    // a server picked in the public list becomes the Server, and its lobbies are listed
    void NetPickServer()
    {
        int i = netServers.SelectedIndex;
        if (i < 0 || i >= netServerInfos.Count) return;
        LobbyClient.ServerInfo si = netServerInfos[i];
        if (lobbyCode.Length > 0) { netInfo.Text = "Leave your lobby first to switch servers."; return; }
        netServer.Text = si.Ip + ":" + si.Port;
        if (lobby != null) { lobby.Dispose(); lobby = null; }
        if (si.Protocol != LobbyClient.Protocol) { netInfo.Text = si.Name + " " + LobbyClient.ServerStatus(si) + "."; return; }
        NetRefresh();
    }

    // the box the host copies to friends: addresses, lobby code, router status
    void ShowShare()
    {
        if (hosted == null || (lobby != null && lobby.Host != "127.0.0.1")) { netShare.Visible = netHostCheck.Visible = netAllowFw.Visible = false; return; }
        var l = new List<string>();
        string code = netCode.Text.Trim();
        l.Add("Friends type one of these as Server" + (code.Length == 4 ? " and the code " + code : "") + ":");
        foreach (string a in hostedLines) l.Add("  " + a);
        if (hostedLines.Count == 0) l.Add("  (no network address found)");
        l.Add(hostedRouter);
        if (hostedAddress.Length > 0) l.Add(hostedAddress);
        if (hostedPortTest.Length > 0) l.Add(hostedPortTest);
        if (hostedPublic.Length > 0)
        {
            int sp = hostedPublic.LastIndexOf(' ');
            l.Add("Listed publicly as " + hostedPublic.Substring(sp + 1) + " on the master server " + hostedPublic.Substring(0, sp) + ".");
        }
        if (hosted.Owned) l.Add("If Windows asks whether wotm_server may use the network, allow it.");
        netShare.Text = string.Join(Environment.NewLine, l.ToArray());
        netShare.Visible = true;
        netHostCheck.Text = hostedFirewall + (hostedFirewallProblem ? " Press 'Allow in Windows Firewall' below (Windows asks for permission); " +
            "if the Windows prompt is open right now, Allow there for private and public networks." : "");
        netHostCheck.ForeColor = hostedFirewallProblem ? Color.FromArgb(255, 170, 90) : DimColor;
        netHostCheck.Visible = hostedFirewall.Length > 0;
        netAllowFw.Visible = hostedFirewallProblem;
    }

    string netSettings = "";

    // lobby lines (UI Timer): list, members, settings, errors and the match start
    void NetPoll()
    {
        if (lobby == null) return;
        foreach (string l in lobby.Poll())
        {
            if (l.StartsWith("LOBBY "))
            {
                LobbyClient.LobbyInfo li = LobbyClient.ParseLobby(l);
                if (li == null) continue;
                netLobbies.Add(li);
                netList.Items.Add(li.Code + "  " + li.Name + " (host " + li.Host + ")  " + li.State + " " + li.Count + "/" + li.Max +
                    (li.Locked ? "  [password]" : "") + (li.Names.Length > 0 ? "  " + string.Join(", ", li.Names) : ""));
            }
            else if (l == "END" && netLobbies.Count == 0) netInfo.Text = "No open lobbies on this server: host one.";
            else if (l.StartsWith("CREATED ")) { netCode.Text = l.Substring(8).Trim(); ShowShare(); }
            else if (l.StartsWith("JOINED ")) { lobbyCode = l.Substring(7).Trim(); netCode.Text = lobbyCode; ShowShare(); }
            else if (l.StartsWith("LEFT ")) { lobbyCode = ""; NetNotInLobby(); netInfo.Text = "Left the lobby."; }
            else if (l.StartsWith("SETTINGS "))
            {
                Dictionary<string, string> kv = LobbyClient.KeyValues(l);
                int wins = kv.ContainsKey("wins") ? ParseInt(kv["wins"]) : 0;
                netSettings = "stage " + (kv.ContainsKey("stage") ? kv["stage"] : "?") + ", " + (wins > 0 ? wins + " wins to end" : "endless") +
                    ", " + LobbyClient.ModifierLabel(kv.ContainsKey("mods") ? kv["mods"] : "");
                ShowLobby();
            }
            else if (l.StartsWith("MEMBERS "))
            {
                LobbyClient.MemberInfo m = LobbyClient.ParseMembers(l);
                if (m == null) continue;
                bool wasOwner = lobbyOwner;
                lobbyMembers = m;
                lobbyOwner = m.Host == LobbyClient.CleanName(netName.Text);
                netStart.Enabled = lobbyOwner;
                // a joiner plays the host's modifiers (shown in the lobby line), so its own boxes do not apply in the lobby
                foreach (CheckBox c in modBoxes) c.Enabled = lobbyOwner;
                if (lobbyOwner && !wasOwner) NetSend(NetSetLine());   // the lobby shows the new host's settings before Start match
                ShowLobby();
            }
            else if (l.StartsWith("ERROR ")) netInfo.Text = "Server: " + l.Substring(6);
            else if (l.StartsWith("MATCH ")) PlayOnline(LobbyClient.MatchEnv(LobbyClient.KeyValues(l), lobbyHost));
        }
        if (lobby.Closed)
        {
            netInfo.Text = "Connection to the server closed.";
            netTimer.Stop();
            lobby = null;
            NetNotInLobby();
        }
    }

    LobbyClient.MemberInfo lobbyMembers;

    // the lobby line: code, members, host and the lobby's settings
    void ShowLobby()
    {
        LobbyClient.MemberInfo m = lobbyMembers;
        if (m == null) return;
        var names = new List<string>();
        foreach (string n in m.Names) names.Add(n.Split(':')[0]);
        netInfo.Text = "Lobby " + m.Code + " (code for friends): " + string.Join(", ", names.ToArray()) + ". Host: " + m.Host +
            (netSettings.Length > 0 ? " (" + netSettings + ")" : "") + "." + (lobbyOwner ? " Press Start match when everyone is in." : " Waiting for the host to start.");
    }

    void NetNotInLobby()
    {
        lobbyOwner = false;
        lobbyMembers = null;
        netSettings = "";
        netStart.Enabled = false;
        foreach (CheckBox c in modBoxes) c.Enabled = true;
    }
    static int MonsterId(int k) { return k >= MonsterIds.Length ? RandomId : MonsterIds[k]; }

    // While c has the focus, Enter presses b (or nothing, when null) instead of Play.
    void AcceptOn(Control c, Button b)
    {
        c.Enter += delegate { AcceptButton = b; };
        c.Leave += delegate { AcceptButton = play; };
    }

    // F1 menu cheats turned on from the start (WOTM_CHEATS=inv,stam|furia,esp,cloak,deus,vencer=N); they apply to the players.
    static readonly string[] CheatKeys = { "inv", "esp", "cloak", "deus" };
    static readonly string[] CheatLabels = { "Invincible", "Special always ready", "CLOAK", "God mode (no damage, no hit reaction)" };
    readonly CheckBox[] cheatBoxes = new CheckBox[4];
    readonly ComboBox cheatStamina = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 };
    readonly NumericUpDown cheatWin = new NumericUpDown { Minimum = 0, Maximum = 3600, Width = 70 };

    GroupBox CheatsGroup(Dictionary<string, string> saved, ToolTip tips)
    {
        var gb = new DarkGroupBox { Text = "Cheats from the start", AutoSize = true, Width = 420, Padding = new Padding(8) };
        var inner = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        gb.Controls.Add(inner);
        string v;
        inner.Controls.Add(new Label { Text = "Same as the in-fight F1 menu; apply to human players.", AutoSize = true, ForeColor = DimColor });
        for (int i = 0; i < CheatKeys.Length; ++i)
        {
            var cb = new DarkCheckBox { Text = CheatLabels[i], AutoSize = true };
            cb.Checked = saved.TryGetValue(":Cheat_" + CheatKeys[i], out v) && v == "1";
            cheatBoxes[i] = cb;
            inner.Controls.Add(cb);
        }
        tips.SetToolTip(cheatBoxes[0], "Health never drops; hits still push you.");
        tips.SetToolTip(cheatBoxes[3], "The game's m_godMode (full health, stamina and special) plus, from us, no hit reaction. A building falling on you still kills.");
        cheatStamina.Items.AddRange(new object[] { "Normal", "Always full", "Rage (130)" });
        cheatStamina.SelectedIndex = saved.TryGetValue(":CheatStamina", out v) ? Math.Max(0, Math.Min(2, ParseInt(v))) : 0;
        inner.Controls.Add(Row("Stamina", cheatStamina));
        cheatWin.Value = saved.TryGetValue(":CheatWin", out v) ? Math.Max(0, Math.Min(3600, ParseInt(v))) : 0;
        tips.SetToolTip(cheatWin, "Knocks out every living AI every N seconds of game time (F1's 'win the fight'). 0 = off.");
        inner.Controls.Add(Row("Win every (s, 0 = off)", cheatWin));
        return gb;
    }

    string CheatsValue()
    {
        var parts = new List<string>();
        for (int i = 0; i < CheatKeys.Length; ++i) if (cheatBoxes[i].Checked) parts.Add(CheatKeys[i]);
        if (cheatStamina.SelectedIndex == 1) parts.Add("stam");
        else if (cheatStamina.SelectedIndex == 2) parts.Add("furia");
        if (cheatWin.Value > 0) parts.Add("vencer=" + (int)cheatWin.Value);
        return string.Join(",", parts.ToArray());
    }

    static int ParseInt(string s)
    {
        int n;
        return int.TryParse(s, out n) ? n : 0;
    }

    static int IndexOrZero(int[] ids, Dictionary<string, string> saved, string key)
    {
        string v;
        int k = saved.TryGetValue(key, out v) ? Array.IndexOf(ids, ParseInt(v)) : -1;
        return k >= 0 ? k : 0;
    }

    static FlowLayoutPanel Row(string label, Control c)
    {
        var p = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
        p.Controls.Add(new Label { Text = label, AutoSize = false, Size = new Size(210, 23), TextAlign = ContentAlignment.MiddleLeft });
        p.Controls.Add(c);
        return p;
    }

    Dictionary<string, string> LoadIni()
    {
        var d = new Dictionary<string, string>();
        try
        {
            foreach (string line in File.ReadAllLines(IniPath))
            {
                int i = line.LastIndexOf('|');
                if (i > 0) d[line.Substring(0, i)] = line.Substring(i + 1).Trim();
            }
        }
        catch (IOException) { }
        return d;
    }

    void SaveIni()
    {
        var lines = new List<string>();
        foreach (var kv in boxes) lines.Add(kv.Key.Key + "|" + (kv.Value.Checked ? "1" : "0"));
        lines.Add(":MatchOn|" + (matchOn.Checked ? "1" : "0"));
        int cu = CustomIndex(matchPlayer.SelectedIndex);
        lines.Add(":MatchPlayer|" + (cu >= 0 ? modCustoms[cu].Value : MonsterId(matchPlayer.SelectedIndex).ToString()));
        lines.Add(":MatchPlayerSkin|" + SkinId(matchPlayerSkin));
        lines.Add(":MatchLevel|" + LevelIds[matchLevel.SelectedIndex]);
        for (int i = 0; i < matchAIs.Length; ++i)
        {
            lines.Add(":MatchAI" + (i + 1) + "|" + (matchAIs[i].SelectedIndex > 0 ? MonsterId(matchAIs[i].SelectedIndex - 1) : 0));
            lines.Add(":MatchAI" + (i + 1) + "Skin|" + SkinId(matchAISkins[i]));
        }
        for (int i = 0; i < CheatKeys.Length; ++i) lines.Add(":Cheat_" + CheatKeys[i] + "|" + (cheatBoxes[i].Checked ? "1" : "0"));
        lines.Add(":CheatStamina|" + cheatStamina.SelectedIndex);
        lines.Add(":CheatWin|" + (int)cheatWin.Value);
        lines.Add(":NetServer|" + netServer.Text.Trim());
        lines.Add(":NetName|" + netName.Text.Trim());
        lines.Add(":NetLobbyName|" + netLobbyName.Text.Trim());
        lines.Add(":NetWins|" + (int)netWins.Value);
        for (int i = 0; i < modBoxes.Length; ++i) lines.Add(":Mod_" + LobbyClient.ModifierNames[i] + "|" + (modBoxes[i].Checked ? "1" : "0"));
        lines.Add(":NetMaster|" + netMaster.Text.Trim());
        lines.Add(":NetPublic|" + (netPublic.Checked ? "1" : "0"));
        lines.Add(":NetPublicName|" + netPublicName.Text.Trim());
        try { File.WriteAllLines(IniPath, lines.ToArray()); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }

    // Video, QoL and separate keyboard live in the main menu's "Recompiled options" panel (native/wotm_settings.hpp, settings.ini
    // next to save.json). The launcher does not pass them; an inherited variable would override the file, so it is removed.
    static readonly string[] SettingsKeys = { "WOTM_RES", "WOTM_ASPECT", "WOTM_FULLSCREEN", "WOTM_HZ", "WOTM_VSYNC",
        "WOTM_LOCKTOGGLE", "WOTM_ENEMYBARS", "WOTM_DAMAGEPOP", "WOTM_KBSEPARATE", "WOTM_SKIPINTRO", "WOTM_VIBRATION", "WOTM_INTERP",
        "WOTM_SUPERJUMP", "WOTM_FASTMONSTERS", "WOTM_TURBO" };

    // The runtime folder (the one with run_live.ps1): WOTM_RECOMP_DIR, then the .wotm_recomp_dir that run_live.bat saves in the repo root.
    string KnownRuntimeDir()
    {
        string env = Environment.GetEnvironmentVariable("WOTM_RECOMP_DIR");
        if (!string.IsNullOrEmpty(env) && File.Exists(Path.Combine(env, "run_live.ps1"))) return env;
        string saved = Path.Combine(exeDir, ".wotm_recomp_dir");
        if (File.Exists(saved))
        {
            string dir = File.ReadAllText(saved).Trim();
            if (File.Exists(Path.Combine(dir, "run_live.ps1"))) return dir;
        }
        return null;
    }
    // The mods folder the game reads (wotm_mods.inc): WOTM_MODS; otherwise mods\ next to the ELF (WOTM_DISC or the repo's disc\, as
    // run_live.ps1 does); failing that, the repo's mods\. null if there is none.
    string ModsDir()
    {
        string env = Environment.GetEnvironmentVariable("WOTM_MODS");
        if (env == "0") return null;
        if (!string.IsNullOrEmpty(env)) return Directory.Exists(env) ? env : null;
        var dirs = new List<string>();
        string disc = Environment.GetEnvironmentVariable("WOTM_DISC");
        if (!string.IsNullOrEmpty(disc)) dirs.Add(Path.Combine(disc, "mods"));
        dirs.Add(Path.Combine(Path.Combine(exeDir, "disc"), "mods"));
        dirs.Add(Path.Combine(exeDir, "mods"));
        foreach (string d in dirs)
        {
            string parent = Path.GetDirectoryName(d);
            if (!Directory.Exists(parent)) continue;
            foreach (string sub in Directory.GetDirectories(parent))
                if (Path.GetFileName(sub) == "mods") return sub;   // exact name: "mods" would also match the disc's MOD\
        }
        return null;
    }
    // Like KnownRuntimeDir; with no known folder, asks
    string RuntimeDir()
    {
        string known = KnownRuntimeDir();
        if (known != null) return known;
        string saved = Path.Combine(exeDir, ".wotm_recomp_dir");
        using (var dlg = new FolderBrowserDialog { Description = "Runtime folder (the one containing run_live.ps1)" })
        {
            if (dlg.ShowDialog(this) == DialogResult.OK && File.Exists(Path.Combine(dlg.SelectedPath, "run_live.ps1")))
            {
                File.WriteAllText(saved, dlg.SelectedPath + Environment.NewLine);
                return dlg.SelectedPath;
            }
        }
        return null;
    }

    bool On(string key)
    {
        foreach (var kv in boxes) if (kv.Key.Key == key) return kv.Value.Checked;
        return false;
    }

    // Play is the offline game and closes the launcher: inside a lobby (or with the server running here) that takes both down, and the
    // online match never comes through here (Start match -> MATCH line -> PlayOnline). So it asks first.
    void Play()
    {
        if (lobby != null || hosted != null)
        {
            string where = lobbyCode.Length > 0 ? "You are in online lobby " + lobbyCode + "." : "You are connected to an online server.";
            string how = lobbyOwner ? "The online match starts for everyone when you press Start match in the Online group."
                                    : "The online match starts when the host presses Start match: the game opens by itself.";
            string cost = "Play starts an offline game on this PC and closes the launcher" +
                (hosted != null ? ", which also stops the server you are hosting." : ", which leaves the lobby.");
            DialogResult r = MessageBox.Show(this, where + " " + how + "\n\n" + cost + "\n\nPlay offline anyway?", "WotM - options",
                MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (r != DialogResult.Yes) return;
        }
        ProcessStartInfo psi = GameStartInfo(matchOn.Checked);
        if (psi != null) Launch(psi);
    }

    // network match: the MATCH line's environment instead of the direct match; no AI-controlled player and no cheats (they would only
    // apply on this PC). The launcher does not close: it stays minimized and in the lobby (the connection stays up) and comes back when
    // the game closes (QUIT in the game or the window).
    void PlayOnline(Dictionary<string, string> env)
    {
        if (onlineGame != null && !onlineGame.HasExited) { netInfo.Text = "The game is already running."; return; }
        ProcessStartInfo psi = GameStartInfo(true);
        if (psi == null) return;
        foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
        psi.EnvironmentVariables.Remove("WOTM_AIVSAI");
        psi.EnvironmentVariables.Remove("WOTM_CHEATS");
        try
        {
            onlineGame = Process.Start(psi);
            onlineGame.EnableRaisingEvents = true;
            onlineGame.Exited += delegate
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    WindowState = FormWindowState.Normal;
                    Activate();
                    netInfo.Text = lobbyCode.Length > 0 ? "Match over. Back in lobby " + lobbyCode + (lobbyOwner ? ": press Start match for another." : ": waiting for the host.")
                                                        : "Match over.";
                });
            };
            WindowState = FormWindowState.Minimized;
            netInfo.Text = "Match running...";
        }
        catch (Exception e) { status.Text = "Failed: " + e.Message; }
    }

    void Launch(ProcessStartInfo psi)
    {
        try
        {
            Process.Start(psi);
            Close();   // the game carries on by itself (independent process); on error the window stays open with the message
        }
        catch (Exception e) { status.Text = "Failed: " + e.Message; }
    }

    ProcessStartInfo GameStartInfo(bool direct)
    {
        SaveIni();
        string rt = RuntimeDir();
        if (rt == null) { status.Text = "Runtime folder not found (run_live.ps1)."; return null; }
        // a direct match skips the menu: the scripted pad (-Auto 1) would press buttons mid-fight
        string args = string.Format("-NoProfile -ExecutionPolicy Bypass -File \"{0}\" -Auto {1} -Stats {2} -Build {3}",
            Path.Combine(rt, "run_live.ps1"), On(":Auto") && !direct ? 1 : 0, On(":Stats") ? 1 : 0, "build_dev");
        var psi = new ProcessStartInfo("powershell.exe", args) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = rt };
        psi.EnvironmentVariables["WOTM_REPO"] = exeDir;
        foreach (string k in SettingsKeys) psi.EnvironmentVariables.Remove(k);
        foreach (var kv in boxes)
        {
            string key = kv.Key.Key;
            if (key.StartsWith(":")) continue;
            string name = key, value = "1";
            int eq = key.IndexOf('=');
            if (eq > 0) { name = key.Substring(0, eq); value = key.Substring(eq + 1); }
            if (kv.Value.Checked) psi.EnvironmentVariables[name] = value;
            else psi.EnvironmentVariables.Remove(name);
        }
        if (matchOn.Checked) psi.EnvironmentVariables["WOTM_MATCH"] = MatchValue();
        else psi.EnvironmentVariables.Remove("WOTM_MATCH");
        string cheats = CheatsValue();
        if (cheats.Length > 0) psi.EnvironmentVariables["WOTM_CHEATS"] = cheats;
        else psi.EnvironmentVariables.Remove("WOTM_CHEATS");
        // all off: the panel's MODIFIERS tab (settings.ini) decides; a network match replaces this with the lobby's list
        string mods = ModsValue();
        if (mods.Length > 0) psi.EnvironmentVariables["WOTM_MODIFIERS"] = mods;
        else psi.EnvironmentVariables.Remove("WOTM_MODIFIERS");
        return psi;
    }

    // Mod kit (tools\modkit_gui.py, docs\passos\MODS.md): pythonw without a console; failing that, python
    void OpenModKit()
    {
        string script = Path.Combine(exeDir, "tools", "modkit_gui.py");
        if (!File.Exists(script)) { status.Text = "tools\\modkit_gui.py not found next to the launcher."; return; }
        foreach (string py in new[] { "pythonw.exe", "pyw.exe", "python.exe", "py.exe" })
        {
            try
            {
                Process.Start(new ProcessStartInfo(py, "\"" + script + "\"") { WorkingDirectory = exeDir, UseShellExecute = false });
                status.Text = "Mod kit opened.";
                return;
            }
            catch (Exception) { }
        }
        status.Text = "Python not found: install Python 3 (with Pillow: pip install pillow) to use the mod kit.";
    }

    void OpenLog()
    {
        string rt = RuntimeDir();
        string p = rt == null ? null : Path.Combine(rt, "logs", "live.err.log");
        if (p != null && !File.Exists(p)) p = Path.Combine(rt, "live.err.log");   // older run_live.ps1 writes it in the root
        if (p != null && File.Exists(p)) Process.Start("notepad.exe", "\"" + p + "\"");
        else status.Text = "No live.err.log yet.";
    }

    // Theme: follows the Windows app mode (Personalization > Colors). Dark = the colors below and a dark title bar (DWM).
    internal static readonly bool Dark = AppsUseDark();
    static readonly Color BgColor = Color.FromArgb(32, 32, 32), InputColor = Color.FromArgb(51, 51, 51), ButtonColor = Color.FromArgb(58, 58, 58);
    static readonly Color FgColor = Color.FromArgb(232, 232, 232);
    static readonly Color DimColor = Dark ? Color.FromArgb(150, 150, 150) : SystemColors.GrayText;

    static bool AppsUseDark()
    {
        try
        {
            object v = Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize", "AppsUseLightTheme", 1);
            return v is int && (int)v == 0;
        }
        catch (Exception) { return false; }
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    static extern int SetWindowTheme(IntPtr hwnd, string appName, string idList);
    // SetPreferredAppMode (no exported name, ordinal 135, Windows 10 1903+): 1 = follow the system; without it the open dropdown is light
    [DllImport("uxtheme.dll", EntryPoint = "#135")]
    static extern int SetPreferredAppMode(int mode);

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (!Dark) return;
        int on = 1;
        try
        {   // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 on Windows 10 20H1+ and 11, 19 before
            if (DwmSetWindowAttribute(Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(Handle, 19, ref on, 4);
        }
        catch (Exception) { }
    }

    // Text in the default color becomes light; chosen colors (gray notes, red warning) stay.
    static void ApplyDark(Control c)
    {
        if (c.ForeColor == SystemColors.ControlText) c.ForeColor = FgColor;
        if (c is ComboBox)
        {   // native Windows dark theme (the file dialog one): readable even when disabled, unlike FlatStyle with colors
            c.BackColor = InputColor;
            c.ForeColor = FgColor;   // the ComboBox default is WindowText (not ControlText): the open list's items came out black
            Control cc = c;
            EventHandler theme = delegate { try { SetWindowTheme(cc.Handle, "DarkMode_CFD", null); } catch (Exception) { } };
            if (c.IsHandleCreated) theme(c, EventArgs.Empty);
            else c.HandleCreated += theme;
        }
        else if (c is TextBox)
        {
            c.BackColor = InputColor;
            ((TextBox)c).BorderStyle = BorderStyle.FixedSingle;
        }
        else if (c is NumericUpDown)
        {   // the color must go to the inner text box (UpDownEdit) too
            c.BackColor = InputColor;
            ((NumericUpDown)c).BorderStyle = BorderStyle.FixedSingle;
            foreach (Control k in c.Controls) if (k is TextBox) { k.BackColor = InputColor; k.ForeColor = FgColor; }
        }
        else if (c is Button)
        {
            var b = (Button)c;
            b.FlatStyle = FlatStyle.Flat;
            b.BackColor = ButtonColor;
            b.FlatAppearance.BorderColor = Color.FromArgb(100, 100, 100);
        }
        else c.BackColor = BgColor;
        if (!(c is NumericUpDown)) foreach (Control k in c.Controls) ApplyDark(k);
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        if (Dark) try { SetPreferredAppMode(1); } catch (Exception) { }
        Application.Run(new Launcher());
    }
}
