#include "core.h"
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

Config cfg;
std::vector<std::string> g_likedIds;

namespace util {

std::string readFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return {}; }
    std::string out;
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    out.resize(got);
    fclose(f);
    return out;
}

bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    size_t i = 0;
    // keep device prefix (sdmc:/) intact
    if (path.size() >= 5 && path.compare(0, 5, "sdmc:") == 0) { cur = "sdmc:"; i = 5; }
    for (; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "sdmc:") mkdir(cur.c_str(), 0777);
            if (i == path.size()) break;
            cur += '/';
        } else {
            cur += path[i];
        }
    }
    return dirExists(path);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t fileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

bool removeFile(const std::string& path) { return unlink(path.c_str()) == 0; }

std::string basename(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string dirname(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? std::string() : p.substr(0, pos);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool endsWith(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

std::string formatTime(int seconds) {
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    if (h > 0) return fmt("%d:%02d:%02d", h, m, s);
    return fmt("%d:%02d", m, s);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += "%20";
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

static SpinLock g_logMutex;

void log(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    ScopedLock<SpinLock> lk(g_logMutex);
    FILE* fp = fopen("sdmc:/switch/scswitch/log.txt", "a");
    if (fp) {
        fprintf(fp, "%s\n", buf);
        fclose(fp);
    }
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

uint32_t hashStr(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

} // namespace util

bool Config::load() {
    std::string data = util::readFile("sdmc:/switch/scswitch/config.cfg");
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= data.size()) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos) nl = data.size();
        lines.push_back(data.substr(start, nl - start));
        if (nl == data.size()) break;
        start = nl + 1;
    }
    for (auto& raw : lines) {
        std::string line = util::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(line.substr(0, eq));
        std::string v = util::trim(line.substr(eq + 1));
        if (k == "client_id") client_id = v;
        else if (k == "music_dir") music_dir = v;
        else if (k == "cache_dir") cache_dir = v;
        else if (k == "proxy") proxy = v;
        else if (k == "oauth_token") oauth_token = v;
        else if (k == "lang") lang = v;
        else if (k == "insecure_tls") insecure_tls = (v == "1" || util::lower(v) == "true");
    }
    return true;
}

bool Config::save() const {
    util::ensureDir("sdmc:/switch/scswitch");
    std::string s;
    s += "# scswitch configuration\n";
    s += "# client_id can be extracted from soundcloud.com web app requests\n";
    s += "client_id=" + client_id + "\n";
    s += "music_dir=" + music_dir + "\n";
    s += "cache_dir=" + cache_dir + "\n";
    s += "proxy=" + proxy + "\n";
    s += "oauth_token=" + oauth_token + "\n";
    s += "lang=" + lang + "\n";
    s += std::string("insecure_tls=") + (insecure_tls ? "1" : "0") + "\n";
    return util::writeFile("sdmc:/switch/scswitch/config.cfg", s);
}

bool isLiked(const std::string& id) {
    return std::find(g_likedIds.begin(), g_likedIds.end(), id) != g_likedIds.end();
}

void setLiked(const std::string& id, bool liked) {
    auto it = std::find(g_likedIds.begin(), g_likedIds.end(), id);
    if (liked && it == g_likedIds.end()) g_likedIds.push_back(id);
    if (!liked && it != g_likedIds.end()) g_likedIds.erase(it);
    saveLikes();
}

void saveLikes() {
    std::string s;
    for (auto& id : g_likedIds) s += id + "\n";
    util::writeFile("sdmc:/switch/scswitch/likes.txt", s);
}

void loadLikes() {
    g_likedIds.clear();
    std::string data = util::readFile("sdmc:/switch/scswitch/likes.txt");
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos) nl = data.size();
        std::string line = util::trim(data.substr(start, nl - start));
        if (!line.empty()) g_likedIds.push_back(line);
        start = nl + 1;
    }
}
