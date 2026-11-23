#include "hls.h"
#include "i18n.h"
#include "core.h"
#include <cstdio>
#include <vector>

namespace hls {

static std::string trim(const std::string& s) {
    return util::trim(s);
}

static std::string resolveRef(const std::string& base, const std::string& ref) {
    if (ref.compare(0, 4, "http") == 0) return ref;
    size_t scheme = base.find("://");
    if (ref.compare(0, 2, "//") == 0) return base.substr(0, scheme) + ":" + ref;
    if (!ref.empty() && ref[0] == '/') {
        size_t hostEnd = base.find('/', scheme + 3);
        return base.substr(0, hostEnd == std::string::npos ? base.size() : hostEnd) + ref;
    }
    size_t slash = base.find_last_of('/');
    return base.substr(0, slash == std::string::npos ? base.size() : slash + 1) + ref;
}

static bool attribute(const std::string& line, const char* key, std::string& out) {
    std::string k = std::string(key) + "=\"";
    size_t p = line.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    size_t e = line.find('"', p);
    if (e == std::string::npos) return false;
    out = line.substr(p, e - p);
    return true;
}

static bool parsePlaylist(const std::string& body, const std::string& base,
                          std::string& variant, std::string& init,
                          std::vector<std::string>& segments) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= body.size()) {
        size_t nl = body.find('\n', start);
        if (nl == std::string::npos) nl = body.size();
        std::string line = trim(body.substr(start, nl - start));
        if (!line.empty()) lines.push_back(line);
        if (nl == body.size()) break;
        start = nl + 1;
    }

    bool master = false;
    for (auto& line : lines) {
        if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0) master = true;
    }
    if (master) {
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].compare(0, 18, "#EXT-X-STREAM-INF:") != 0) continue;
            for (size_t j = i + 1; j < lines.size(); j++) {
                if (!lines[j].empty() && lines[j][0] != '#') {
                    variant = resolveRef(base, lines[j]);
                    return true;
                }
            }
        }
        return false;
    }

    for (auto& line : lines) {
        if (line.compare(0, 11, "#EXT-X-MAP:") == 0) {
            std::string uri;
            if (attribute(line, "URI", uri)) init = resolveRef(base, uri);
            continue;
        }
        if (line[0] == '#') continue;
        segments.push_back(resolveRef(base, line));
    }
    return true;
}

bool download(const std::string& m3u8Url, const std::string& destPath,
              net::Progress progress, std::string& error,
              const std::string& extraHeaders) {
    std::string playlistUrl = m3u8Url;
    std::string init;
    std::vector<std::string> segments;

    for (int depth = 0; depth < 3; depth++) {
        net::Response r = net::get(playlistUrl, "Accept: application/vnd.apple.mpegurl, */*\r\n" + extraHeaders, 12000);
        if (r.status != 200 || r.body.empty()) {
            error = r.error.empty() ? util::fmt("HLS playlist HTTP %d", r.status) : r.error;
            return false;
        }
        std::string variant;
        init.clear();
        segments.clear();
        if (!parsePlaylist(r.body, playlistUrl, variant, init, segments)) {
            error = tr("не удалось разобрать HLS playlist");
            return false;
        }
        if (!variant.empty()) { playlistUrl = variant; continue; }
        break;
    }
    if (segments.empty()) { error = tr("в HLS нет сегментов"); return false; }

    util::ensureDir(util::dirname(destPath));
    std::string part = destPath + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) { error = tr("не удалось создать файл"); return false; }

    std::vector<std::string> urls;
    if (!init.empty()) urls.push_back(init);
    for (auto& s : segments) urls.push_back(s);

    // Probe the first segment before opening the destination: if the media CDN
    // is blocked/unreachable, fail in seconds instead of timing out on every
    // segment of every track in a playlist.
    net::Response first = net::get(urls[0], "Accept: */*\r\n" + extraHeaders, 8000);
    if (first.status != 200 || first.body.empty()) {
        fclose(f);
        util::removeFile(part);
        error = tr("медиа-CDN не отвечает (включи VPN на ПК/прокси)");
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < urls.size(); i++) {
        const std::string* data = nullptr;
        std::string owned;
        if (i == 0) {
            owned = std::move(first.body);
            data = &owned;
        } else {
            net::Response partResp = net::get(urls[i], "Accept: */*\r\n" + extraHeaders, 8000);
            if (partResp.status != 200 || partResp.body.empty()) {
                error = partResp.error.empty() ? util::fmt("HLS segment HTTP %d", partResp.status)
                                               : partResp.error;
                ok = false;
                break;
            }
            owned = std::move(partResp.body);
            data = &owned;
        }
        if (fwrite(data->data(), 1, data->size(), f) != data->size()) {
            error = tr("ошибка записи HLS сегмента");
            ok = false;
            break;
        }
        progress.call(i + 1, urls.size());
    }
    fclose(f);
    if (!ok) {
        util::removeFile(part);
        return false;
    }
    if (rename(part.c_str(), destPath.c_str()) != 0) {
        util::removeFile(part);
        error = tr("не удалось сохранить файл");
        return false;
    }
    util::log("hls: saved %s (%d segments)", destPath.c_str(), (int)segments.size());
    return true;
}

} // namespace hls
