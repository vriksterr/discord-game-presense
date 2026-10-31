// Game Presence Mirror v2
//
// Gaming PC  : watches for the games you selected and reports them over the LAN.
// Streaming PC: receives the reports and publishes them to the local Discord client
//               through Discord's Rich Presence IPC pipe, so friends see "Playing <game>".
//
// Discord's public "detectable applications" list maps executable names to Discord
// application IDs; that is how a game is shown with its real name and icon.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr unsigned short kDefaultPort = 47831;
unsigned short g_port = kDefaultPort;  // TCP port between the two PCs; set with --port
constexpr int kReportSeconds = 1;           // sender -> receiver, and how often `auto` scans
constexpr int kDiscordRefreshSeconds = 5;   // receiver -> Discord: unchanged activity is re-sent at most this often
                                            // (Discord rate-limits presence updates to roughly 5 per 20 seconds)
constexpr int kExpirySeconds = 20;  // receiver clears presence if the sender goes silent
constexpr char kMagic[] = "GPR2";

// ---------------------------------------------------------------- text helpers

std::string to_utf8(const std::wstring& value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring from_utf8(const std::string& value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

std::string trim(const std::string& value) {
    size_t a = value.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return value.substr(a, value.find_last_not_of(" \t\r\n") - a + 1);
}

bool all_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}

fs::path exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// ---------------------------------------------------------------- tiny JSON parser

struct Json {
    char type = 'n';  // n=null/bool/number, s=string, a=array, o=object
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const {
        for (const auto& f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string str(const char* key) const {
        const Json* j = get(key);
        return j && j->type == 's' ? j->text : std::string{};
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : t_(text) {}
    bool parse(Json& out) { return value(out, 0); }

private:
    const std::string& t_;
    size_t p_ = 0;

    void ws() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) ++p_; }

    bool value(Json& out, int depth) {
        if (depth > 64) return false;
        ws();
        if (p_ >= t_.size()) return false;
        char c = t_[p_];
        if (c == '"') { out.type = 's'; return string(out.text); }
        if (c == '[') {
            out.type = 'a'; ++p_; ws();
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            for (;;) {
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
                ws();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') { ++p_; continue; }
                if (t_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '{') {
            out.type = 'o'; ++p_; ws();
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (p_ >= t_.size() || t_[p_] != '"' || !string(key)) return false;
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') return false;
                ++p_;
                out.fields.emplace_back(std::move(key), Json{});
                if (!value(out.fields.back().second, depth + 1)) return false;
                ws();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') { ++p_; continue; }
                if (t_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        // number / true / false / null
        size_t start = p_;
        while (p_ < t_.size() && std::string(",]} \t\r\n").find(t_[p_]) == std::string::npos) ++p_;
        out.type = 'n';
        out.text = t_.substr(start, p_ - start);
        return p_ > start;
    }

    bool string(std::string& out) {
        ++p_;  // opening quote
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= t_.size()) return false;
            char e = t_[p_++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (p_ + 4 > t_.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::strtoul(t_.substr(p_, 4).c_str(), nullptr, 16));
                    p_ += 4;
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    else { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    break;
                }
                default: out.push_back(e); break;  // \" \\ \/
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------- Discord game list

// One way a running exe can map to a Discord game. `path` is the listed name lowercased with
// backslashes, e.g. "x64\dyinglightgame_x64_rwdi.exe"; Discord matches it as a suffix of the
// process's full path. The same exe basename can belong to many games (game.exe, hl2.exe...).
struct Candidate { std::string appId; std::string name; std::string path; bool launcher = false; };
using GameMap = std::map<std::string, std::vector<Candidate>>;  // lowercase exe basename -> candidates

struct ListedGame { std::string appId; std::string name; };
using SkuMap = std::map<std::string, ListedGame>;  // lowercase store id (Steam app id, Epic catalog id...) -> Discord game

std::string http_get(const wchar_t* host, const wchar_t* path) {
    std::string body;
    HINTERNET session = WinHttpOpen(L"GamePresenceMirror/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return body;
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        DWORD available = 0;
        while (status == 200 && WinHttpQueryDataAvailable(request, &available) && available) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
            body.append(chunk, 0, read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return body;
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf();
    out = ss.str();
    return true;
}

struct App {
    std::string id, name;
    std::vector<std::string> aliases;
    std::vector<std::string> exes;   // lowercase basenames (win32), launchers included
    std::vector<std::string> paths;  // parallel to exes: listed name, lowercase, backslashes
    std::vector<bool> launcher;      // parallel to exes
    std::vector<std::pair<std::string, std::string>> skus;  // third_party_skus: (distributor, lowercase id)
};

std::vector<App> load_apps(bool forceRefresh) {
    fs::path cache = exe_dir() / L"detectable.json";
    std::error_code ec;
    bool stale = forceRefresh || !fs::exists(cache, ec) ||
        fs::file_time_type::clock::now() - fs::last_write_time(cache, ec) > std::chrono::hours(24 * 7);
    if (stale) {
        std::cout << "Downloading Discord's detectable game list...\n";
        std::string body = http_get(L"discord.com", L"/api/v9/applications/detectable");
        if (body.size() > 1024) { std::ofstream(cache, std::ios::binary | std::ios::trunc) << body; }
        else std::cerr << "Could not download the game list; using the cached copy if there is one.\n";
    }
    std::string text;
    std::vector<App> apps;
    if (!read_file(cache, text)) return apps;
    Json root;
    if (!JsonParser(text).parse(root) || root.type != 'a') { std::cerr << "Game list is not valid JSON.\n"; return apps; }
    for (const Json& json : root.items) {
        App app;
        app.id = json.str("id");
        app.name = json.str("name");
        if (app.id.empty() || app.name.empty()) continue;
        if (const Json* aliases = json.get("aliases"); aliases && aliases->type == 'a')
            for (const Json& a : aliases->items) if (a.type == 's') app.aliases.push_back(a.text);
        if (const Json* exes = json.get("executables"); exes && exes->type == 'a') {
            for (const Json& exe : exes->items) {
                std::string os = exe.str("os");
                if (!os.empty() && os != "win32") continue;
                std::string path = exe.str("name");
                if (!path.empty() && path[0] == '>') path.erase(0, 1);
                path = lower(path);
                std::replace(path.begin(), path.end(), '/', '\\');
                size_t slash = path.find_last_of('\\');
                std::string n = slash == std::string::npos ? path : path.substr(slash + 1);
                const Json* launcher = exe.get("is_launcher");
                if (n.empty()) continue;
                app.exes.push_back(n);
                app.paths.push_back(path);
                app.launcher.push_back(launcher && launcher->type == 'n' && launcher->text == "true");
            }
        }
        // Many games (about 14,000) list no exe at all and are identified only by their store ids.
        if (const Json* skus = json.get("third_party_skus"); skus && skus->type == 'a')
            for (const Json& sku : skus->items)
                if (!sku.str("distributor").empty() && !sku.str("id").empty()) app.skus.emplace_back(sku.str("distributor"), lower(sku.str("id")));
        apps.push_back(std::move(app));
    }
    return apps;
}

GameMap build_game_map(const std::vector<App>& apps) {
    GameMap map;
    for (const App& app : apps)
        for (size_t i = 0; i < app.exes.size(); ++i) map[app.exes[i]].push_back({app.id, app.name, app.paths[i], app.launcher[i]});
    return map;
}

// Store id -> game for one store ("steam", "epic", ...).
SkuMap build_sku_map(const std::vector<App>& apps, const std::string& distributor) {
    SkuMap map;
    for (const App& app : apps)
        for (const auto& sku : app.skus)
            if (sku.first == distributor) map.emplace(sku.second, ListedGame{app.id, app.name});
    return map;
}

bool has_exe(const App& app) {
    for (size_t i = 0; i < app.exes.size(); ++i) if (!app.launcher[i]) return true;
    return false;
}

// Normalised game name -> games with that name, for games that list no exe. Used to match an
// install folder name when a game has neither an exe entry nor a usable store id.
using NameMap = std::map<std::string, std::vector<const App*>>;
NameMap build_name_map(const std::vector<App>& apps);  // defined after compact()

// Install-folder name when a path is inside a known game-library folder, e.g.
// "...\steamapps\common\wardogs\bin\game.exe" -> "wardogs". Empty if the path isn't in a library.
std::string library_folder_name(const std::string& path) {
    static const char* const markers[] = {
        "\\steamapps\\common\\", "\\epic games\\", "\\gog galaxy\\games\\", "\\gog games\\",
        "\\ubisoft game launcher\\games\\", "\\ea games\\", "\\origin games\\", "\\xboxgames\\"};
    for (const char* marker : markers) {
        size_t pos = path.find(marker);
        if (pos == std::string::npos) continue;
        size_t start = pos + std::strlen(marker);
        size_t end = path.find('\\', start);
        if (end == std::string::npos) continue;  // a file directly in the library folder
        return path.substr(start, end - start);
    }
    return {};
}

// Pick the game a running exe belongs to, or nullptr if it can't be told apart.
// `imagePath` is the process's full lowercase path ("" if unreadable). `ambiguous` is set when
// several different games still match (for example a generic game.exe).
const Candidate* resolve_exe(const std::vector<Candidate>& candidates, const std::string& imagePath, bool& ambiguous) {
    ambiguous = false;
    std::vector<const Candidate*> strong, weak;  // strong: listed folder matches the real path
    for (const Candidate& c : candidates) {
        if (c.launcher) continue;
        bool hasFolder = c.path.find('\\') != std::string::npos;
        if (!hasFolder || imagePath.empty()) { weak.push_back(&c); continue; }
        if (imagePath == c.path || (imagePath.size() > c.path.size() &&
                imagePath.compare(imagePath.size() - c.path.size(), c.path.size(), c.path) == 0 &&
                imagePath[imagePath.size() - c.path.size() - 1] == '\\'))
            strong.push_back(&c);
    }
    auto unique_game = [&](const std::vector<const Candidate*>& list) -> const Candidate* {
        for (const Candidate* c : list) if (c->appId != list[0]->appId) { ambiguous = true; return nullptr; }
        return list.empty() ? nullptr : list[0];
    };
    if (!strong.empty()) {
        // Prefer the most specific (longest) matching path.
        size_t longest = 0;
        for (const Candidate* c : strong) longest = std::max(longest, c->path.size());
        std::vector<const Candidate*> best;
        for (const Candidate* c : strong) if (c->path.size() == longest) best.push_back(c);
        return unique_game(best);
    }
    return unique_game(weak);
}

// Lowercase letters and digits only, so "Dying Light 2" matches "dyinglight2" and "dying-light".
std::string compact(const std::string& value) {
    std::string result;
    for (unsigned char c : lower(value)) if (std::isalnum(c)) result.push_back(static_cast<char>(c));
    return result;
}

NameMap build_name_map(const std::vector<App>& apps) {
    NameMap map;
    for (const App& app : apps) {
        if (has_exe(app)) continue;  // games with an exe are matched by it
        std::vector<std::string> names{compact(app.name)};
        for (const auto& a : app.aliases) names.push_back(compact(a));
        for (const auto& n : names) {
            if (n.size() < 4) continue;  // too short to be a trustworthy folder name
            auto& games = map[n];
            if (std::find(games.begin(), games.end(), &app) == games.end()) games.push_back(&app);
        }
    }
    return map;
}

// Every game whose name or alias contains all the typed words (order and spacing ignored).
std::vector<const App*> search_apps(const std::vector<App>& apps, const std::string& query) {
    std::vector<std::string> words;
    std::istringstream in(query);
    for (std::string w; in >> w;) if (!compact(w).empty()) words.push_back(compact(w));
    std::vector<const App*> hits;
    if (words.empty()) return hits;
    for (const App& app : apps) {
        std::vector<std::string> names{compact(app.name)};
        for (const auto& a : app.aliases) names.push_back(compact(a));
        for (const auto& n : names) {
            if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return n.find(w) != std::string::npos; })) {
                hits.push_back(&app);
                break;
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [](const App* a, const App* b) { return lower(a->name) < lower(b->name); });
    return hits;
}

// ---------------------------------------------------------------- picked game (picked.txt)

// Games you picked, one per line: "applicationId|Game name". `send` asks which one to show.
// The file is ordered oldest to newest; picking a game again moves it to the end.
struct Picked { std::string appId; std::string name; };

std::vector<Picked> load_picks() {
    std::vector<Picked> picks;
    std::ifstream in(exe_dir() / L"picked.txt");
    std::string line;
    while (std::getline(in, line)) {
        size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        Picked p{trim(line.substr(0, bar)), trim(line.substr(bar + 1))};
        if (all_digits(p.appId) && p.appId.size() <= 24 && !p.name.empty()) picks.push_back(std::move(p));
    }
    return picks;
}

void add_pick(Picked picked) {
    for (char& c : picked.name) if (c == '\r' || c == '\n' || c == '|') c = ' ';
    std::vector<Picked> picks = load_picks();
    picks.erase(std::remove_if(picks.begin(), picks.end(), [&](const Picked& p) { return p.appId == picked.appId; }), picks.end());
    picks.push_back(std::move(picked));
    std::ofstream out(exe_dir() / L"picked.txt", std::ios::trunc);
    for (const auto& p : picks) out << p.appId << "|" << p.name << "\n";
}

// Show the picked games, newest first, and ask which one to use. Returns false if cancelled.
bool choose_pick(Picked& chosen) {
    std::vector<Picked> picks = load_picks();
    if (picks.empty()) {
        std::cerr << "No games picked yet. Run: game_presence_mirror.exe pick <game name>\n";
        return false;
    }
    std::reverse(picks.begin(), picks.end());
    if (picks.size() == 1) { chosen = picks[0]; return true; }
    std::cout << "Which game do you want to show on Discord?\n";
    for (size_t i = 0; i < picks.size(); ++i) std::cout << "  " << i + 1 << ") " << picks[i].name << "\n";
    std::cout << "Enter a number (blank to cancel): ";
    std::string input; std::getline(std::cin, input);
    size_t n = 0;
    std::istringstream(input) >> n;
    if (n < 1 || n > picks.size()) { std::cout << "Cancelled.\n"; return false; }
    chosen = picks[n - 1];
    return true;
}

// ---------------------------------------------------------------- processes

struct Running { DWORD pid; std::string exe; };

std::vector<Running> running_processes() {
    std::vector<Running> list;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return list;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        list.push_back({entry.th32ProcessID, lower(to_utf8(entry.szExeFile))});
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return list;
}

long long process_start_unix(DWORD pid) {
    long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return now;
    FILETIME created, exited, kernel, user;
    long long result = now;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER t; t.LowPart = created.dwLowDateTime; t.HighPart = created.dwHighDateTime;
        long long unixTime = static_cast<long long>(t.QuadPart / 10000000ull) - 11644473600ll;
        if (unixTime > 0 && unixTime <= now) result = unixTime;
    }
    CloseHandle(process);
    return result;
}

// Full path of a process's exe, lowercase UTF-8; empty if it can't be read.
std::string process_image_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) result = lower(to_utf8(std::wstring(path, length)));
    CloseHandle(process);
    return result;
}

// The Steam app id of the game Steam says is running right now, or "" if none.
// Steam keeps this in the registry for the signed-in user.
std::string steam_running_app_id() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return {};
    return value ? std::to_string(value) : std::string{};
}

// ---------------------------------------------------------------- Epic Games Launcher

// The Epic launcher keeps a manifest (*.item, JSON) per installed game. Discord's Epic store ids
// are these same ids, so a process running from InstallLocation identifies the game.
struct EpicInstall { std::string appName, catalogId, dir; };  // dir: lowercase, backslashes, trailing backslash

bool parse_epic_manifest(const std::string& text, EpicInstall& out) {
    Json json;
    if (!JsonParser(text).parse(json) || json.type != 'o') return false;
    out.appName = lower(json.str("AppName"));
    out.catalogId = lower(json.str("CatalogItemId"));
    std::string dir = lower(json.str("InstallLocation"));
    std::replace(dir.begin(), dir.end(), '/', '\\');
    while (!dir.empty() && dir.back() == '\\') dir.pop_back();
    if (dir.size() < 4 || (out.appName.empty() && out.catalogId.empty())) return false;  // reject "" and bare drive roots
    out.dir = dir + "\\";
    return true;
}

std::vector<EpicInstall> load_epic_installs() {
    std::vector<EpicInstall> installs;
    wchar_t base[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (!length || length >= MAX_PATH) return installs;
    std::error_code ec;
    fs::path dir = fs::path(base) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".item") continue;
        std::string text;
        EpicInstall install;
        if (read_file(it->path(), text) && parse_epic_manifest(text, install)) installs.push_back(std::move(install));
    }
    return installs;
}

// ---------------------------------------------------------------- Xbox / Microsoft Store games

// Xbox (GDK) games ship a MicrosoftGame.config in their install folder. It can hold the game's
// Store id (<StoreId>9NRMZXGKP4NM</StoreId>), which is the id Discord lists for Xbox games.
// Returns the lowercase id, or "" if there is none.
std::string parse_store_id(std::string text) {
    if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE) {
        // UTF-16 LE with BOM: convert to UTF-8
        std::wstring wide(reinterpret_cast<const wchar_t*>(text.data() + 2), (text.size() - 2) / 2);
        text = to_utf8(wide);
    }
    std::string haystack = lower(text);
    size_t open = haystack.find("<storeid>");
    if (open == std::string::npos) return {};
    open += 9;
    size_t close = haystack.find("</storeid>", open);
    if (close == std::string::npos) return {};
    std::string id = trim(haystack.substr(open, close - open));
    bool valid = id.size() >= 8 && id.size() <= 16 && std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
    return valid ? id : std::string{};
}

// Look for MicrosoftGame.config in the exe's folder and up to 6 parent folders.
// `cache` remembers each folder's answer so the disk is only read once per folder.
std::string game_store_id(const std::string& imagePath, std::map<std::string, std::string>& cache) {
    size_t slash = imagePath.find_last_of('\\');
    if (slash == std::string::npos) return {};
    std::string dir = imagePath.substr(0, slash);
    for (int level = 0; level < 7 && dir.size() > 3; ++level) {
        auto cached = cache.find(dir);
        if (cached != cache.end()) { if (!cached->second.empty()) return cached->second; }
        else {
            std::string text, id;
            if (read_file(fs::path(from_utf8(dir)) / L"MicrosoftGame.config", text)) id = parse_store_id(text);
            cache[dir] = id;
            if (!id.empty()) return id;
        }
        size_t up = dir.find_last_of('\\');
        if (up == std::string::npos) break;
        dir.erase(up);
    }
    return {};
}

// ---------------------------------------------------------------- sockets

class Winsock {
public:
    Winsock() { WSADATA d{}; ok_ = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
    ~Winsock() { if (ok_) WSACleanup(); }
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

bool send_line(SOCKET s, const std::string& line) {
    const char* data = line.data();
    size_t size = line.size();
    while (size) {
        int sent = send(s, data, static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent; size -= static_cast<size_t>(sent);
    }
    return true;
}

bool recv_line(SOCKET s, std::string& line, size_t maxSize) {
    line.clear();
    char c;
    while (line.size() < maxSize) {
        if (recv(s, &c, 1, 0) != 1) return false;
        if (c == '\n') return true;
        line.push_back(c);
    }
    return false;
}

// ---------------------------------------------------------------- Discord IPC

class DiscordIpc {
public:
    ~DiscordIpc() { close(); }

    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }
    const std::string& client_id() const { return clientId_; }

    void close() {
        if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
        clientId_.clear();
    }

    bool connect(const std::string& clientId) {
        close();
        for (int i = 0; i < 10 && pipe_ == INVALID_HANDLE_VALUE; ++i) {
            std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
        std::string reply;
        if (!write_frame(0, "{\"v\":1,\"client_id\":\"" + clientId + "\"}") || !read_frame(reply) ||
            reply.find("READY") == std::string::npos) {
            close();
            return false;
        }
        clientId_ = clientId;
        return true;
    }

    bool set_activity(long long start, DWORD pid) {
        return command("{\"pid\":" + std::to_string(pid) + ",\"activity\":{\"instance\":false,\"timestamps\":{\"start\":" +
            std::to_string(start) + "}}}");
    }

    bool clear_activity(DWORD pid) { return command("{\"pid\":" + std::to_string(pid) + "}"); }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string clientId_;
    int nonce_ = 0;

    bool command(const std::string& args) {
        std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + args + ",\"nonce\":\"" + std::to_string(++nonce_) + "\"}";
        std::string reply;
        return write_frame(1, payload) && read_frame(reply) && reply.find("\"ERROR\"") == std::string::npos;
    }

    bool write_frame(int opcode, const std::string& json) {
        std::string frame(8, '\0');
        int length = static_cast<int>(json.size());
        std::memcpy(&frame[0], &opcode, 4);
        std::memcpy(&frame[4], &length, 4);
        frame += json;
        DWORD written = 0;
        return WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
    }

    bool read_exact(char* data, DWORD size) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (size) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (!available) {
                if (std::chrono::steady_clock::now() > deadline) return false;
                Sleep(20);
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(pipe_, data, std::min(size, available), &read, nullptr) || !read) return false;
            data += read; size -= read;
        }
        return true;
    }

    bool read_frame(std::string& json) {
        char header[8];
        if (!read_exact(header, 8)) return false;
        int length = 0;
        std::memcpy(&length, header + 4, 4);
        if (length < 0 || length > 1 << 20) return false;
        json.assign(static_cast<size_t>(length), '\0');
        return length == 0 || read_exact(json.data(), static_cast<DWORD>(length));
    }
};

// ---------------------------------------------------------------- sender (gaming PC)

bool report(const std::string& host, const std::string& token, const std::string& message) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(g_port);
    if (InetPtonA(AF_INET, host.c_str(), &address.sin_addr) != 1) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    bool ok = connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        send_line(s, std::string(kMagic) + " " + token + " " + message + "\n");
    std::string reply;
    ok = ok && recv_line(s, reply, 16) && reply == "OK";
    closesocket(s);
    return ok;
}

// Ctrl+C or closing the console: stop the loops so they can tell the receiver to clear Discord.
std::atomic<bool> g_stop{false};
std::atomic<bool> g_done{false};

BOOL WINAPI on_console_event(DWORD) {
    g_stop = true;
    for (int i = 0; i < 40 && !g_done; ++i) Sleep(100);  // the console-close event allows only a few seconds
    return TRUE;
}

void sleep_unless_stopped(int milliseconds) {
    for (int waited = 0; waited < milliseconds && !g_stop; waited += 100) Sleep(100);
}

// Always show the game chosen with `pick`, whether or not it is running on this PC.
// playedSeconds: time already spent in the game; the Discord timer starts that far in the past.
int run_pinned_sender(const std::string& host, const std::string& token, long long playedSeconds) {
    Winsock winsock;
    if (!winsock.ok()) { std::cerr << "Winsock startup failed.\n"; return 1; }

    Picked picked;
    if (!choose_pick(picked)) return 2;
    SetConsoleCtrlHandler(on_console_event, TRUE);
    long long start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - playedSeconds;
    bool reported = false;
    bool receiverUp = true;  // so a failure is announced once, not every second
    std::cout << "Showing \"" << picked.name << "\" on Discord via " << host << ":" << g_port;
    if (playedSeconds > 0) std::cout << ", already playing for " << playedSeconds / 3600 << "h " << playedSeconds % 3600 / 60 << "m " << playedSeconds % 60 << "s";
    std::cout << " (Ctrl+C to stop).\n";

    while (!g_stop) {
        if (report(host, token, "SET " + picked.appId + " " + std::to_string(start) + " " + picked.name)) {
            if (!reported) std::cout << "Playing: " << picked.name << "\n";
            else if (!receiverUp) std::cout << "Receiver reachable again.\n";
            reported = true; receiverUp = true;
        } else {
            if (receiverUp) std::cerr << "Receiver did not accept the report (check IP, port, token, and firewall); retrying every second.\n";
            reported = false; receiverUp = false;
        }
        sleep_unless_stopped(kReportSeconds * 1000);
    }
    if (report(host, token, "CLEAR")) std::cout << "Cleared Discord activity.\n";
    g_done = true;
    return 0;
}

// Report any game from Discord's list that is running. Everything it knows comes from the list
// (exe names and folders, Steam / Epic ids, game names), so `update` is all that's needed to
// support new games.
int run_auto_sender(const std::string& host, const std::string& token) {
    Winsock winsock;
    if (!winsock.ok()) { std::cerr << "Winsock startup failed.\n"; return 1; }
    SetConsoleCtrlHandler(on_console_event, TRUE);

    // Everything derived from the list; rebuilt when detectable.json changes (e.g. after `update`).
    std::vector<App> apps;
    GameMap games;
    SkuMap steamGames, epicGames, xboxGames;
    NameMap folderNames;
    auto load_list = [&]() {
        std::vector<App> fresh = load_apps(false);
        if (fresh.empty()) return false;  // unreadable or half-written: keep what we have
        apps = std::move(fresh);
        games = build_game_map(apps);
        steamGames = build_sku_map(apps, "steam");
        epicGames = build_sku_map(apps, "epic");
        xboxGames = build_sku_map(apps, "xbox");
        for (const auto& entry : build_sku_map(apps, "microsoft")) xboxGames.emplace(entry);
        folderNames = build_name_map(apps);
        return true;
    };
    if (!load_list()) { std::cerr << "Could not load Discord's game list. Run `update` while online.\n"; return 1; }
    fs::path listFile = exe_dir() / L"detectable.json";
    std::error_code ec;
    auto listTime = fs::last_write_time(listFile, ec);
    std::vector<EpicInstall> epicInstalls = load_epic_installs();
    std::cout << "Loaded " << apps.size() << " known games (" << epicInstalls.size() << " Epic installs found). Receiver: "
              << host << ":" << g_port << ". Auto-detect on (Ctrl+C to stop).\n";

    // A game found running this cycle. `key` identifies it across cycles.
    struct Detected { std::string key, appId, name; long long start = 0; std::string via; };
    auto now_unix = [] { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); };

    std::string activeKey, activeName;  // game currently reported
    bool clearPending = false;
    bool receiverUp = true;             // so a failure is announced once, not every second
    std::string steamKey;               // Steam game seen last cycle
    long long steamStart = 0;           // when auto first saw it (Steam gives no start time)
    std::set<std::string> warned;       // one-time warnings
    struct ProcInfo { std::string exe, path, storeId; };
    std::map<DWORD, ProcInfo> pathCache;  // process image paths (and Xbox Store ids), looked up once per process
    std::map<std::string, std::string> storeIdByDir;  // folder -> Xbox Store id from MicrosoftGame.config ("" if none)
    auto lastReport = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastListCheck = std::chrono::steady_clock::now();
    auto lastEpicLoad = lastListCheck;

    while (!g_stop) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastListCheck > std::chrono::seconds(30)) {
            lastListCheck = now;
            auto latest = fs::last_write_time(listFile, ec);
            if (!ec && latest != listTime && load_list()) {
                listTime = latest;
                std::cout << "Game list updated: " << apps.size() << " known games.\n";
            }
        }
        if (now - lastEpicLoad > std::chrono::seconds(60)) { lastEpicLoad = now; epicInstalls = load_epic_installs(); }

        std::vector<Running> processes = running_processes();
        std::set<DWORD> livePids;
        for (const auto& p : processes) {
            livePids.insert(p.pid);
            ProcInfo& info = pathCache[p.pid];
            if (info.exe != p.exe) {
                std::string path = process_image_path(p.pid);
                info = {p.exe, path, path.empty() ? std::string{} : game_store_id(path, storeIdByDir)};
            }
        }
        for (auto it = pathCache.begin(); it != pathCache.end();) it = livePids.count(it->first) ? std::next(it) : pathCache.erase(it);

        std::vector<Detected> found;
        // Add a game, or merge into one already found (keeping the earliest start time).
        auto add_found = [&](const std::string& key, const std::string& appId, const std::string& name, long long start, const std::string& via) {
            auto same = std::find_if(found.begin(), found.end(), [&](const Detected& d) { return d.appId == appId; });
            if (same != found.end()) { same->start = std::min(same->start, start); return; }
            found.push_back({key, appId, name, start, via});
        };

        // 1. Steam: the game Steam says is running, matched by Steam app id. This is how games Discord
        //    lists without an exe are detected, and it tells apart games that share an exe name.
        bool steamUp = std::any_of(processes.begin(), processes.end(), [](const Running& p) { return p.exe == "steam.exe"; });
        std::string steamId = steamUp ? steam_running_app_id() : std::string{};
        if (!steamId.empty()) {
            auto it = steamGames.find(steamId);
            if (it != steamGames.end()) {
                std::string key = "steam:" + steamId;
                if (key != steamKey) { steamKey = key; steamStart = now_unix(); }
                add_found(key, it->second.appId, it->second.name, steamStart, "Steam app " + steamId);
            } else {
                steamKey.clear();
                if (warned.insert("steam:" + steamId).second)
                    std::cerr << "A Steam game (app id " << steamId << ") is running but Discord doesn't list it; use `pick --id` and `send` to show it.\n";
            }
        } else {
            steamKey.clear();
        }

        // 2. Exe names from Discord's list, checked against the process's real path.
        for (const auto& p : processes) {
            auto known = games.find(p.exe);
            if (known == games.end()) continue;
            bool ambiguous = false;
            const Candidate* match = resolve_exe(known->second, pathCache[p.pid].path, ambiguous);
            if (!match) {
                if (ambiguous && warned.insert("exe:" + p.exe).second)
                    std::cerr << p.exe << " is used by several games in Discord's list and can't be told apart; not reporting it.\n";
                continue;
            }
            add_found("exe:" + std::to_string(p.pid) + ":" + match->appId, match->appId, match->name, process_start_unix(p.pid), "exe " + p.exe);
        }

        // 3. Epic Games Launcher: a process running from an installed Epic game's folder, matched by
        //    the Epic ids in Discord's list. Covers Epic games Discord lists without an exe.
        for (const auto& install : epicInstalls) {
            auto game = epicGames.find(install.catalogId);
            if (game == epicGames.end()) game = epicGames.find(install.appName);
            if (game == epicGames.end()) continue;
            long long earliest = 0;
            for (const auto& p : processes) {
                const std::string& path = pathCache[p.pid].path;
                if (path.compare(0, install.dir.size(), install.dir) != 0) continue;
                long long start = process_start_unix(p.pid);
                if (!earliest || start < earliest) earliest = start;
            }
            if (earliest) add_found("epic:" + game->second.appId, game->second.appId, game->second.name, earliest, "Epic install " + install.dir);
        }

        // 4. Xbox app / Microsoft Store (GDK) games: a process running from a folder whose
        //    MicrosoftGame.config holds a Store id that Discord lists.
        for (const auto& p : processes) {
            const std::string& storeId = pathCache[p.pid].storeId;
            if (storeId.empty()) continue;
            auto game = xboxGames.find(storeId);
            if (game == xboxGames.end()) continue;
            add_found("xbox:" + game->second.appId, game->second.appId, game->second.name, process_start_unix(p.pid), "Xbox Store id " + storeId);
        }

        // 5. Other launchers (and Steam when Steam isn't running): a process running from a known
        //    game-library folder whose folder name exactly matches the name of a game that lists no exe.
        for (const auto& p : processes) {
            std::string folder = library_folder_name(pathCache[p.pid].path);
            if (folder.empty()) continue;
            auto hit = folderNames.find(compact(folder));
            if (hit == folderNames.end() || hit->second.size() != 1) continue;  // unknown or ambiguous name
            const App* app = hit->second[0];
            add_found("folder:" + app->id, app->id, app->name, process_start_unix(p.pid), "install folder " + folder);
        }

        // Keep reporting the current game while it is still running; otherwise take the first one found.
        const Detected* chosen = found.empty() ? nullptr : &found[0];
        for (const auto& d : found) if (d.key == activeKey) { chosen = &d; break; }

        if (chosen) {
            bool newGame = chosen->key != activeKey;
            if (newGame || now - lastReport >= std::chrono::seconds(kReportSeconds)) {
                std::string msg = "SET " + chosen->appId + " " + std::to_string(chosen->start) + " " + chosen->name;
                if (report(host, token, msg)) {
                    if (newGame) std::cout << "Playing: " << chosen->name << "  (detected via " << chosen->via << ")\n";
                    if (!receiverUp) std::cout << "Receiver reachable again.\n";
                    receiverUp = true;
                    activeKey = chosen->key; activeName = chosen->name; clearPending = true;
                    lastReport = now;
                } else {
                    // lastReport is left alone, so the next scan (one second later) tries again.
                    if (receiverUp) std::cerr << "Receiver did not accept the report (check IP, port, token, and firewall); retrying every second.\n";
                    receiverUp = false;
                }
            }
        } else if (!activeKey.empty() || clearPending) {
            if (report(host, token, "CLEAR")) {
                std::cout << "Stopped playing: " << activeName << "\n";
                if (!receiverUp) std::cout << "Receiver reachable again.\n";
                receiverUp = true;
                activeKey.clear(); activeName.clear(); clearPending = false;
            } else {
                if (receiverUp) std::cerr << "Could not tell the receiver the game closed; retrying every second.\n";
                receiverUp = false;
            }
        }
        sleep_unless_stopped(kReportSeconds * 1000);
    }
    if (!activeKey.empty() || clearPending) report(host, token, "CLEAR");
    g_done = true;
    return 0;
}

// ---------------------------------------------------------------- receiver (streaming PC)

int run_receiver(const std::string& token) {
    Winsock winsock;
    if (!winsock.ok()) { std::cerr << "Winsock startup failed.\n"; return 1; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(g_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        std::cerr << "Could not listen on TCP port " << g_port << " (is another program, or another copy of this one, already using it? Try a different --port).\n";
        return 1;
    }
    std::cout << "Listening on TCP port " << g_port << ". Keep the Discord desktop app running on this PC.\n";

    DiscordIpc discord;
    bool active = false;
    std::string activeId;
    long long activeStart = 0;     // what Discord currently shows
    auto lastPush = std::chrono::steady_clock::now();  // when we last sent it to Discord
    bool discordDown = false;      // so "can't reach Discord" is printed once, not every second
    auto lastBeat = std::chrono::steady_clock::now();
    DWORD ownPid = GetCurrentProcessId();

    auto stop_presence = [&]() {
        if (discord.connected()) discord.clear_activity(ownPid);
        discord.close();
        active = false; activeId.clear(); activeStart = 0;
    };

    for (;;) {
        fd_set readSet; FD_ZERO(&readSet); FD_SET(listener, &readSet);
        timeval wait{1, 0};
        bool ready = select(0, &readSet, nullptr, nullptr, &wait) > 0;

        if (active && std::chrono::steady_clock::now() - lastBeat > std::chrono::seconds(kExpirySeconds)) {
            std::cout << "No update from the gaming PC; clearing presence.\n";
            stop_presence();
        }
        if (!ready) continue;

        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        std::string line;
        bool ok = false;
        if (recv_line(client, line, 512)) {
            std::istringstream in(line);
            std::string magic, sentToken, command;
            in >> magic >> sentToken >> command;
            if (magic == kMagic && sentToken == token) {
                if (command == "SET") {
                    std::string appId; long long start = 0;
                    in >> appId >> start;
                    std::string name; std::getline(in, name); name = trim(name);
                    if (all_digits(appId) && appId.size() <= 24 && start > 0) {
                        auto now = std::chrono::steady_clock::now();
                        // The gaming PC reports every second, but Discord already counts the elapsed time itself
                        // from `start`, and rate-limits presence updates. So only push when the game or start
                        // time changed, plus a periodic refresh (which also recovers from a Discord restart).
                        bool unchanged = active && discord.connected() && activeId == appId && activeStart == start &&
                            now - lastPush < std::chrono::seconds(kDiscordRefreshSeconds);
                        if (unchanged) {
                            ok = true;
                        } else {
                            bool needConnect = !discord.connected() || discord.client_id() != appId;
                            if (needConnect) {
                                if (discord.connected()) discord.clear_activity(ownPid);
                                ok = discord.connect(appId) && discord.set_activity(start, ownPid);
                            } else {
                                ok = discord.set_activity(start, ownPid);
                                if (!ok) ok = discord.connect(appId) && discord.set_activity(start, ownPid);  // pipe died: reconnect once
                            }
                            if (ok) {
                                if (needConnect || discordDown) std::cout << "Now showing on Discord: " << name << "\n";
                                discordDown = false;
                                lastPush = now;
                                activeStart = start;
                            } else {
                                if (!discordDown) std::cerr << "Could not reach Discord. Is the Discord desktop app running and signed in on this PC?\n";
                                discordDown = true;
                                discord.close();
                            }
                        }
                        if (ok) { active = true; activeId = appId; lastBeat = now; }
                    }
                } else if (command == "CLEAR") {
                    if (active) std::cout << "Game closed; clearing Discord presence.\n";
                    stop_presence();
                    ok = true;
                }
            } else {
                std::cerr << "Rejected a report with a wrong token.\n";
            }
        }
        send_line(client, ok ? "OK\n" : "NO\n");
        closesocket(client);
    }
}

// ---------------------------------------------------------------- selection commands

// Search Discord's game list by name and save the chosen game to picked.txt.
int run_pick(const std::string& query) {
    std::vector<App> apps = load_apps(false);
    std::vector<const App*> hits = search_apps(apps, query);
    if (hits.empty()) {
        std::cout << "No game in Discord's list matches \"" << query << "\". Try fewer or different words.\n";
        return 0;
    }
    size_t shown = hits.size();
    std::cout << hits.size() << " match(es) for \"" << query << "\":\n";
    for (size_t i = 0; i < shown; ++i) {
        std::string exeList;
        for (size_t e = 0; e < hits[i]->exes.size() && e < 2; ++e) exeList += (e ? ", " : "") + hits[i]->exes[e];
        std::cout << "  " << i + 1 << ") " << hits[i]->name << (exeList.empty() ? "" : "  [" + exeList + "]") << "\n";
    }
    std::cout << "Enter the number of the game to show on Discord, or blank to cancel: ";
    std::string input; std::getline(std::cin, input);
    size_t n = 0;
    std::istringstream(input) >> n;
    if (n < 1 || n > shown) { std::cout << "Nothing picked.\n"; return 0; }
    add_pick(Picked{hits[n - 1]->id, hits[n - 1]->name});
    std::cout << "Added \"" << hits[n - 1]->name << "\" to your picked games. Run `send` to choose what to show on Discord.\n";
    return 0;
}

void print_usage() {
    std::cout <<
        "Game Presence Mirror\n"
        "Shows a game as your Discord activity on your streaming PC.\n\n"
        "Streaming PC (where Discord runs):\n"
        "  game_presence_mirror.exe receive --token <secret> [--port <n>]\n\n"
        "Gaming PC:\n"
        "  game_presence_mirror.exe pick <game name>       search Discord's game list and pick one game\n"
        "  game_presence_mirror.exe pick --id <app id> <name>   pick a custom game (your own Discord application)\n"
        "  game_presence_mirror.exe send <streaming PC IPv4> --token <secret> [--port <n>]\n"
        "                                                  asks which picked game to show, then shows it (running or not)\n"
        "    optional: --hours <n> --minutes <n> --seconds <n>   start the timer that long ago (time already played)\n"
        "  game_presence_mirror.exe auto <streaming PC IPv4> --token <secret> [--port <n>]\n"
        "                                                  report whatever known game is actually running\n"
        "  game_presence_mirror.exe list                   show your picked games\n"
        "  game_presence_mirror.exe update                 refresh Discord's game list\n\n"
        "The token is any password you invent; it must match on both PCs.\n"
        "--port <n> changes the TCP port (default 47831); it must match on both PCs.\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(to_utf8(argv[i]));

    // Remove "--flag value" from args; returns false if the flag is absent.
    auto take_option = [&args](const std::string& flag, std::string& value) {
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] != flag) continue;
            value = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            return true;
        }
        return false;
    };
    std::string token, hoursText, minutesText, secondsText, portText;
    take_option("--token", token);
    bool hasPort = take_option("--port", portText);
    bool hasHours = take_option("--hours", hoursText);
    bool hasMinutes = take_option("--minutes", minutesText);
    bool hasSeconds = take_option("--seconds", secondsText);
    bool tokenOk = !token.empty() && token.find_first_of(" \r\n") == std::string::npos && token.size() <= 64;

    // Time already played, for `send`. Hours, minutes and seconds can be combined; total at most 1000 hours.
    long long playedSeconds = 0;
    if ((hasHours && (!all_digits(hoursText) || hoursText.size() > 4)) ||
        (hasMinutes && (!all_digits(minutesText) || minutesText.size() > 6)) ||
        (hasSeconds && (!all_digits(secondsText) || secondsText.size() > 8))) {
        std::cerr << "--hours, --minutes and --seconds must be whole numbers.\n";
        return 2;
    }
    if (hasHours) playedSeconds += std::stoll(hoursText) * 3600;
    if (hasMinutes) playedSeconds += std::stoll(minutesText) * 60;
    if (hasSeconds) playedSeconds += std::stoll(secondsText);
    if (playedSeconds > 1000ll * 3600) { std::cerr << "The played time can be at most 1000 hours.\n"; return 2; }

    if (hasPort) {
        if (!all_digits(portText) || portText.size() > 5 || std::stoi(portText) < 1 || std::stoi(portText) > 65535) {
            std::cerr << "--port must be a number from 1 to 65535.\n";
            return 2;
        }
        g_port = static_cast<unsigned short>(std::stoi(portText));
    }

    if (args.empty()) { print_usage(); return 0; }
    const std::string& cmd = args[0];
    if (hasPort && cmd != "receive" && cmd != "send" && cmd != "auto") {
        std::cerr << "--port only works with `receive`, `send` and `auto`.\n";
        return 2;
    }
    if ((hasHours || hasMinutes || hasSeconds) && cmd != "send") {
        std::cerr << "--hours, --minutes and --seconds only work with `send`.\n";
        return 2;
    }
    if (cmd == "receive") {
        if (!tokenOk) { std::cerr << "Provide --token <secret> (no spaces, up to 64 chars).\n"; return 2; }
        return run_receiver(token);
    }
    if (cmd == "send" && args.size() == 2) {
        if (!tokenOk) { std::cerr << "Provide --token <secret> (no spaces, up to 64 chars).\n"; return 2; }
        return run_pinned_sender(args[1], token, playedSeconds);
    }
    if (cmd == "auto" && args.size() == 2) {
        if (!tokenOk) { std::cerr << "Provide --token <secret> (no spaces, up to 64 chars).\n"; return 2; }
        return run_auto_sender(args[1], token);
    }
    if (cmd == "pick" && args.size() >= 2 && args[1] == "--id") {
        // Custom game: an application created in the Discord developer portal.
        if (args.size() < 4 || !all_digits(args[2]) || args[2].size() > 24) {
            std::cerr << "Usage: game_presence_mirror.exe pick --id <application id> <application name>\n";
            return 2;
        }
        Picked picked{args[2], {}};
        for (size_t i = 3; i < args.size(); ++i) picked.name += (i > 3 ? " " : "") + args[i];
        add_pick(picked);
        std::cout << "Added \"" << picked.name << "\" (app id " << picked.appId << ") to your picked games. Run `send` to choose what to show on Discord.\n"
                     "Friends will see the name and icon set on that application in the Discord developer portal.\n";
        return 0;
    }
    if (cmd == "pick") {
        if (args.size() < 2) { std::cerr << "Usage: game_presence_mirror.exe pick <game name>\n       game_presence_mirror.exe pick --id <application id> <application name>\n"; return 2; }
        std::string query;
        for (size_t i = 1; i < args.size(); ++i) query += (i > 1 ? " " : "") + args[i];
        return run_pick(query);
    }
    if (cmd == "list") {
        std::vector<Picked> picks = load_picks();
        if (picks.empty()) std::cout << "No games picked yet.\n";
        for (auto it = picks.rbegin(); it != picks.rend(); ++it) std::cout << it->name << " (app id " << it->appId << ")\n";
        return 0;
    }
    if (cmd == "update") {
        std::cout << "Loaded " << load_apps(true).size() << " known games. A running `auto` picks up the new list within 30 seconds.\n";
        return 0;
    }
    print_usage();
    return 2;
}
