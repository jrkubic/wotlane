// mods/ folder (MODS.md): files that override the disc's, without touching the ISO.
// Each subfolder of mods/ is a mod and mirrors the disc tree (mods/my_mod/MON/CONGAR1.TEX replaces \MON\CONGAR1.TEX). Folders
// starting with '_' or '.' are disabled. When two mods replace the same file, the last one in alphabetical order wins (like
// layers: "10_base", "20_extra"); the log says which one lost.
// The game finds files through fileCdSearchFile (sector and size) and reads by sector; each mod file gets a range of
// fake sectors beyond the end of any DVD (kFirstLsn), which the hooks (runtime/wotm_mods.inc) and the native sound reader
// serve from here. The native layer (wotm_disc.hpp: movies, icon, photos and the save list's monster) looks things up here by name.
// Host only: doesn't depend on the runtime. The scan runs once, before boot (scan); after that everything is read-only.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "wotm_json.hpp"

namespace wotm::mods {

// Playable monsters: type (MonsterNewNames, m_monsterSel >> 5), in-game name and file prefix in MON/
struct MonsterInfo { int type; const char *name; const char *prefix; };
inline constexpr MonsterInfo kMonsters[] = {
    {1, "congar", "MONKEY"}, {2, "robo-47", "ROBOT"}, {3, "togera", "LIZARD"}, {4, "ultra-v", "SHOGUN"}, {5, "preytor", "MANTIS"},
    {7, "kineticlops", "ENERGY"}, {8, "magmo", "LAVA"}, {9, "raptros", "DRAGON"}, {10, "agamo", "ROCK"}, {11, "zorgulon", "ALIEN"},
};
// By name ("togera", "Robo-47", "robo47") or by prefix ("LIZARD")
inline const MonsterInfo *monsterInfo(std::string s) {
    auto norm = [](std::string t) {
        std::string o;
        for (char c : t) if (std::isalnum(static_cast<unsigned char>(c))) o += char(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    s = norm(s);
    for (const MonsterInfo &m : kMonsters)
        if (s == norm(m.name) || s == norm(m.prefix)) return &m;
    return nullptr;
}
inline const MonsterInfo *monsterInfo(int type) {
    for (const MonsterInfo &m : kMonsters)
        if (m.type == type) return &m;
    return nullptr;
}

// Types a custom monster can use as its base: the playable ones and the bosses the port already lets you play (runtime/wotm_custom.inc)
inline constexpr MonsterInfo kCustomBases[] = {{13, "assboss", "ASSBOSS"}, {14, "jelly", "JELLY"}, {15, "final", "FINAL"}};
inline const MonsterInfo *customBase(const std::string &s) {
    if (const MonsterInfo *m = monsterInfo(s)) return m;
    for (const MonsterInfo &m : kCustomBases)
        if (s == m.name || s == m.prefix) return &m;
    return nullptr;
}

// Custom monster (mods/<mod>/monsters/<folder>/monster.json; MODS.md, "Custom monsters"): a new monster on the select screen (CUSTOM
// slot), which for now uses the code, model and files of one of the game's types (`base`). Its own files (another prefix, a free
// type) come later; the format already reserves "files".
struct Custom {
    std::string id;             // "goliath" (WOTM_MATCH=goliath, the save)
    std::string name;           // what the select screen and the HUD show ("GOLIATH")
    int type = 0;               // game type (13 = assboss)
    std::string typeName;       // "assboss"
    std::string prefix;         // "ASSBOSS"
    int costume = 0;            // disc costume
    std::string cameraFrom;     // MonsterLongNames of the .CHR to take the camera from ("lizard"); empty = its own type's
    float cameraScale = 1.f;    // distances for the borrowed camera
    std::string narrator;       // narrator/<name>.mp3 (empty = the type's)
    bool ai = false;            // a CPU can use it (CPU slot on the select screen)
    std::filesystem::path portrait;   // HUD portrait PNG (empty = the one from the .RTX)
    int portraitTex = -1;       // portrait texId in <PREFIX><costume>.RTX
    std::string mod;
    std::filesystem::path dir;
};

// Mod costume (mods/<mod>/skins/<folder>/skin.json)
struct Skin {
    int monster = 0;            // monster type
    std::string monsterName;    // "togera" (in a custom's skin, the custom's id)
    std::string custom;         // id of the custom monster that owns the skin (empty: skin for a game monster); only shows up in its list
    std::string prefix;         // "LIZARD"
    std::string id;             // "green" (the save stores "togera/green")
    std::string name;           // what the select screen shows
    int base = 0;               // disc costume that supplies whatever is missing (and the costume's sounds)
    bool ai = false;            // randomly picked AIs can use it
    int index = 0;              // in-game costume (4..99), for this run only
    std::string mod;
    std::filesystem::path dir;
};
// A skin's PNG: only applies to its own costume's file (stem "LIZARD4"), never to the base costume
struct SkinPng { std::string stem; std::filesystem::path host; std::string mod, rel; };

struct File {
    std::string rel;              // "MON/CONGAR1.TEX" (uppercase, '/')
    std::filesystem::path host;   // file on the PC
    std::string mod;              // mod folder name
    uint64_t size = 0;
    uint32_t lsn = 0, sectors = 0;   // fake range: [lsn, lsn + sectors)
    mutable bool logged = false;     // the game's first lookup has already been logged
};

// Digest of the enabled mods in `dir` (subfolders not starting with '_' or '.'): path and contents of every file, in name order.
// Network play puts it in the config hash (every PC with the same mods). 0 = no mod files.
inline uint64_t digest(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
    std::vector<std::pair<std::string, fs::path>> files;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (!e.is_directory(ec) || n.empty() || n[0] == '_' || n[0] == '.') continue;
        for (auto it = fs::recursive_directory_iterator(e.path(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec))
                files.emplace_back(fs::relative(it->path(), dir, ec).generic_string(), it->path());
    }
    if (files.empty()) return 0;
    std::sort(files.begin(), files.end());
    uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a
    auto mix = [&h](const char *p, size_t n) { for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(p[i])) * 0x100000001b3ull; };
    char buf[1 << 16];
    for (const auto &[rel, host] : files) {
        mix(rel.c_str(), rel.size() + 1);
        std::ifstream in(host, std::ios::binary);
        while (in.read(buf, sizeof buf) || in.gcount() > 0) mix(buf, size_t(in.gcount()));
        mix("\0", 1);
    }
    return h ? h : 1;
}

class Registry {
public:
    static constexpr uint32_t kFirstLsn = 0x00800000u;   // 16 GB in 2 KB sectors: beyond any DVD (~2.3 M sectors)
    static constexpr uint64_t kMaxSize = 256ull << 20;

    static Registry &get() { static Registry r; return r; }

    static std::string key(std::string rel) {
        std::replace(rel.begin(), rel.end(), '\\', '/');
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        for (char &c : rel) c = char(std::toupper(static_cast<unsigned char>(c)));
        return rel;
    }
    // Name as the game builds it ("\LVL\CENTRAL.NGP;1", "cdrom0:\SND\SHELL_S.BNK;1") -> "LVL/CENTRAL.NGP"
    static std::string keyFromGame(std::string name) {
        if (const auto c = name.find(':'); c != std::string::npos) name = name.substr(c + 1);
        if (const auto sc = name.find(';'); sc != std::string::npos) name = name.substr(0, sc);
        return key(name);
    }

    // Reads the mods/ folder (once). Returns how many files were added.
    size_t scan(const std::filesystem::path &dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        m_dir = dir;
        if (dir.empty() || !fs::is_directory(dir, ec)) { m_ready = true; return 0; }
        std::vector<fs::path> mods;
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (e.is_directory(ec) && !n.empty() && n[0] != '_' && n[0] != '.') mods.push_back(e.path());
        }
        std::sort(mods.begin(), mods.end(), [](const fs::path &a, const fs::path &b) { return key(a.filename().string()) < key(b.filename().string()); });
        for (const fs::path &m : mods) {
            m_mods.push_back(m.filename().string());
            for (auto it = fs::recursive_directory_iterator(m, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string rel = key(fs::relative(it->path(), m, ec).generic_string());
                const uint64_t size = it->file_size(ec);
                if (rel.empty() || ec) continue;
                if (rel.rfind("SKINS/", 0) == 0) continue;   // skins/<id>/: new costumes (scanSkins), don't override the disc
                if (rel.rfind("MONSTERS/", 0) == 0) continue;   // monsters/<id>/: custom monsters (scanCustoms)
                if (size > kMaxSize) { std::fprintf(stderr, "[mods] %s/%s ignored: larger than 256 MB\n", m_mods.back().c_str(), rel.c_str()); continue; }
                File &f = m_files[rel];
                if (!f.mod.empty()) std::fprintf(stderr, "[mods] %s: %s beats %s\n", rel.c_str(), m_mods.back().c_str(), f.mod.c_str());
                f.rel = rel;
                f.host = it->path();
                f.mod = m_mods.back();
                f.size = size;
            }
        }
        scanCustoms(mods);   // before the skins: a skin can belong to a custom
        scanSkins(mods);
        // Sector ranges in name order (stable across runs), with one spare sector between files
        uint32_t next = kFirstLsn;
        for (auto &[rel, f] : m_files) {
            f.sectors = uint32_t((f.size + 2047) / 2048);
            f.lsn = next;
            next += std::max(f.sectors, 1u) + 1;
            m_byLsn.push_back(&f);
        }
        m_ready = true;
        return m_files.size() + m_alias.size();
    }

    bool ready() const { return m_ready; }
    const std::filesystem::path &dir() const { return m_dir; }
    const std::vector<std::string> &modNames() const { return m_mods; }
    size_t count() const { return m_files.size(); }
    const std::map<std::string, File> &files() const { return m_files; }

    const File *find(const std::string &rel) const {
        if (!m_ready || m_files.empty()) return nullptr;
        const auto it = m_files.find(key(rel));
        return it == m_files.end() ? nullptr : &it->second;
    }
    // The base costume's file that a mod costume uses in place of what the skin folder didn't provide ("MON/LIZARD4.NGP" ->
    // "MON/LIZARD1.NGP"); otherwise the name itself. What gets read from disc is always aliasOf(rel).
    std::string aliasOf(const std::string &rel) const {
        if (!m_ready || m_alias.empty()) return key(rel);
        const auto it = m_alias.find(key(rel));
        return it == m_alias.end() ? key(rel) : it->second;
    }
    std::string hostPath(const std::string &rel) const {
        const File *f = find(aliasOf(rel));
        return f ? f->host.string() : std::string{};
    }

    // ---- mod skins (costumes 4 and up) ----
    const std::vector<Skin> &skins() const { return m_skins; }
    // Skin by monster type (MonsterNewNames) and by id ("green") or "monster/id"; nullptr if there's none.
    // With "monster/id", the monster has to match (the same id can exist on Togera and on a Togera-based custom).
    const Skin *skin(int monster, std::string id) const {
        std::string owner;
        if (const auto s = id.find('/'); s != std::string::npos) owner = lower(id.substr(0, s)), id = id.substr(s + 1);
        for (const Skin &k : m_skins)
            if (k.monster == monster && k.id == lower(id) && (owner.empty() || owner == k.monsterName)) return &k;
        return nullptr;
    }
    // A custom monster's costumes: the one from monster.json and its skins, in that order
    std::vector<int> customCostumes(const Custom &c) const {
        std::vector<int> v{c.costume};
        for (const Skin &k : m_skins)
            if (k.custom == c.id) v.push_back(k.index);
        return v;
    }
    const Skin *skinByIndex(int monster, int index) const {
        for (const Skin &k : m_skins)
            if (k.monster == monster && k.index == index) return &k;
        return nullptr;
    }
    const std::vector<SkinPng> &skinPngs() const { return m_skinPngs; }

    // ---- custom monsters ----
    const std::vector<Custom> &customs() const { return m_customs; }
    const Custom *custom(const std::string &id) const {
        for (const Custom &c : m_customs)
            if (c.id == lower(id)) return &c;
        return nullptr;
    }
    // The custom that uses this type and costume (the first one), for the fight hooks
    const Custom *customByType(int type, int costume = -1) const {
        for (const Custom &c : m_customs)
            if (c.type == type && (costume < 0 || c.costume == costume)) return &c;
        return nullptr;
    }
    static bool isModLsn(uint32_t lsn) { return lsn >= kFirstLsn; }
    const File *findLsn(uint32_t lsn) const {
        if (!isModLsn(lsn)) return nullptr;
        auto it = std::upper_bound(m_byLsn.begin(), m_byLsn.end(), lsn, [](uint32_t l, const File *f) { return l < f->lsn; });
        if (it == m_byLsn.begin()) return nullptr;
        const File *f = *(it - 1);
        return lsn < f->lsn + std::max(f->sectors, 1u) ? f : nullptr;
    }

    // Reads `sectors` sectors starting at the fake LSN; anything past the end of the file comes back zeroed. false: LSN outside the mods.
    bool read(uint32_t lsn, uint32_t sectors, uint8_t *dst, size_t bytes) const {
        const File *f = findLsn(lsn);
        if (!f) return false;
        std::memset(dst, 0, bytes);
        const uint64_t off = uint64_t(lsn - f->lsn) * 2048;
        if (off >= f->size) return true;
        const size_t n = size_t(std::min<uint64_t>({uint64_t(sectors) * 2048, uint64_t(bytes), f->size - off}));
        std::ifstream in(f->host, std::ios::binary);
        if (!in || !in.seekg(static_cast<std::streamoff>(off))) return false;
        in.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(n));
        return in.gcount() == static_cast<std::streamsize>(n);
    }

private:
    std::filesystem::path m_dir;
    std::vector<std::string> m_mods;
    std::map<std::string, File> m_files;
    std::vector<const File *> m_byLsn;   // in LSN order (the map is already in name order, and the LSNs follow that order)
    std::map<std::string, std::string> m_alias;   // "MON/LIZARD4.NGP" -> "MON/LIZARD1.NGP"
    std::vector<Skin> m_skins;
    std::vector<SkinPng> m_skinPngs;
    std::vector<Custom> m_customs;
    std::atomic<bool> m_ready{false};

    static std::string lower(std::string s) {
        for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    // Child of `dir` with this name, case-insensitive
    static std::filesystem::path child(const std::filesystem::path &dir, const std::string &name) {
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir, ec))
            if (key(e.path().filename().string()) == key(name)) return e.path();
        return {};
    }

    // mods/<mod>/skins/<folder>/skin.json (MODS.md, "Skins"): {"monster", "id", "name", "base", "ai"}. Each skin becomes costume
    // <PREFIX><n> (n = 4, 5, ... per monster, in mod/id order; for this run only: the save stores "monster/id"). Whatever the folder brings
    // (.NGP+.PTR, .TEX, .RTX, PNGs) is its own; whatever is missing is the base costume's file (m_alias).
    void scanSkins(const std::vector<std::filesystem::path> &mods) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::map<std::string, Skin> byKey;   // "togera/green"
        for (const fs::path &m : mods) {
            const fs::path dir = child(m, "skins");
            if (dir.empty() || !fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> subs;
            for (const auto &e : fs::directory_iterator(dir, ec))
                if (e.is_directory(ec)) subs.push_back(e.path());
            std::sort(subs.begin(), subs.end());
            for (const fs::path &s : subs) {
                const std::string where = m.filename().string() + "/skins/" + s.filename().string();
                const fs::path js = child(s, "skin.json");
                std::string text;
                if (!js.empty()) {
                    std::ifstream in(js, std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                json::Value v;
                std::string err;
                if (js.empty() || !json::parse(text, v, &err) || v.type != json::Value::Obj) {
                    std::fprintf(stderr, "[mods] skin %s ignored: %s\n", where.c_str(), js.empty() ? "no skin.json" : ("invalid skin.json: " + err).c_str());
                    continue;
                }
                Skin k;
                const json::Value *mon = v.get("monster");
                const MonsterInfo *mi = mon && mon->type == json::Value::Str ? monsterInfo(mon->s) : nullptr;
                const Custom *cu = !mi && mon && mon->type == json::Value::Str ? custom(mon->s) : nullptr;
                if (!mi && !cu) {
                    std::fprintf(stderr, "[mods] skin %s ignored: \"monster\" is neither a playable monster (togera, congar, ...) nor a custom monster's id\n", where.c_str());
                    continue;
                }
                if (cu) {   // custom's skin: a costume of the base type, offered only in the custom's list
                    k.monster = cu->type;
                    k.monsterName = cu->id;
                    k.custom = cu->id;
                    k.prefix = cu->prefix;
                } else {
                    k.monster = mi->type;
                    k.monsterName = mi->name;
                    k.prefix = mi->prefix;
                }
                const json::Value *id = v.get("id");
                k.id = lower(id && id->type == json::Value::Str ? id->s : s.filename().string());
                if (k.id.empty() || k.id.size() > 32 || k.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
                    std::fprintf(stderr, "[mods] skin %s ignored: \"id\" must be only letters, digits and _ (up to 32)\n", where.c_str());
                    continue;
                }
                const json::Value *nm = v.get("name");
                k.name = nm && nm->type == json::Value::Str && !nm->s.empty() ? nm->s : k.id;
                const json::Value *base = v.get("base");
                k.base = base ? int(base->asInt(0)) : cu ? cu->costume : 0;
                if (k.base < 0 || k.base > 3) {
                    std::fprintf(stderr, "[mods] skin %s: \"base\" %d outside 0..3, using 0\n", where.c_str(), k.base);
                    k.base = 0;
                }
                const json::Value *ai = v.get("ai");
                k.ai = ai && ai->asBool(false);
                k.mod = m.filename().string();
                k.dir = s;
                Skin &slot = byKey[k.monsterName + "/" + k.id];
                if (!slot.id.empty())
                    std::fprintf(stderr, "[mods] skin %s/%s: %s beats %s\n", k.monsterName.c_str(), k.id.c_str(), k.mod.c_str(), slot.mod.c_str());
                slot = k;
            }
        }
        for (auto &[name, k] : byKey) m_skins.push_back(k);
        std::stable_sort(m_skins.begin(), m_skins.end(), [](const Skin &a, const Skin &b) {
            return a.monster != b.monster ? a.monster < b.monster : key(a.mod) != key(b.mod) ? key(a.mod) < key(b.mod) : a.id < b.id;
        });
        std::map<int, int> next;
        std::vector<Skin> kept;
        for (Skin &k : m_skins) {
            int &n = next.emplace(k.monster, 4).first->second;
            const std::string stem = k.prefix + std::to_string(n);
            if (n > 99 || stem.size() > 8) {   // fileStatus stores names of up to 8 letters
                std::fprintf(stderr, "[mods] skin %s/%s ignored: too many costumes for %s\n", k.monsterName.c_str(), k.id.c_str(), k.monsterName.c_str());
                continue;
            }
            std::map<std::string, fs::path> own;   // extension -> file in the folder
            std::vector<fs::path> pngs;
            for (auto it = fs::recursive_directory_iterator(k.dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = key(it->path().extension().string());
                if (ext == ".PNG") pngs.push_back(it->path());
                else if (ext == ".NGP" || ext == ".PTR" || ext == ".TEX" || ext == ".RTX") {
                    auto &o = own[ext.substr(1)];
                    if (!o.empty()) std::fprintf(stderr, "[mods] skin %s/%s: two .%s, using %s\n", k.monsterName.c_str(), k.id.c_str(), ext.c_str() + 1, o.filename().string().c_str());
                    else o = it->path();
                }
            }
            if (own.count("NGP") != own.count("PTR")) {
                std::fprintf(stderr, "[mods] skin %s/%s ignored: .NGP and .PTR go together (the .PTR relocates the .NGP)\n", k.monsterName.c_str(), k.id.c_str());
                continue;
            }
            k.index = n++;
            std::string brought;
            for (const char *ext : {"NGP", "PTR", "TEX", "RTX"}) {
                const std::string rel = "MON/" + stem + "." + ext;
                if (const auto o = own.find(ext); o != own.end()) {
                    File &f = m_files[rel];
                    f.rel = rel;
                    f.host = o->second;
                    f.mod = k.mod;
                    f.size = fs::file_size(o->second, ec);
                    brought += std::string(brought.empty() ? "" : " ") + ext;
                } else
                    m_alias[rel] = "MON/" + k.prefix + std::to_string(k.base) + "." + ext;
            }
            for (const fs::path &p : pngs) m_skinPngs.push_back(SkinPng{stem, p, k.mod, k.monsterName + "/" + k.id + "/" + p.filename().string()});
            std::fprintf(stderr, "[mods] skin %s/%s (\"%s\", %s) = costume %d (%s); base %d%s%s, %zu png\n", k.monsterName.c_str(), k.id.c_str(),
                         k.name.c_str(), k.mod.c_str(), k.index, stem.c_str(), k.base, brought.empty() ? "" : "; from the folder: ", brought.c_str(), pngs.size());
            kept.push_back(k);
        }
        m_skins = std::move(kept);
    }

    // mods/<mod>/monsters/<folder>/monster.json: {"id", "name", "base", "costume", "camera": {"from", "scale" (%)}, "narrator", "ai"}.
    // Same id in two mods: the last one in alphabetical order wins, like the files.
    void scanCustoms(const std::vector<std::filesystem::path> &mods) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::map<std::string, Custom> byId;
        for (const fs::path &m : mods) {
            const fs::path dir = child(m, "monsters");
            if (dir.empty() || !fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> subs;
            for (const auto &e : fs::directory_iterator(dir, ec))
                if (e.is_directory(ec)) subs.push_back(e.path());
            std::sort(subs.begin(), subs.end());
            for (const fs::path &s : subs) {
                const std::string where = m.filename().string() + "/monsters/" + s.filename().string();
                const fs::path js = child(s, "monster.json");
                std::string text;
                if (!js.empty()) {
                    std::ifstream in(js, std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                json::Value v;
                std::string err;
                if (js.empty() || !json::parse(text, v, &err) || v.type != json::Value::Obj) {
                    std::fprintf(stderr, "[mods] monster %s ignored: %s\n", where.c_str(), js.empty() ? "no monster.json" : ("invalid monster.json: " + err).c_str());
                    continue;
                }
                Custom c;
                const json::Value *base = v.get("base");
                const MonsterInfo *mi = base && base->type == json::Value::Str ? customBase(lower(base->s)) : nullptr;
                if (!mi) {
                    std::fprintf(stderr, "[mods] monster %s ignored: \"base\" is not an accepted type (togera, congar, ..., assboss, jelly, final)\n", where.c_str());
                    continue;
                }
                c.type = mi->type;
                c.typeName = mi->name;
                c.prefix = mi->prefix;
                const json::Value *id = v.get("id");
                c.id = lower(id && id->type == json::Value::Str ? id->s : s.filename().string());
                if (c.id.empty() || c.id.size() > 32 || c.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
                    std::isdigit(static_cast<unsigned char>(c.id[0]))) {
                    std::fprintf(stderr, "[mods] monster %s ignored: \"id\" must be only letters, digits and _ (up to 32, starting with a letter)\n", where.c_str());
                    continue;
                }
                const json::Value *nm = v.get("name");
                c.name = nm && nm->type == json::Value::Str && !nm->s.empty() ? nm->s : c.id;
                const json::Value *co = v.get("costume");
                c.costume = co ? int(co->asInt(0)) : 0;
                if (c.costume < 0 || c.costume > 3) c.costume = 0;
                if (const json::Value *cam = v.get("camera"); cam && cam->type == json::Value::Obj) {
                    if (const json::Value *f = cam->get("from"); f && f->type == json::Value::Str)
                        if (const MonsterInfo *b = customBase(lower(f->s))) c.cameraFrom = kLongNames[b->type];
                    if (const json::Value *sc = cam->get("scale"); sc && sc->type == json::Value::Int && sc->i >= 10 && sc->i <= 1000)   // in %
                        c.cameraScale = float(sc->i) / 100.f;
                }
                if (const json::Value *na = v.get("narrator"); na && na->type == json::Value::Str) c.narrator = lower(na->s);
                // "portrait": HUD portrait PNG (in the monster's folder or the mod root), replacing the <PREFIX><costume>.RTX image
                // with texId "portraitTex" (default: ASSBOSS1's, 6; the last entry in the .RTX, 128x64)
                if (const json::Value *po = v.get("portrait"); po && po->type == json::Value::Str && !po->s.empty()) {
                    fs::path p = s / fs::u8path(po->s);
                    if (!fs::is_regular_file(p, ec)) p = m / fs::u8path(po->s);
                    const json::Value *pt = v.get("portraitTex");
                    c.portraitTex = pt && pt->type == json::Value::Int ? int(pt->i) : (c.type == 13 && c.costume == 1 ? 6 : -1);
                    if (!fs::is_regular_file(p, ec))
                        std::fprintf(stderr, "[mods] monster %s: \"portrait\" %s not found\n", where.c_str(), po->s.c_str());
                    else if (c.portraitTex < 0)
                        std::fprintf(stderr, "[mods] monster %s: \"portrait\" without \"portraitTex\" (portrait texId in the .RTX)\n", where.c_str());
                    else
                        c.portrait = p;
                }
                const json::Value *ai = v.get("ai");
                c.ai = ai && ai->asBool(false);
                c.mod = m.filename().string();
                c.dir = s;
                Custom &slot = byId[c.id];
                if (!slot.id.empty())
                    std::fprintf(stderr, "[mods] monster %s: %s beats %s\n", c.id.c_str(), c.mod.c_str(), slot.mod.c_str());
                slot = c;
            }
        }
        for (auto &[id, c] : byId) {
            std::fprintf(stderr, "[mods] custom monster %s (\"%s\", %s): base %s costume %d%s%s\n", c.id.c_str(), c.name.c_str(), c.mod.c_str(),
                         c.typeName.c_str(), c.costume, c.cameraFrom.empty() ? "" : ", camera from ", c.cameraFrom.c_str());
            m_customs.push_back(c);
        }
    }
    // MonsterLongNames (prefix of the .CHR in RST/ and of the files in MON/, lowercase), index = type
    static constexpr const char *kLongNames[] = {"null", "monkey", "robot", "lizard", "shogun", "mantis", "spider", "energy", "lava",
                                                 "dragon", "rock", "alien", "ogre", "assboss", "jelly", "final", nullptr};
};

} // namespace wotm::mods
