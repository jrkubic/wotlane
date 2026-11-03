// wotm_narrator.inc: narrator for the selection screens (included by wotm_live.inc, before wotm_padsel.inc). On confirming a monster
// (native screens 3 and 6, 3- and 4-player select) or a stage (game screen 8: the confirm's changeScreen(8, 99), hooked in
// wotm_padsel.inc) it plays the MP3 with its name, together with the game's confirm sound (not instead of it).
// NARRATOR option on the FEATURES tab of "Recompiled options" (WOTM_ANNOUNCER): 0 OFF, 1 GODZILLA SAVE THE EARTH (default, the MP3s),
// 2 MONSTER CRY: instead of the monster's name, its cry (the sound from the fight's intro cinematic, from the monster's bank;
// playMonsterCry in wotm_snd.inc); a monster with no cry (Goliath Prime) stays silent; stages and the title screen still use the MP3s.
//
// Files <name>.mp3: monsters (congar, robo47, togera, ultrav, preytor, kineticlops, magmo, raptros, agamo, zorgulon,
// goliathprime, cerebulon) and stages (levelName: midtownpark, gamblersgulch, rosdalecanyon, metrocity, ...), searched in this order:
// mods/<mod>/narrator/ (the usual mods/ folder: a mod can replace the voice), narrator/ next to the ELF and narrator/ at the repo
// root (the build's). A missing file means silence and one log line.
// Played through raylib (LoadSound/PlaySound, MP3 enabled in the raylib build), at the game's master effects volume (group 0 of the
// native sound, the same quadratic curve as 989snd). One narration at a time: a new name cuts off the previous one.
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace wotm_narrator
{
namespace fs = std::filesystem;

static std::mutex g_m;
static std::map<std::string, Sound> g_sounds;   // by file name; empty Sound (frameCount 0) = not found
static Sound *g_playing = nullptr;

// Files for each monster (game index, m_monsterSel >> 5), in order of preference
static const char *const *namesOf(int monster)
{
    static const char *const kNone[] = {nullptr};
    static const char *const kNames[16][3] = {
        {nullptr},           {"congar", nullptr},  {"robo47", nullptr},      {"togera", nullptr},
        {"ultrav", nullptr}, {"preytor", nullptr}, {nullptr},                {"kineticlops", nullptr},
        {"magmo", nullptr},  {"raptros", nullptr}, {"agamo", nullptr},       {"zorgulon", nullptr},
        {nullptr},           {"goliathprime", "assboss", nullptr},           {nullptr},
        {"cerebulon", nullptr}};
    return monster >= 0 && monster < 16 ? kNames[monster] : kNone;
}

static fs::path findFile(const std::string &name)
{
    std::error_code ec;
    const std::string file = name + ".mp3";
    const std::string host = wotm::mods::Registry::get().hostPath("NARRATOR/" + file);
    if (!host.empty())
        return host;
    const fs::path modsDir = wotm::mods::Registry::get().dir();
    const fs::path dirs[] = {modsDir.empty() ? fs::path{} : modsDir.parent_path() / "narrator",
                             fs::path(__FILE__).parent_path().parent_path().parent_path() / "narrator"};   // native/runtime -> repo root
    for (const fs::path &d : dirs)
        if (!d.empty() && fs::is_regular_file(d / file, ec))
            return d / file;
    return {};
}

static Sound *soundOf(const std::string &name)
{
    auto it = g_sounds.find(name);
    if (it == g_sounds.end())
    {
        Sound s{};
        const fs::path p = findFile(name);
        if (!p.empty())
            s = LoadSound(p.string().c_str());
        if (s.frameCount == 0)
            std::fprintf(stderr, "[narrator] %s.mp3 %s\n", name.c_str(), p.empty() ? "not found" : "unreadable");
        else
            std::fprintf(stderr, "[narrator] %s\n", p.string().c_str());
        it = g_sounds.emplace(name, s).first;
    }
    return it->second.frameCount ? &it->second : nullptr;
}

// Master effects volume (0..0x400) -> 0..1, with 989snd's curve
static float effectsVolume()
{
#if defined(WOTM_HAVE_SND)
    const float f = float(wotm::snd::Engine::get().masterVolume(0)) / float(0x400);
    return std::clamp(f * f, 0.f, 1.f);
#else
    return 1.f;
#endif
}

// File for each stage (Shell::m_levelNum, GameLevelNames; GLOSSARIO.md, "Stages"). The dev team's test stages (17..24) have none.
static const char *levelName(int level)
{
    static const char *const kLevels[28] = {
        nullptr,         "midtownpark",     "gamblersgulch", "rosdalecanyon", "metrocity",  "centuryairfield", "atomicisland",
        "baytown",       "clubcaldera",     "tsunopolis",    "ufo",           "capitol",    "training",        "lakepowell",
        "minibaytown",   "volcano",         "tokyodeathmatch", nullptr,       nullptr,      nullptr,           nullptr,
        nullptr,         nullptr,           nullptr,         nullptr,         "dodgeball",  "bigshot",         "crushorama"};
    return level >= 0 && level < 28 ? kLevels[level] : nullptr;
}

// Plays the first file found from the list (nullptr-terminated). Called from the EE thread.
static void play(const char *const *names)
{
    if (!wotm::settings::on(wotm::settings::Announcer) || !IsAudioDeviceReady())
        return;
    std::lock_guard<std::mutex> lk(g_m);
    Sound *s = nullptr;
    for (const char *const *n = names; *n && !s; ++n)
        s = soundOf(*n);
    if (!s)
        return;
    if (g_playing && IsSoundPlaying(*g_playing))
        StopSound(*g_playing);
    SetSoundVolume(*s, effectsVolume());
    PlaySound(*s);
    g_playing = s;
}

// MONSTER CRY: the monster's cry instead of its name (cuts off any MP3 playing; no cry means silence). false = another option, use the MP3
static bool cry(int monster)
{
    if (wotm::settings::get(wotm::settings::Announcer) != 2)
        return false;
    std::lock_guard<std::mutex> lk(g_m);
    if (g_playing && IsSoundPlaying(*g_playing))
        StopSound(*g_playing);
#if defined(WOTM_HAVE_SND)
    wotm_snd_hooks::playMonsterCry(monster);
#else
    (void)monster;
#endif
    return true;
}

// The monster's name (game index, m_monsterSel >> 5)
static void announce(int monster)
{
    if (!cry(monster))
        play(namesOf(monster));
}
// Custom monster: the "narrator" from monster.json and, if that file is missing, its base type's (with MONSTER CRY, the base type's cry)
static void announce(const std::string &name, int monster)
{
    if (cry(monster))
        return;
    const char *const *t = namesOf(monster);
    const char *names[4] = {name.empty() ? nullptr : name.c_str(), nullptr, nullptr, nullptr};
    for (int i = 0, k = names[0] ? 1 : 0; t[i] && k < 3; ++i) names[k++] = t[i];
    play(names);
}

// introsound.mp3 on the "press start" screen (20), once per game launch: on the first call to screenWaitForStart, which uiMain
// only makes once the transition ends (camera animation and fade, betweenScreens 0). Returning to the title later doesn't repeat it.
static void announceIntro()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    static const char *const names[] = {"introsound", nullptr};
    play(names);
}

// Start on the title screen (changeScreen(20, 1)): cuts off introsound if it's still playing
static void stopIntro()
{
    std::lock_guard<std::mutex> lk(g_m);
    const auto it = g_sounds.find("introsound");
    if (it != g_sounds.end() && it->second.frameCount && IsSoundPlaying(it->second))
    {
        StopSound(it->second);
        std::fprintf(stderr, "[narrator] introsound cut off by Start\n");
    }
}

// The stage's name (m_levelNum), on confirming the stage select
static void announceLevel(int level)
{
    const char *const names[] = {levelName(level), nullptr};
    if (names[0])
        play(names);
}
} // namespace wotm_narrator
