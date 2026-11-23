#include "library.h"
#include "i18n.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace library {

static bool audioExt(const std::string& name) {
    return util::endsWith(name, ".mp3") || util::endsWith(name, ".flac") ||
           util::endsWith(name, ".ogg") || util::endsWith(name, ".oga") ||
           util::endsWith(name, ".m4a") || util::endsWith(name, ".mp4");
}

std::string idForPath(const std::string& path) { return "local:" + path; }

static uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) | ((uint32_t)p[2] << 7) | p[3];
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string decodeTagText(uint8_t enc, const uint8_t* data, size_t len) {
    std::string out;
    if (len == 0) return out;
    // strip trailing NULs
    while (len > 0 && data[len - 1] == 0) len--;
    if (enc == 0) {
        for (size_t i = 0; i < len; i++) out += (char)data[i];
    } else if (enc == 3) {
        for (size_t i = 0; i < len; i++) out += (char)data[i];
    } else if (enc == 1 || enc == 2) {
        size_t i = 0;
        bool be = false;
        if (len >= 2) {
            if (data[0] == 0xFF && data[1] == 0xFE) { i = 2; be = false; }
            else if (data[0] == 0xFE && data[1] == 0xFF) { i = 2; be = true; }
            else if (enc == 2) be = true;
        }
        for (; i + 1 < len; i += 2) {
            uint32_t ch = be ? ((data[i] << 8) | data[i + 1]) : ((data[i + 1] << 8) | data[i]);
            appendUtf8(out, ch);
        }
    }
    return out;
}

static bool readId3v2(FILE* f, Track& t) {
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) != 10) return false;
    if (memcmp(hdr, "ID3", 3) != 0) return false;
    int ver = hdr[3];
    uint32_t size = syncsafe(hdr + 6);
    if (size > 4 * 1024 * 1024) return false;
    std::vector<uint8_t> buf(size);
    if (fread(buf.data(), 1, size, f) != size) return false;

    size_t pos = 0;
    if (hdr[5] & 0x40) { // extended header
        if (size < 4) return false;
        uint32_t ext = ver >= 4 ? syncsafe(buf.data()) : be32(buf.data());
        pos = ext + (ver >= 4 ? 0 : 4);
    }
    while (pos + 10 <= size) {
        const uint8_t* frame = buf.data() + pos;
        if (frame[0] == 0) break;
        char id[5] = {(char)frame[0], (char)frame[1], (char)frame[2], (char)frame[3], 0};
        uint32_t fsize = ver >= 4 ? syncsafe(frame + 4) : be32(frame + 4);
        pos += 10;
        if (fsize == 0 || pos + fsize > size) break;
        std::string key(id);
        if (key == "TIT2") t.title = decodeTagText(frame[10], buf.data() + pos, fsize);
        else if (key == "TPE1") t.artist = decodeTagText(frame[10], buf.data() + pos, fsize);
        else if (key == "TALB") t.album = decodeTagText(frame[10], buf.data() + pos, fsize);
        pos += fsize;
    }
    return true;
}

static bool readId3v1(FILE* f, Track& t) {
    uint8_t buf[128];
    if (fseek(f, -128, SEEK_END) != 0) return false;
    if (fread(buf, 1, 128, f) != 128) return false;
    if (memcmp(buf, "TAG", 3) != 0) return false;
    auto field = [&](int off, int len) {
        std::string s((const char*)buf + off, (size_t)len);
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        return util::trim(s);
    };
    if (t.title.empty()) t.title = field(3, 30);
    if (t.artist.empty()) t.artist = field(33, 30);
    if (t.album.empty()) t.album = field(63, 30);
    return true;
}

void fillMetadata(Track& t) {
    std::string name = util::basename(t.path);
    std::string stem = name;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    std::string fakeTitle = stem, fakeArtist;
    size_t dash = stem.find(" - ");
    if (dash != std::string::npos) {
        fakeArtist = util::trim(stem.substr(0, dash));
        fakeTitle = util::trim(stem.substr(dash + 3));
    }

    FILE* f = fopen(t.path.c_str(), "rb");
    if (f) {
        readId3v2(f, t);
        readId3v1(f, t);
        fclose(f);
    }
    if (t.title.empty()) t.title = fakeTitle;
    if (t.artist.empty()) t.artist = fakeArtist;
    if (t.artist.empty()) t.artist = tr("Неизвестный исполнитель");
    if (t.title.empty()) t.title = name;
    t.cover = t.artist + "/" + t.title;
    t.id = idForPath(t.path);
    t.liked = isLiked(t.id);
}

void scanLocal(const std::string& root, std::vector<Track>& out, int depth) {
    if (depth > 8) return;
    DIR* d = opendir(root.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || name[0] == '.') continue;
        std::string full = util::joinPath(root, name);
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scanLocal(full, out, depth + 1);
        } else if (S_ISREG(st.st_mode) && audioExt(name)) {
            Track t;
            t.path = full;
            fillMetadata(t);
            out.push_back(std::move(t));
        }
    }
    closedir(d);
}

static std::string clean(const std::string& s) {
    std::string out = s;
    for (auto& c : out) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

void saveCache(const std::vector<Track>& tracks) {
    std::string data;
    for (auto& t : tracks) {
        if (!t.cloud || t.path.empty()) continue;
        data += clean(t.sc_id) + "\t" + clean(t.title) + "\t" + clean(t.artist) + "\t" +
                std::to_string(t.duration) + "\t" + clean(t.path) + "\t" + clean(t.cover) + "\n";
    }
    util::writeFile(util::joinPath(cfg.cache_dir, "index.tsv"), data);
}

void loadCache(std::vector<Track>& out) {
    std::string data = util::readFile(util::joinPath(cfg.cache_dir, "index.tsv"));
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(start, nl - start);
        start = nl + 1;
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t p = 0;
        while (true) {
            size_t tab = line.find('\t', p);
            if (tab == std::string::npos) { f.push_back(line.substr(p)); break; }
            f.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (f.size() < 6) continue;
        if (!util::fileExists(f[4])) continue;
        Track t;
        t.cloud = true;
        t.cached = true;
        t.sc_id = f[0];
        t.id = "sc:" + f[0];
        t.title = f[1];
        t.artist = f[2];
        t.duration = atoi(f[3].c_str());
        t.path = f[4];
        t.cover = f[5];
        t.liked = isLiked(t.id);
        out.push_back(std::move(t));
    }
}

} // namespace library

namespace library {

static bool isSystemDir(const std::string& path, const std::string& name) {
    std::string low = util::lower(name);
    static const char* topSkip[] = {
        "nintendo", "atmosphere", "bootloader", "emummc", "backup", "firmware",
        "!nsp", "game", "mods", "mods2", "saltySD", "config", "themes", "emuiibo",
        "amibo", "lost.dir", ".trashes", ".spotlight-v100", ".fseventsd",
        ".temporaryitems", "system volume information"
    };
    std::string lowPath = util::lower(path);
    if (lowPath == "sdmc:/" || lowPath == "sdmc:") {
        for (const char* s : topSkip) if (low == util::lower(s)) return true;
    }
    if (lowPath == "sdmc:/switch/scswitch/cache") return true;
    if (lowPath == "sdmc:/switch/appstore") return true;
    if (lowPath == "sdmc:/switch/.overlays" || lowPath == "sdmc:/switch/.packages") return true;
    return false;
}

static void scanWholeRec(const std::string& root, std::vector<Track>& out, int depth) {
    if (depth > 6) return;
    DIR* d = opendir(root.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || name.empty() || name[0] == '.') continue;
        std::string full = util::joinPath(root, name);
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (isSystemDir(full, name)) continue;
            scanWholeRec(full, out, depth + 1);
        } else if (S_ISREG(st.st_mode) && audioExt(name)) {
            Track t;
            t.path = full;
            fillMetadata(t);
            out.push_back(std::move(t));
        }
    }
    closedir(d);
}

void scanWholeSd(std::vector<Track>& out) {
    scanWholeRec("sdmc:/", out, 0);
}

} // namespace library
