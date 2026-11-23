#include "sc.h"
#include "i18n.h"
#include <cstring>
#include "core.h"
#include "json.h"
#include "http.h"
#include <cstdlib>

namespace sc {

bool configured() { return !cfg.client_id.empty(); }

std::string authHeader() {
    if (cfg.oauth_token.empty()) return "";
    return "Authorization: OAuth " + cfg.oauth_token + "\r\n";
}

static std::string jsonHeaders() {
    return "Accept: application/json\r\n" + authHeader();
}

std::string cachePath(const std::string& trackId) {
    return util::joinPath(cfg.cache_dir, "sc_" + trackId + ".mp3");
}

std::string hlsCachePath(const std::string& trackId) {
    return util::joinPath(cfg.cache_dir, "sc_" + trackId + ".m4a");
}

std::string findCached(const std::string& trackId) {
    std::string mp3 = cachePath(trackId);
    if (util::fileExists(mp3)) return mp3;
    std::string m4a = hlsCachePath(trackId);
    if (util::fileExists(m4a)) return m4a;
    return "";
}

static std::string api(const std::string& pathAndQuery) {
    std::string sep = pathAndQuery.find('?') == std::string::npos ? "?" : "&";
    return "https://api-v2.soundcloud.com" + pathAndQuery + sep + "client_id=" + util::urlEncode(cfg.client_id);
}

bool search(const std::string& query, std::vector<TrackInfo>& out, std::string& err, int limit) {
    out.clear();
    if (!configured()) { err = tr("client_id не задан"); return false; }
    std::string url = api("/search/tracks?q=" + util::urlEncode(query) +
                          "&limit=" + std::to_string(limit) + "&offset=0&linked_partitioning=1&app_locale=ru");
    net::Response r = net::get(url, jsonHeaders());
    if (r.status != 200) {
        err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        if (r.status == 401 || r.status == 403) err = tr("client_id отклонён SoundCloud");
        util::log("sc: search failed: %s (%d)", err.c_str(), r.status);
        return false;
    }
    Json root;
    std::string jerr;
    if (!Json::parse(r.body, root, &jerr)) { err = "JSON: " + jerr; return false; }
    const Json* collection = root.get("collection");
    if (!collection || !collection->isArray()) { err = tr("нет результатов"); return false; }
    for (size_t i = 0; i < collection->size(); i++) {
        const Json* item = collection->at(i);
        if (!item) continue;
        TrackInfo t;
        if (const Json* id = item->get("id")) t.id = util::fmt("%lld", id->integer());
        t.title = item->get("title") ? item->get("title")->string() : tr("Без названия");
        if (const Json* user = item->get("user"))
            if (const Json* name = user->get("username")) t.user = name->string();
        if (const Json* p = item->get("permalink_url")) t.permalink = p->string();
        if (const Json* a = item->get("artwork_url")) t.artwork = a->string();
        if (const Json* d = item->get("duration")) t.durationMs = (int)d->number();
        if (const Json* s = item->get("streamable")) t.streamable = s->boolean(true);
        if (!t.id.empty()) out.push_back(t);
    }
    util::log("sc: search '%s' -> %d results", query.c_str(), (int)out.size());
    return true;
}

bool resolveProgressiveUrl(const std::string& trackId, std::string& url, std::string& err) {
    if (!configured()) { err = tr("client_id не задан"); return false; }
    net::Response r = net::get(api("/tracks/" + trackId), jsonHeaders());
    if (r.status != 200) {
        err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        return false;
    }
    Json track;
    if (!Json::parse(r.body, track, &err)) { err = "JSON: " + err; return false; }
    const Json* media = track.get("media");
    const Json* transcodings = media ? media->get("transcodings") : nullptr;
    if (!transcodings || !transcodings->isArray()) { err = tr("нет транскодингов"); return false; }

    std::string transcodingUrl;
    bool hasHls = false;
    for (size_t i = 0; i < transcodings->size(); i++) {
        const Json* t = transcodings->at(i);
        if (!t) continue;
        const Json* fmt = t->get("format");
        if (!fmt) continue;
        const Json* proto = fmt->get("protocol");
        const Json* mime = fmt->get("mime_type");
        bool progressive = proto && proto->string() == "progressive";
        bool mpeg = mime && mime->string().find("mpeg") != std::string::npos;
        if (proto && proto->string() == "hls") hasHls = true;
        if (progressive && mpeg) {
            if (const Json* u = t->get("url")) transcodingUrl = u->string();
            if (!transcodingUrl.empty()) break;
        }
    }
    if (transcodingUrl.empty()) {
        err = hasHls ? tr("только HLS/AAC: встроенный плеер не умеет, скачай через ПК (tools/fetch_playlist.sh)")
                     : tr("нет progressive MP3 потока");
        return false;
    }

    std::string sep = transcodingUrl.find('?') == std::string::npos ? "?" : "&";
    transcodingUrl += sep + std::string("client_id=") + util::urlEncode(cfg.client_id);
    if (const Json* auth = track.get("track_authorization")) {
        std::string token = auth->string();
        if (!token.empty()) transcodingUrl += "&track_authorization=" + util::urlEncode(token);
    }

    net::Response r2 = net::get(transcodingUrl, jsonHeaders());
    if (r2.status != 200) {
        err = r2.error.empty() ? util::fmt("HTTP %d", r2.status) : r2.error;
        return false;
    }
    Json stream;
    if (!Json::parse(r2.body, stream, &err)) { err = "JSON: " + err; return false; }
    const Json* u = stream.get("url");
    if (!u) { err = tr("нет URL потока"); return false; }
    url = u->string();
    return !url.empty();
}

static bool resolveTranscoding(const std::string& trackId, const char* protocol,
                               const char* mimeNeedle, std::string& url, std::string& err) {
    if (!configured()) { err = tr("client_id не задан"); return false; }
    net::Response r = net::get(api("/tracks/" + trackId), jsonHeaders());
    if (r.status != 200) {
        err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        return false;
    }
    Json track;
    if (!Json::parse(r.body, track, &err)) { err = "JSON: " + err; return false; }
    const Json* media = track.get("media");
    const Json* transcodings = media ? media->get("transcodings") : nullptr;
    if (!transcodings || !transcodings->isArray()) { err = tr("нет транскодингов"); return false; }

    std::string transcodingUrl;
    for (size_t i = 0; i < transcodings->size(); i++) {
        const Json* t = transcodings->at(i);
        if (!t) continue;
        const Json* fmt = t->get("format");
        if (!fmt) continue;
        const Json* proto = fmt->get("protocol");
        const Json* mime = fmt->get("mime_type");
        if (!proto || proto->string() != protocol) continue;
        if (mimeNeedle && (!mime || mime->string().find(mimeNeedle) == std::string::npos)) continue;
        if (const Json* u = t->get("url")) transcodingUrl = u->string();
        if (!transcodingUrl.empty()) break;
    }
    if (transcodingUrl.empty()) { err = std::string(tr("нет ")) + protocol + tr(" потока"); return false; }

    std::string sep = transcodingUrl.find('?') == std::string::npos ? "?" : "&";
    transcodingUrl += sep + std::string("client_id=") + util::urlEncode(cfg.client_id);
    if (const Json* auth = track.get("track_authorization")) {
        std::string token = auth->string();
        if (!token.empty()) transcodingUrl += "&track_authorization=" + util::urlEncode(token);
    }
    net::Response r2 = net::get(transcodingUrl, jsonHeaders());
    if (r2.status != 200) {
        err = r2.error.empty() ? util::fmt("HTTP %d", r2.status) : r2.error;
        return false;
    }
    Json stream;
    if (!Json::parse(r2.body, stream, &err)) { err = "JSON: " + err; return false; }
    const Json* u = stream.get("url");
    if (!u) { err = tr("нет URL потока"); return false; }
    url = u->string();
    return !url.empty();
}

bool resolveHlsUrl(const std::string& trackId, std::string& url, std::string& err) {
    return resolveTranscoding(trackId, "hls", nullptr, url, err);
}

} // namespace sc

namespace sc {

static PlaylistInfo playlistFromJson(const Json& item) {
    PlaylistInfo p;
    if (const Json* id = item.get("id")) p.id = util::fmt("%lld", id->integer());
    if (const Json* t = item.get("title")) p.title = t->string();
    if (const Json* u = item.get("user"))
        if (const Json* n = u->get("username")) p.user = n->string();
    if (const Json* a = item.get("artwork_url")) p.artwork = a->string();
    if (const Json* pl = item.get("permalink_url")) p.permalink = pl->string();
    if (const Json* tc = item.get("track_count")) p.trackCount = (int)tc->number();
    else if (const Json* tracks = item.get("tracks"))
        if (tracks->isArray()) p.trackCount = (int)tracks->size();
    return p;
}

bool searchPlaylists(const std::string& query, std::vector<PlaylistInfo>& out, std::string& err, int limit) {
    out.clear();
    if (!configured()) { err = tr("client_id не задан"); return false; }
    std::string url = api("/search/playlists?q=" + util::urlEncode(query) +
                          "&limit=" + std::to_string(limit) + "&offset=0&linked_partitioning=1&app_locale=ru");
    net::Response r = net::get(url, jsonHeaders());
    if (r.status != 200) {
        err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        if (r.status == 401 || r.status == 403) err = tr("client_id отклонён SoundCloud");
        util::log("sc: playlist search failed: %s (%d)", err.c_str(), r.status);
        return false;
    }
    Json root;
    std::string jerr;
    if (!Json::parse(r.body, root, &jerr)) { err = "JSON: " + jerr; return false; }
    const Json* collection = root.get("collection");
    if (!collection || !collection->isArray()) { err = tr("нет результатов"); return false; }
    for (size_t i = 0; i < collection->size(); i++) {
        const Json* item = collection->at(i);
        if (!item) continue;
        PlaylistInfo p = playlistFromJson(*item);
        if (!p.id.empty()) out.push_back(p);
    }
    util::log("sc: playlist search '%s' -> %d results", query.c_str(), (int)out.size());
    return true;
}

static std::string extractCanonical(const std::string& html) {
    // Share links (on.soundcloud.com/xxxx) redirect to the canonical page. Our
    // HTTP client follows redirects, so read the canonical URL from the HTML.
    const char* marks[] = {"<link rel=\"canonical\" href=\"", "property=\"og:url\" content=\""};
    for (const char* mark : marks) {
        size_t p = html.find(mark);
        if (p == std::string::npos) continue;
        p += strlen(mark);
        size_t e = html.find('"', p);
        if (e != std::string::npos && e > p) return html.substr(p, e - p);
    }
    return "";
}

bool resolvePlaylistUrl(const std::string& url, std::vector<PlaylistInfo>& out, std::string& err) {
    out.clear();
    if (!configured()) { err = tr("client_id не задан"); return false; }
    std::string target = util::trim(url);
    if (target.find("://") == std::string::npos) target = "https://" + target;

    if (target.find("on.soundcloud.com") != std::string::npos ||
        target.find("//soundcloud.app.goo.gl") != std::string::npos) {
        net::Response page = net::get(target);
        std::string canonical = extractCanonical(page.body);
        if (!canonical.empty()) target = canonical;
    }

    net::Response r = net::get(api("/resolve?url=" + util::urlEncode(target)), jsonHeaders());
    if (r.status != 200) {
        if (r.status == 404)
            err = tr("404: плейлист/аккаунт не найден или приватный (публичный API видит только открытые)");
        else
            err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        util::log("sc: resolve %s failed (%d) %s", target.c_str(), r.status, err.c_str());
        return false;
    }
    Json root;
    if (!Json::parse(r.body, root, &err)) { err = "JSON: " + err; return false; }
    const Json* kind = root.get("kind");
    std::string k = kind ? kind->string() : "";
    if (k == "playlist" || (!root.get("tracks") && root.get("title"))) {
        PlaylistInfo p = playlistFromJson(root);
        if (!p.id.empty()) out.push_back(p);
        return !out.empty();
    }
    if (k == "user" || root.get("username")) {
        std::string uid = root.get("id") ? util::fmt("%lld", root.get("id")->integer()) : "";
        if (uid.empty()) { err = tr("не удалось определить аккаунт"); return false; }
        net::Response r2 = net::get(api("/users/" + uid + "/playlists?limit=50&offset=0"),
                                    jsonHeaders());
        if (r2.status != 200) { err = util::fmt("HTTP %d", r2.status); return false; }
        Json list;
        if (!Json::parse(r2.body, list, &err)) { err = "JSON: " + err; return false; }
        const Json* collection = list.get("collection");
        if (!collection || !collection->isArray()) { err = tr("нет плейлистов"); return false; }
        for (size_t i = 0; i < collection->size(); i++) {
            const Json* item = collection->at(i);
            if (!item) continue;
            PlaylistInfo p = playlistFromJson(*item);
            if (!p.id.empty()) out.push_back(p);
        }
        if (out.empty()) { err = tr("у аккаунта нет публичных плейлистов"); return false; }
        return true;
    }
    err = tr("это не плейлист и не аккаунт");
    return false;
}

bool getPlaylistTracks(const std::string& playlistId, std::vector<TrackInfo>& out,
                       std::string& title, std::string& err) {
    out.clear();
    if (!configured()) { err = tr("client_id не задан"); return false; }
    net::Response r = net::get(api("/playlists/" + playlistId), jsonHeaders());
    if (r.status != 200) {
        err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
        return false;
    }
    Json root;
    if (!Json::parse(r.body, root, &err)) { err = "JSON: " + err; return false; }
    if (const Json* t = root.get("title")) title = t->string();
    const Json* tracks = root.get("tracks");
    if (!tracks || !tracks->isArray()) { err = tr("в плейлисте нет треков"); return false; }
    for (size_t i = 0; i < tracks->size(); i++) {
        const Json* item = tracks->at(i);
        if (!item || item->isNull()) continue;
        TrackInfo t;
        if (const Json* id = item->get("id")) t.id = util::fmt("%lld", id->integer());
        if (t.id.empty()) continue;
        if (const Json* ti = item->get("title")) t.title = ti->string();
        if (const Json* u = item->get("user"))
            if (const Json* n = u->get("username")) t.user = n->string();
        if (const Json* d = item->get("duration")) t.durationMs = (int)d->number();
        if (const Json* p = item->get("permalink_url")) t.permalink = p->string();
        out.push_back(t);
    }
    return !out.empty();
}

} // namespace sc

namespace sc {

// api-v2 has no /me endpoint (it returns 404). SoundCloud OAuth tokens look
// like "2-<app>-<user id>-<secret>", so pull the user id out of the token and
// use /users/<id>/playlists, which with Authorization also returns the private
// playlists of the account.
static std::string userIdFromToken() {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= cfg.oauth_token.size()) {
        size_t dash = cfg.oauth_token.find('-', start);
        if (dash == std::string::npos) dash = cfg.oauth_token.size();
        parts.push_back(cfg.oauth_token.substr(start, dash - start));
        if (dash == cfg.oauth_token.size()) break;
        start = dash + 1;
    }
    if (parts.size() < 4) return "";
    const std::string& id = parts[2];
    if (id.empty()) return "";
    for (char c : id) if (c < '0' || c > '9') return "";
    return id;
}

bool myPlaylists(std::vector<PlaylistInfo>& out, std::string& err) {
    out.clear();
    if (!configured()) { err = tr("client_id не задан"); return false; }
    if (cfg.oauth_token.empty()) { err = tr("нужен OAuth token (настройки → OAuth)"); return false; }
    std::string uid = userIdFromToken();
    if (uid.empty()) { err = tr("не удалось прочитать user id из токена"); return false; }

    std::string next = api("/users/" + uid + "/playlists?limit=100&offset=0&linked_partitioning=1");
    for (int page = 0; page < 5 && !next.empty(); page++) {
        net::Response r = net::get(next, jsonHeaders());
        if (r.status != 200) {
            err = r.error.empty() ? util::fmt("HTTP %d", r.status) : r.error;
            if (r.status == 401) err = tr("OAuth token не принят");
            util::log("sc: my playlists failed (%d) %s", r.status, err.c_str());
            return false;
        }
        Json root;
        if (!Json::parse(r.body, root, &err)) { err = "JSON: " + err; return false; }
        const Json* collection = root.get("collection");
        if (collection && collection->isArray()) {
            for (size_t i = 0; i < collection->size(); i++) {
                const Json* item = collection->at(i);
                if (!item) continue;
                PlaylistInfo p = playlistFromJson(*item);
                if (!p.id.empty()) out.push_back(p);
            }
        }
        next.clear();
        if (const Json* nh = root.get("next_href")) {
            std::string n = nh->string();
            if (n.rfind("http", 0) == 0) next = n;
        }
    }
    util::log("sc: my playlists (uid %s) -> %d", uid.c_str(), (int)out.size());
    return true;
}

} // namespace sc
