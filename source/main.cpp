#include <switch.h>
#include <switch/arm/counter.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <atomic>
#include <memory>

#include "core.h"
#include "gfx.h"
#include "audio.h"
#include "library.h"
#include "sc.h"
#include "http.h"
#include "hls.h"
#include "i18n.h"

namespace {

constexpr int W = 1280;
constexpr int H = 720;
constexpr int SIDEBAR_W = 236;
constexpr int TOPBAR_H = 64;
constexpr int PLAYER_H = 104;
constexpr int CONTENT_X = SIDEBAR_W + 28;
constexpr int CONTENT_Y = TOPBAR_H + 16;
constexpr int CONTENT_W = W - CONTENT_X - 28;
constexpr int CONTENT_H = H - PLAYER_H - CONTENT_Y - 8;
constexpr int ROW_H = 64;

enum Screen {
    SCR_HOME = 0, SCR_SEARCH, SCR_PLAYLISTS, SCR_LIBRARY, SCR_LIKED,
    SCR_DOWNLOADS, SCR_SETTINGS, SCR_PLAYLIST, SCR_PLAYER, SCR_COUNT
};

const int kNav[] = {SCR_HOME, SCR_SEARCH, SCR_PLAYLISTS, SCR_LIBRARY,
                    SCR_LIKED, SCR_DOWNLOADS, SCR_SETTINGS};
const int kNavCount = (int)(sizeof(kNav) / sizeof(kNav[0]));

const char* screenTitle(int s) {
    switch (s) {
        case SCR_HOME: return tr("Главная");
        case SCR_SEARCH: return tr("Поиск в SoundCloud");
        case SCR_PLAYLISTS: return tr("Плейлисты SoundCloud");
        case SCR_PLAYLIST: return tr("Плейлист");
        case SCR_LIBRARY: return tr("Моя музыка");
        case SCR_LIKED: return tr("Мне нравится");
        case SCR_DOWNLOADS: return tr("Загрузки");
        case SCR_SETTINGS: return tr("Настройки");
        case SCR_PLAYER: return tr("Сейчас играет");
    }
    return "";
}

double nowSeconds() {
    return (double)armGetSystemTick() / 19200000.0;
}

struct Job {
    int kind = 0; // 1 track search, 2 track download, 3 SD scan, 4 playlist search,
                  // 5 open playlist, 6 resolve URL/account, 7 download playlist
    std::string query;
    std::string playlistId;
    std::string title;
    Track track;
    bool playAfter = false;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::atomic<float> progress{-1};
    std::string oauthUsed;
    std::string error;
    int dlIndex = 0;
    int dlTotal = 1;
    int dlOk = 0;
    std::vector<sc::TrackInfo> found;
    std::vector<Track> foundLocal;
    std::vector<sc::PlaylistInfo> playlists;
    std::vector<Track> playlistTracks;
    Thread thread{};
    bool started = false;
};

struct App {
    Player player;
    std::vector<Track> localTracks;
    std::vector<Track> cachedTracks;
    std::vector<Track> likedList;
    std::vector<Track> results;
    std::vector<sc::PlaylistInfo> playlists;
    std::vector<Track> playlistTracks;
    std::string playlistTitle;
    std::string playlistUser;
    std::string playlistQuery;
    std::vector<Track> queue;
    int queueIndex = -1;
    Track current;
    bool hasCurrent = false;
    bool nextRequested = false;
    bool shuffle = false;

    int screen = SCR_HOME;
    int sel[SCR_COUNT] = {};
    int scroll[SCR_COUNT] = {};
    u64 heldButton = 0;
    double holdStart = 0, lastRepeat = 0, lastMoveAt = 0;
    bool stickUpLatch = false, stickDownLatch = false;
    bool touchActive = false, touchSeeking = false;
    int seekTouchX = 0, seekTouchW = 1;
    double lastTouchSeek = 0;

    std::string query;
    std::string toast;
    double toastUntil = 0;
    float spin = 0;

    Job* job = nullptr;
    bool searching = false;
    bool audioOk = false;
    bool netOnline = false;
    double netCheckAt = 0;

    void showToast(const std::string& msg, double secs = 2.5) {
        toast = msg;
        toastUntil = nowSeconds() + secs;
        util::log("toast: %s", msg.c_str());
    }

    void refreshLiked() {
        likedList.clear();
        for (auto& t : localTracks) if (isLiked(t.id)) likedList.push_back(t);
        for (auto& t : cachedTracks) if (isLiked(t.id)) likedList.push_back(t);
    }

    std::vector<Track>& listForScreen(int s) {
        switch (s) {
            case SCR_LIBRARY: return localTracks;
            case SCR_LIKED: return likedList;
            case SCR_DOWNLOADS: return cachedTracks;
            case SCR_SEARCH: return results;
            case SCR_PLAYLIST: return playlistTracks;
            default: return localTracks;
        }
    }

    int countForScreen(int s) {
        if (s == SCR_PLAYLISTS) return (int)playlists.size();
        return (int)listForScreen(s).size();
    }

    int visibleRows(int s) {
        if (s == SCR_PLAYLISTS) return 6;
        if (s == SCR_PLAYLIST) return 6;
        return 7;
    }

    const char* emptyText(int s) {
        switch (s) {
            case SCR_LIBRARY: return tr("Пока пусто. Положи MP3/FLAC/OGG в sdmc:/switch/scswitch/music");
            case SCR_LIKED: return tr("Ставь ♥ на треках — они появятся здесь");
            case SCR_DOWNLOADS: return tr("Скачай трек из SoundCloud — он останется для офлайна");
            case SCR_SEARCH: return sc::configured() ? tr("Введи запрос и нажми A") : tr("Сначала укажи client_id в настройках");
            case SCR_PLAYLISTS: return tr("Y — поиск плейлистов, X — ссылка на плейлист/аккаунт");
            case SCR_PLAYLIST: return tr("В плейлисте нет доступных треков");
            default: return tr("Пусто");
        }
    }

    // ---------- background jobs ----------

    static void jobEntry(void* arg) {
        Job* j = (Job*)arg;
        switch (j->kind) {
            case 1: runSearch(j); break;
            case 2: runDownload(j); break;
            case 3: runScan(j); break;
            case 4: runPlaylistSearch(j); break;
            case 5: runOpenPlaylist(j); break;
            case 6: runPlaylistUrl(j); break;
            case 7: runDownloadPlaylist(j); break;
            case 8: runMyPlaylists(j); break;
            default: j->done = true; break;
        }
    }

    static void runSearch(Job* j) {
        std::string err;
        j->ok = sc::search(j->query, j->found, err);
        j->error = err;
        j->done = true;
    }

    static void downloadProgress(void* user, uint64_t done, uint64_t total) {
        Job* j = (Job*)user;
        float frac = total > 0 ? (float)((double)done / (double)total) : 0.0f;
        if (j->dlTotal > 0) j->progress = (float)((j->dlIndex + frac) / (double)j->dlTotal);
    }

    static bool downloadOne(Job* j, Track& t) {
        std::string err, url;
        net::Progress progress{&App::downloadProgress, j};
        if (sc::resolveProgressiveUrl(t.sc_id, url, err)) {
            std::string dest = sc::cachePath(t.sc_id);
            if (net::download(url, dest, progress, err, sc::authHeader())) {
                t.path = dest;
                return true;
            }
            util::log("download: mp3 failed %s (%s)", t.title.c_str(), err.c_str());
        }
        std::string perr = err;
        if (sc::resolveHlsUrl(t.sc_id, url, err)) {
            std::string dest = sc::hlsCachePath(t.sc_id);
            if (hls::download(url, dest, progress, err, sc::authHeader())) {
                t.path = dest;
                return true;
            }
            util::log("download: hls failed %s (%s)", t.title.c_str(), err.c_str());
        }
        j->error = perr + " / " + err;
        return false;
    }

    static void runDownload(Job* j) {
        j->dlTotal = 1;
        j->dlIndex = 0;
        Track t = j->track;
        j->ok = downloadOne(j, t);
        t.cached = j->ok;
        j->track = t;
        j->done = true;
    }

    static void runScan(Job* j) {
        library::scanWholeSd(j->foundLocal);
        j->ok = true;
        j->done = true;
    }

    static void runPlaylistSearch(Job* j) {
        std::string err;
        j->ok = sc::searchPlaylists(j->query, j->playlists, err);
        // A single word may just be an account permalink ("tukkkk").
        if (j->ok && j->playlists.empty() && j->query.find(' ') == std::string::npos &&
            j->query.find('/') == std::string::npos) {
            std::vector<sc::PlaylistInfo> alt;
            std::string err2;
            if (sc::resolvePlaylistUrl("https://soundcloud.com/" + j->query, alt, err2) && !alt.empty()) {
                j->playlists = alt;
            }
        }
        j->error = err;
        j->done = true;
    }

    static void runMyPlaylists(Job* j) {
        std::string err;
        j->ok = sc::myPlaylists(j->playlists, err);
        j->error = err;
        j->done = true;
    }

    static void runPlaylistUrl(Job* j) {
        std::string err;
        j->ok = sc::resolvePlaylistUrl(j->query, j->playlists, err);
        j->error = err;
        j->done = true;
    }

    static void runOpenPlaylist(Job* j) {
        std::string err;
        j->ok = sc::getPlaylistTracks(j->playlistId, j->found, j->title, err);
        j->error = err;
        j->done = true;
    }

    static void runDownloadPlaylist(Job* j) {
        j->dlTotal = (int)j->playlistTracks.size();
        j->dlOk = 0;
        int consecutiveFailures = 0;
        for (int i = 0; i < j->dlTotal; i++) {
            j->dlIndex = i;
            Track& t = j->playlistTracks[i];
            j->dlIndex = i;
            std::string cached = sc::findCached(t.sc_id);
            if (!cached.empty()) { t.path = cached; j->dlOk++; consecutiveFailures = 0; continue; }
            if (downloadOne(j, t)) {
                j->dlOk++;
                consecutiveFailures = 0;
            } else if (++consecutiveFailures >= 3) {
                j->error = tr("медиа-CDN недоступен (включи VPN на ПК или проверь прокси)");
                util::log("playlist: aborting after 3 consecutive media failures");
                break;
            }
        }
        j->dlIndex = j->dlTotal;
        j->progress = 1.0f;
        j->ok = j->dlOk > 0;
        if (!j->ok && j->error.empty()) j->error = tr("не удалось скачать ни одного трека");
        j->done = true;
    }

    void startJob(Job* j) {
        Result rc = threadCreate(&j->thread, &App::jobEntry, j, nullptr, 0x80000, 0x2C, -2);
        if (R_FAILED(rc)) {
            j->done = true;
            j->error = tr("не удалось создать поток");
            util::log("job: threadCreate 0x%x", rc);
            return;
        }
        j->started = true;
        rc = threadStart(&j->thread);
        if (R_FAILED(rc)) {
            j->started = false;
            j->done = true;
            j->error = tr("не удалось запустить поток");
            util::log("job: threadStart 0x%x", rc);
        }
    }

    void reapJob() {
        if (!job || !job->done.load()) return;
        if (job->started) {
            threadWaitForExit(&job->thread);
            threadClose(&job->thread);
        }
        if (job->kind == 1) {
            searching = false;
            if (job->ok) {
                results.clear();
                for (auto& info : job->found) results.push_back(fromInfo(info));
                sel[SCR_SEARCH] = 0;
                scroll[SCR_SEARCH] = 0;
                showToast(util::fmt(tr("Найдено: %d"), (int)results.size()));
            } else {
                showToast(tr("Поиск: ") + job->error, 4);
            }
        } else if (job->kind == 8) {
            if (job->ok) {
                playlists = job->playlists;
                sel[SCR_PLAYLISTS] = 0;
                scroll[SCR_PLAYLISTS] = 0;
                showToast(util::fmt(tr("Моих плейлистов: %d"), (int)playlists.size()), 3);
            } else {
                showToast(tr("Аккаунт: ") + job->error, 4);
            }
        } else if (job->kind == 4 || job->kind == 6) {
            if (job->ok) {
                playlists = job->playlists;
                sel[SCR_PLAYLISTS] = 0;
                scroll[SCR_PLAYLISTS] = 0;
                showToast(util::fmt(tr("Плейлистов: %d"), (int)playlists.size()), 3);
            } else {
                showToast(tr("Плейлисты: ") + job->error, 4);
            }
        } else if (job->kind == 5) {
            if (job->ok) {
                playlistTracks.clear();
                for (auto& info : job->found) playlistTracks.push_back(fromInfo(info));
                playlistTitle = job->title;
                screen = SCR_PLAYLIST;
                sel[SCR_PLAYLIST] = 0;
                scroll[SCR_PLAYLIST] = 0;
                showToast(util::fmt(tr("%s · %d треков"), playlistTitle.c_str(), (int)playlistTracks.size()), 3);
            } else {
                showToast(tr("Плейлист: ") + job->error, 4);
            }
        } else if (job->kind == 7) {
            int added = 0;
            for (auto& t : job->playlistTracks) {
                if (!util::fileExists(t.path)) continue;
                t.cached = true;
                bool exists = false;
                for (auto& c : cachedTracks) if (c.id == t.id) { c = t; exists = true; break; }
                if (!exists) { cachedTracks.push_back(t); added++; }
            }
            library::saveCache(cachedTracks);
            refreshLiked();
            showToast(util::fmt(tr("Плейлист: скачано %d из %d (новых %d)"),
                                 job->dlOk, job->dlTotal, added), 5);
        } else if (job->kind == 3) {
            for (auto& t : job->foundLocal) {
                bool exists = false;
                for (auto& l : localTracks) if (l.path == t.path) { exists = true; break; }
                if (!exists) localTracks.push_back(t);
            }
            std::sort(localTracks.begin(), localTracks.end(), [](const Track& a, const Track& b) {
                return util::lower(a.artist + " " + a.title) < util::lower(b.artist + " " + b.title);
            });
            sel[SCR_LIBRARY] = 0;
            scroll[SCR_LIBRARY] = 0;
            refreshLiked();
            showToast(util::fmt(tr("Вся SD: найдено %d, всего %d"), (int)job->foundLocal.size(),
                                 (int)localTracks.size()), 4);
        } else {
            Track t = job->track;
            if (job->ok) {
                t.cached = true;
                bool exists = false;
                for (auto& c : cachedTracks) if (c.id == t.id) { c = t; exists = true; }
                if (!exists) cachedTracks.push_back(t);
                library::saveCache(cachedTracks);
                refreshLiked();
                if (job->playAfter) {
                    queue.clear();
                    for (auto& c : cachedTracks) queue.push_back(c);
                    queueIndex = 0;
                    for (size_t i = 0; i < queue.size(); i++) if (queue[i].id == t.id) queueIndex = (int)i;
                    player.playFile(t.path);
                    current = t;
                    hasCurrent = true;
                }
                showToast(job->playAfter ? tr("Готово, играю") : tr("Трек сохранён"));
            } else {
                showToast(tr("Загрузка: ") + job->error, 4);
            }
        }
        delete job;
        job = nullptr;
    }

    Track fromInfo(const sc::TrackInfo& info) {
        Track t;
        t.cloud = true;
        t.sc_id = info.id;
        t.id = "sc:" + info.id;
        t.title = info.title;
        t.artist = info.user.empty() ? "SoundCloud" : info.user;
        t.permalink = info.permalink;
        t.duration = info.durationMs / 1000;
        t.cover = info.user + "/" + info.title;
        std::string cached = sc::findCached(info.id);
        t.path = cached.empty() ? sc::cachePath(info.id) : cached;
        t.cached = !cached.empty();
        t.liked = isLiked(t.id);
        return t;
    }

    // ---------- playback ----------

    bool isCurrentTrack(const Track& t) {
        return hasCurrent && current.id == t.id && player.loaded();
    }

    void playTrack(const Track& t, std::vector<Track>* list, int index) {
        queue.clear();
        if (list) queue = *list;
        else queue.push_back(t);
        queueIndex = index >= 0 ? index : 0;
        if (shuffle && queue.size() > 1) {
            std::mt19937 rng((unsigned)nowSeconds() ^ util::hashStr(t.id));
            std::shuffle(queue.begin(), queue.end(), rng);
            queueIndex = 0;
            for (size_t i = 0; i < queue.size(); i++)
                if (queue[i].id == t.id) { queueIndex = (int)i; break; }
        }
        if (t.cloud && !util::fileExists(t.path)) {
            if (job) { showToast(tr("Дождись текущей задачи")); return; }
            Job* j = new Job();
            j->kind = 2;
            j->track = t;
            j->playAfter = true;
            job = j;
            startJob(j);
            showToast(tr("Загружаю трек из SoundCloud…"), 3);
            return;
        }
        current = t;
        if (current.cloud) current.cached = util::fileExists(current.path);
        hasCurrent = true;
        player.playFile(current.path);
    }

    void playNext(bool forward) {
        if (queue.empty()) return;
        if (queueIndex < 0) queueIndex = 0;
        if (shuffle && queue.size() > 1) {
            std::mt19937 rng((unsigned)nowSeconds());
            for (int tries = 0; tries < 32; tries++) {
                int idx = (int)(rng() % queue.size());
                if (idx == queueIndex) continue;
                Track t = queue[idx];
                if (t.cloud && !util::fileExists(t.path)) continue;
                queueIndex = idx;
                current = t;
                hasCurrent = true;
                player.playFile(t.path);
                return;
            }
        }
        for (size_t tries = 0; tries < queue.size(); tries++) {
            queueIndex += forward ? 1 : -1;
            if (queueIndex >= (int)queue.size()) queueIndex = 0;
            if (queueIndex < 0) queueIndex = (int)queue.size() - 1;
            Track t = queue[queueIndex];
            if (t.cloud && !util::fileExists(t.path)) continue;
            current = t;
            hasCurrent = true;
            player.playFile(t.path);
            return;
        }
    }

    void toggleLikeCurrent() {
        Track* t = nullptr;
        if (screen == SCR_PLAYER) t = hasCurrent ? &current : nullptr;
        else {
            auto& list = listForScreen(screen);
            if (!list.empty()) {
                int idx = sel[screen] + scroll[screen];
                if (idx >= 0 && idx < (int)list.size()) t = &list[idx];
            }
        }
        if (!t) return;
        bool liked = !isLiked(t->id);
        setLiked(t->id, liked);
        t->liked = liked;
        for (auto& x : localTracks) if (x.id == t->id) x.liked = liked;
        for (auto& x : cachedTracks) if (x.id == t->id) x.liked = liked;
        for (auto& x : results) if (x.id == t->id) x.liked = liked;
        refreshLiked();
        showToast(liked ? tr("Добавлено в ♥") : tr("Убрано из ♥"));
    }

    // ---------- actions ----------

    bool prompt(const char* header, const std::string& initial, int maxLen, std::string& out) {
        SwkbdConfig kbd;
        if (R_FAILED(swkbdCreate(&kbd, 0))) { showToast(tr("Клавиатура недоступна")); return false; }
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, SwkbdType_Normal);
        swkbdConfigSetHeaderText(&kbd, header);
        swkbdConfigSetGuideText(&kbd, tr("Ввод закроется по кнопке ОК"));
        swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
        if (!initial.empty()) swkbdConfigSetInitialText(&kbd, initial.c_str());
        char buf[1024] = {0};
        Result rc = swkbdShow(&kbd, buf, sizeof(buf));
        swkbdClose(&kbd);
        if (R_FAILED(rc)) return false;
        out = buf;
        return true;
    }

    void doSearch() {
        if (!sc::configured()) {
            screen = SCR_SETTINGS;
            showToast(tr("Сначала введи client_id"), 3);
            return;
        }
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        if (!prompt(tr("Поиск в SoundCloud"), query, 120, query)) return;
        if (query.empty()) return;
        Job* j = new Job();
        j->kind = 1;
        j->query = query;
        job = j;
        searching = true;
        startJob(j);
    }

    void switchTab(int delta) {
        if (screen == SCR_PLAYLIST) return;
        int pos = 0;
        for (int i = 0; i < kNavCount; i++) if (kNav[i] == screen) { pos = i; break; }
        pos = (pos + delta + kNavCount) % kNavCount;
        screen = kNav[pos];
        sel[screen] = std::min(sel[screen], std::max(0, countForScreen(screen) - 1));
    }

    void doPlaylistSearch() {
        if (!sc::configured()) {
            screen = SCR_SETTINGS;
            showToast(tr("Сначала введи client_id"), 3);
            return;
        }
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        if (!prompt(tr("Поиск плейлистов или ссылка"), playlistQuery, 300, playlistQuery)) return;
        std::string q = util::trim(playlistQuery);
        if (q.empty()) return;
        playlistQuery = q;
        Job* j = new Job();
        bool looksLikeUrl = q.find("soundcloud.com") != std::string::npos ||
                            q.compare(0, 4, "http") == 0 || q.find('/') != std::string::npos;
        j->kind = looksLikeUrl ? 6 : 4;
        j->query = q;
        job = j;
        startJob(j);
        showToast(looksLikeUrl ? tr("Открываю ссылку…") : tr("Ищу плейлисты…"), 3);
    }

    void openPlaylistUrl() {
        if (!sc::configured()) {
            screen = SCR_SETTINGS;
            showToast(tr("Сначала введи client_id"), 3);
            return;
        }
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        std::string url;
        if (!prompt(tr("Ссылка на плейлист или аккаунт soundcloud.com"), "", 300, url)) return;
        if (util::trim(url).empty()) return;
        Job* j = new Job();
        j->kind = 6;
        j->query = util::trim(url);
        job = j;
        startJob(j);
        showToast(tr("Открываю ссылку…"), 3);
    }

    void loadMyPlaylists() {
        if (!sc::configured()) { screen = SCR_SETTINGS; showToast(tr("Сначала введи client_id"), 3); return; }
        if (cfg.oauth_token.empty()) { screen = SCR_SETTINGS; showToast(tr("Нужен OAuth token (настройки)"), 4); return; }
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        Job* j = new Job();
        j->kind = 8;
        job = j;
        startJob(j);
        showToast(tr("Загружаю мои плейлисты…"), 3);
    }

    void openPlaylistAt(int idx) {
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        if (idx < 0 || idx >= (int)playlists.size()) return;
        Job* j = new Job();
        j->kind = 5;
        j->playlistId = playlists[idx].id;
        j->title = playlists[idx].title;
        playlistUser = playlists[idx].user;
        job = j;
        startJob(j);
        showToast(tr("Загружаю плейлист…"), 3);
    }

    void downloadCurrentPlaylist() {
        if (playlistTracks.empty()) { showToast(tr("Плейлист пуст")); return; }
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        Job* j = new Job();
        j->kind = 7;
        j->playlistTracks = playlistTracks;
        j->title = playlistTitle;
        j->progress = 0;
        job = j;
        startJob(j);
        showToast(util::fmt(tr("Качаю плейлист: %d треков…"), (int)playlistTracks.size()), 4);
    }

    void toggleShuffle() {
        shuffle = !shuffle;
        if (shuffle && queue.size() > 1) {
            std::string cur = hasCurrent ? current.id : "";
            std::mt19937 rng((unsigned)nowSeconds());
            std::shuffle(queue.begin(), queue.end(), rng);
            queueIndex = 0;
            for (size_t i = 0; i < queue.size(); i++)
                if (queue[i].id == cur) { queueIndex = (int)i; break; }
        }
        showToast(shuffle ? tr("Перемешивание включено") : tr("Перемешивание выключено"));
    }

    void rescan() {
        localTracks.clear();
        library::scanLocal(cfg.music_dir, localTracks);
        if (cfg.music_dir != "sdmc:/music") library::scanLocal("sdmc:/music", localTracks);
        library::scanLocal("sdmc:/Music", localTracks);
        // de-duplicate by path
        std::sort(localTracks.begin(), localTracks.end(), [](const Track& a, const Track& b) {
            return a.path < b.path;
        });
        localTracks.erase(std::unique(localTracks.begin(), localTracks.end(),
                                     [](const Track& a, const Track& b) { return a.path == b.path; }),
                          localTracks.end());
        sel[SCR_LIBRARY] = 0;
        scroll[SCR_LIBRARY] = 0;
        refreshLiked();
        showToast(util::fmt(tr("Найдено треков: %d"), (int)localTracks.size()));
    }

    void openSettingsClientId() {
        std::string v = cfg.client_id;
        if (!prompt("client_id SoundCloud", v, 200, v)) return;
        if (v != cfg.client_id) {
            cfg.client_id = util::trim(v);
            cfg.save();
            showToast(tr("client_id сохранён"));
        }
    }

    void openOAuthToken() {
        std::string v = cfg.oauth_token;
        if (!prompt(tr("OAuth token SoundCloud (из cookie oauth_token)"), v, 400, v)) return;
        cfg.oauth_token = util::trim(v);
        cfg.save();
        showToast(cfg.oauth_token.empty() ? tr("OAuth выключен") : tr("OAuth token сохранён"), 4);
    }

    void openProxy() {
        std::string v = cfg.proxy;
        if (!prompt(tr("Прокси (socks5://IP:PORT или http://IP:PORT)"), v, 200, v)) return;
        cfg.proxy = util::trim(v);
        cfg.save();
        showToast(cfg.proxy.empty() ? tr("Прокси выключен") : tr("Прокси: ") + cfg.proxy, 4);
    }

    void scanWholeSd() {
        if (job) { showToast(tr("Дождись текущей задачи")); return; }
        Job* j = new Job();
        j->kind = 3;
        job = j;
        startJob(j);
        showToast(tr("Сканирую всю SD-карту…"), 4);
    }

    void openMusicDir() {
        std::string v = cfg.music_dir;
        if (!prompt(tr("Папка с музыкой"), v, 200, v)) return;
        if (!v.empty() && v != cfg.music_dir) {
            cfg.music_dir = util::trim(v);
            util::ensureDir(cfg.music_dir);
            cfg.save();
            rescan();
        }
    }

    void removeDownload() {
        auto& list = cachedTracks;
        int idx = sel[SCR_DOWNLOADS] + scroll[SCR_DOWNLOADS];
        if (idx < 0 || idx >= (int)list.size()) return;
        Track t = list[idx];
        if (util::fileExists(t.path)) util::removeFile(t.path);
        list.erase(list.begin() + idx);
        library::saveCache(cachedTracks);
        refreshLiked();
        if (idx >= (int)list.size() && idx > 0) {
            sel[SCR_DOWNLOADS] = (int)list.size() - 1;
            if (scroll[SCR_DOWNLOADS] > sel[SCR_DOWNLOADS]) scroll[SCR_DOWNLOADS] = sel[SCR_DOWNLOADS];
        }
        showToast(tr("Удалено: ") + t.title);
    }

    // ---------- input ----------

    // Keeps sel/scroll consistent when the underlying list changes size while
    // the user is browsing (new downloads, rescans, liked list rebuilds).
    void clampView(int s) {
        int count = countForScreen(s);
        if (count <= 0) { sel[s] = 0; scroll[s] = 0; return; }
        if (sel[s] < 0) sel[s] = 0;
        if (sel[s] >= count) sel[s] = count - 1;
        int visible = std::max(1, visibleRows(s));
        int maxScroll = std::max(0, count - visible);
        if (scroll[s] > maxScroll) scroll[s] = maxScroll;
        if (scroll[s] < 0) scroll[s] = 0;
        if (sel[s] < scroll[s]) scroll[s] = sel[s];
        if (sel[s] >= scroll[s] + visible) scroll[s] = sel[s] - visible + 1;
    }

    void moveSelection(int delta) {
        if (screen == SCR_PLAYER) return;
        double now = nowSeconds();
        if (now - lastMoveAt < 0.035) return; // one physical press = one move
        lastMoveAt = now;
        if (screen == SCR_SETTINGS) {
            sel[SCR_SETTINGS] += delta;
            if (sel[SCR_SETTINGS] < 0) sel[SCR_SETTINGS] = 0;
            if (sel[SCR_SETTINGS] > 7) sel[SCR_SETTINGS] = 7;
            return;
        }
        int s = screen;
        int count = countForScreen(s);
        if (count == 0) return;
        sel[s] += delta;
        clampView(s);
    }

    bool inRect(int px, int py, int x, int y, int w, int h) {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    void seekFromTouch(int x) {
        double dur = player.duration() > 0 ? player.duration()
                                          : (hasCurrent ? current.duration : 0);
        if (dur <= 0 || seekTouchW <= 0) return;
        float f = (float)(x - seekTouchX) / (float)seekTouchW;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        player.seek(f * dur);
    }

    void handleTouch(int x, int y, bool down) {
        if (!down) { touchActive = false; touchSeeking = false; return; }
        int py = H - PLAYER_H;
        if (!touchActive) {
            touchActive = true;
            if (y >= py) {
                if (inRect(x, y, 454, py + 28, 40, 48)) { playNext(false); return; }
                if (inRect(x, y, 494, py + 26, 44, 52)) { player.togglePause(); return; }
                if (inRect(x, y, 544, py + 28, 40, 48)) { playNext(true); return; }
                if (inRect(x, y, 600, py + 18, 340, 60)) {
                    touchSeeking = true;
                    seekTouchX = 600;
                    seekTouchW = 340;
                    seekFromTouch(x);
                    lastTouchSeek = nowSeconds();
                    return;
                }
            } else if (screen == SCR_PLAYER) {
                int x0 = CONTENT_X + 388;
                int y0 = CONTENT_Y;
                if (inRect(x, y, x0 - 4, y0 + 330, 44, 56)) { playNext(false); return; }
                if (inRect(x, y, x0 + 52, y0 + 326, 52, 60)) { player.togglePause(); return; }
                if (inRect(x, y, x0 + 112, y0 + 330, 44, 56)) { playNext(true); return; }
                if (inRect(x, y, x0, y0 + 200, 552, 110)) {
                    touchSeeking = true;
                    seekTouchX = x0;
                    seekTouchW = 552;
                    seekFromTouch(x);
                    lastTouchSeek = nowSeconds();
                    return;
                }
            }
        } else if (touchSeeking) {
            double now = nowSeconds();
            if (now - lastTouchSeek > 0.15) {
                lastTouchSeek = now;
                seekFromTouch(x);
            }
        }
    }

    // The stick is handled with an explicit deadzone + edge latch instead of
    // libnx's pseudo-buttons (they can flicker near the threshold and cause
    // extra selection jumps while scrolling).
    void handleStick(int y) {
        const int dz = 14000;
        bool up = y > dz;
        bool dn = y < -dz;
        if (up && !stickUpLatch) moveSelection(-1);
        if (dn && !stickDownLatch) moveSelection(1);
        stickUpLatch = up;
        stickDownLatch = dn;
    }

    void handleInput(u64 down, u64 held) {
        // repeat for d-pad / left stick navigation
        const u64 navMask = HidNpadButton_Up | HidNpadButton_Down;
        if (held & navMask) {
            if (down & navMask) {
                heldButton = held & navMask;
                holdStart = lastRepeat = nowSeconds();
            } else if (heldButton && (held & heldButton) == heldButton) {
                double t = nowSeconds();
                if (t - holdStart > 0.5 && t - lastRepeat > 0.12) {
                    lastRepeat = t;
                    moveSelection((heldButton & HidNpadButton_Up) ? -1 : 1);
                }
            } else {
                heldButton = 0;
            }
        } else {
            heldButton = 0;
        }

        if (down & HidNpadButton_Up) moveSelection(-1);
        if (down & HidNpadButton_Down) moveSelection(1);

        // Left/right only seek in the full-screen player. Tabs are L/R shoulders
        // (ZL/ZR) so a stray d-pad press can never change the section.
        if (down & HidNpadButton_Left) player.seekRelative(-10);
        if (down & HidNpadButton_Right) player.seekRelative(+10);
        if (down & HidNpadButton_ZL) {
            if (screen == SCR_PLAYER) playNext(false);
            else switchTab(-1);
        }
        if (down & HidNpadButton_ZR) {
            if (screen == SCR_PLAYER) playNext(true);
            else switchTab(1);
        }
        if (down & HidNpadButton_L) {
            if (screen == SCR_PLAYLISTS) {
                loadMyPlaylists();
            } else {
                player.setVolume(player.volume() - 0.05f);
                showToast(util::fmt(tr("Громкость %d%%"), (int)(player.volume() * 100 + 0.5f)), 1);
            }
        }
        if (down & HidNpadButton_R) {
            player.setVolume(player.volume() + 0.05f);
            showToast(util::fmt(tr("Громкость %d%%"), (int)(player.volume() * 100 + 0.5f)), 1);
        }
        if (down & HidNpadButton_Plus) player.togglePause();

        if (down & HidNpadButton_B) {
            if (screen == SCR_PLAYER) screen = SCR_HOME;
            else if (screen == SCR_PLAYLIST) screen = SCR_PLAYLISTS;
            else if (screen == SCR_SETTINGS) screen = SCR_HOME;
        }

        if (down & HidNpadButton_X) {
            if (screen == SCR_DOWNLOADS) removeDownload();
            else if (screen == SCR_PLAYLISTS) openPlaylistUrl();
            else toggleLikeCurrent();
        }

        if (down & HidNpadButton_Minus) {
            if (screen == SCR_PLAYER) screen = SCR_HOME;
            else if (hasCurrent) screen = SCR_PLAYER;
        }

        if (down & HidNpadButton_Y) {
            if (screen == SCR_LIBRARY) toggleShuffle();
            else if (screen == SCR_SEARCH) doSearch();
            else if (screen == SCR_PLAYLISTS) doPlaylistSearch();
            else if (screen == SCR_PLAYLIST) downloadCurrentPlaylist();
            else toggleShuffle();
        }

        if (down & HidNpadButton_A) {
            if (screen == SCR_HOME) {
                if (!localTracks.empty()) { screen = SCR_LIBRARY; }
                else screen = SCR_SEARCH;
            } else if (screen == SCR_SEARCH) {
                if (!results.empty() && !searching) {
                    int idx = sel[SCR_SEARCH] + scroll[SCR_SEARCH];
                    if (idx >= 0 && idx < (int)results.size()) {
                        if (isCurrentTrack(results[idx])) player.togglePause();
                        else playTrack(results[idx], &results, idx);
                    }
                } else {
                    doSearch();
                }
            } else if (screen == SCR_PLAYLISTS) {
                int idx = sel[SCR_PLAYLISTS] + scroll[SCR_PLAYLISTS];
                if (!playlists.empty() && idx >= 0 && idx < (int)playlists.size()) openPlaylistAt(idx);
                else openPlaylistUrl();
            } else if (screen == SCR_PLAYLIST) {
                int idx = sel[SCR_PLAYLIST] + scroll[SCR_PLAYLIST];
                if (idx >= 0 && idx < (int)playlistTracks.size()) {
                    if (isCurrentTrack(playlistTracks[idx])) player.togglePause();
                    else playTrack(playlistTracks[idx], &playlistTracks, idx);
                }
            } else if (screen == SCR_LIBRARY || screen == SCR_LIKED || screen == SCR_DOWNLOADS) {
                auto& list = listForScreen(screen);
                int idx = sel[screen] + scroll[screen];
                if (idx >= 0 && idx < (int)list.size()) {
                    if (isCurrentTrack(list[idx])) player.togglePause();
                    else playTrack(list[idx], &list, idx);
                }
            } else if (screen == SCR_SETTINGS) {
                if (sel[SCR_SETTINGS] == 0) openSettingsClientId();
                else if (sel[SCR_SETTINGS] == 1) { g_english = !g_english; cfg.lang = g_english ? "en" : "ru"; cfg.save(); }
                else if (sel[SCR_SETTINGS] == 2) openMusicDir();
                else if (sel[SCR_SETTINGS] == 3) openProxy();
                else if (sel[SCR_SETTINGS] == 4) openOAuthToken();
                else if (sel[SCR_SETTINGS] == 5) { cfg.insecure_tls = !cfg.insecure_tls; cfg.save(); showToast(cfg.insecure_tls ? tr("TLS: без проверки") : tr("TLS: строгая проверка")); }
                else if (sel[SCR_SETTINGS] == 6) rescan();
                else if (sel[SCR_SETTINGS] == 7) scanWholeSd();
            } else if (screen == SCR_PLAYER) {
                player.togglePause();
            }
        }
    }

    // ---------- drawing ----------

    void drawSidebar() {
        gfx.fillRect(0, 0, SIDEBAR_W, H - PLAYER_H, pal::sidebar);
        gfx.fillRect(SIDEBAR_W - 1, 0, 1, H - PLAYER_H, pal::line);

        gfx.fillRect(24, 24, 44, 44, pal::accent);
        gfx.text(24 + 13, 24 + 14, "SC", pal::text, 2);
        gfx.text(82, 28, "switchcloud", pal::text, 2);
        gfx.text(84, 60, tr("офлайн-музыка"), pal::textMute, 1);

        const char* names[kNavCount] = {tr("Главная"), tr("Поиск"), tr("Плейлисты"), tr("Моя музыка"),
                                        tr("Мне нравится"), tr("Загрузки"), tr("Настройки")};
        int y = 118;
        for (int i = 0; i < kNavCount; i++) {
            bool active = screen == kNav[i]
                || (screen == SCR_PLAYER && kNav[i] == SCR_HOME)
                || (screen == SCR_PLAYLIST && kNav[i] == SCR_PLAYLISTS);
            if (active) {
                gfx.fillRect(14, y - 4, SIDEBAR_W - 28, 46, pal::cardHover);
                gfx.fillRect(14, y - 4, 4, 46, pal::accent);
            }
            u32 color = active ? pal::text : pal::textDim;
            gfx.text(34, y + 9, names[i], color, 2);
            y += 56;
        }

        int bottom = H - PLAYER_H - 54;
        if (netOnline) {
            gfx.fillRect(28, bottom + 6, 10, 10, pal::green);
            gfx.text(48, bottom, tr("Сеть подключена"), pal::textDim, 1);
        } else {
            gfx.fillRect(28, bottom + 6, 10, 10, pal::textMute);
            gfx.text(48, bottom, tr("Нет сети"), pal::textMute, 1);
        }
        gfx.text(28, bottom + 24, tr("ZL/ZR — разделы"), pal::textMute, 1);
        gfx.text(28, bottom + 42, "v0.1 · libnx · FFmpeg", pal::textMute, 1);
    }

    void drawTopbar() {
        gfx.fillRect(SIDEBAR_W, 0, W - SIDEBAR_W, TOPBAR_H, pal::bg);
        const char* title = (screen == SCR_PLAYLIST && !playlistTitle.empty())
                                ? playlistTitle.c_str() : screenTitle(screen);
        gfx.text(CONTENT_X, 18, title, pal::text, 2, CONTENT_W - 260);
        std::string right = sc::configured() ? tr("SoundCloud: ключ задан") : tr("SoundCloud: нет client_id");
        int tw = gfx.textWidth(right.c_str(), 1);
        gfx.text(W - 28 - tw, 24, right.c_str(), sc::configured() ? pal::textDim : pal::accent, 1);
        gfx.hLine(SIDEBAR_W, TOPBAR_H - 1, W - SIDEBAR_W, pal::line);
    }

    void drawRow(int x, int y, int w, const Track& t, bool selected, bool isPlaying) {
        int h = ROW_H - 8;
        if (selected) {
            gfx.fillRect(x, y, w, h, pal::cardHover);
            gfx.fillRect(x, y, 4, h, pal::accent);
        } else {
            gfx.fillRect(x, y, w, h, pal::card);
        }
        gfx.artwork(x + 12, y + 8, h - 16, t.cover.empty() ? t.title : t.cover);
        gfx.text(x + 72, y + 12, t.title.c_str(), isPlaying ? pal::accent : pal::text, 2, w - 200);
        std::string sub = t.artist;
        if (t.cloud) sub += t.cached ? tr("  ·  офлайн") : "  ·  SoundCloud";
        gfx.text(x + 72, y + 38, sub.c_str(), pal::textDim, 1, w - 220);
        if (t.liked) gfx.text(x + w - 150, y + 22, "♥", pal::accent, 1);
        if (t.duration > 0) {
            std::string d = util::formatTime(t.duration);
            gfx.text(x + w - 22 - gfx.textWidth(d.c_str(), 1), y + 24, d.c_str(), pal::textMute, 1);
        }
        if (t.uploadProgress >= 0) {
            int pw = (int)((w - 24) * t.uploadProgress);
            gfx.fillRect(x + 12, y + h - 6, w - 24, 4, pal::line);
            gfx.fillRect(x + 12, y + h - 6, pw, 4, pal::accent);
        }
    }

    void drawList(const std::vector<Track>& list, int s) {
        int x = CONTENT_X, y0 = CONTENT_Y + 44;
        if (list.empty()) {
            gfx.text(x, y0 + 20, emptyText(s), pal::textMute, 1, CONTENT_W);
            return;
        }
        int visible = std::max(1, visibleRows(s));
        int start = scroll[s];
        for (int i = 0; i < visible; i++) {
            int idx = start + i;
            if (idx >= (int)list.size()) break;
            Track t = list[idx];
            bool isPlaying = hasCurrent && current.id == t.id && player.loaded();
            drawRow(x, y0 + i * ROW_H, CONTENT_W, t, idx == sel[s], isPlaying);
        }
        if ((int)list.size() > visible) {
            std::string cnt = util::fmt(tr("%d–%d из %d"), start + 1,
                                        std::min((int)list.size(), start + visible), (int)list.size());
            gfx.text(x + CONTENT_W - gfx.textWidth(cnt.c_str(), 1), y0 + visible * ROW_H + 6, cnt.c_str(), pal::textMute, 1);
        }
    }

    void drawHome() {
        int x = CONTENT_X, y = CONTENT_Y;
        gfx.vGradient(x, y, CONTENT_W, 150, rgba(255, 85, 0, 255), rgba(120, 30, 90, 255));
        gfx.text(x + 28, y + 22, tr("Твоя музыка."), pal::text, 3);
        gfx.text(x + 28, y + 68, tr("Локальные треки + SoundCloud. Офлайн — всегда."), pal::text, 1);
        gfx.text(x + 28, y + 104, tr("A — играть   B — назад   X — ♥   - — полный экран"), rgba(255, 255, 255, 210), 1);

        int cy = y + 172;
        gfx.text(x, cy, tr("Недавнее"), pal::text, 2);
        cy += 38;
        std::vector<Track> recent = localTracks;
        for (auto& c : cachedTracks) recent.push_back(c);
        int shown = 0;
        for (auto& t : recent) {
            if (shown >= 3) break;
            drawRow(x, cy, CONTENT_W, t, false, hasCurrent && current.id == t.id);
            cy += ROW_H;
            shown++;
        }
        if (shown == 0) gfx.text(x, cy + 4, emptyText(SCR_LIBRARY), pal::textMute, 1, CONTENT_W);
    }

    void drawSearch() {
        int x = CONTENT_X, y = CONTENT_Y;
        int fieldH = 56;
        gfx.fillRect(x, y, CONTENT_W, fieldH, pal::card);
        gfx.frame(x, y, CONTENT_W, fieldH, pal::line);
        std::string q = query.empty() ? tr("Нажми A, чтобы ввести запрос…") : query;
        gfx.text(x + 18, y + 18, q.c_str(), query.empty() ? pal::textMute : pal::text, 2, CONTENT_W - 36);
        gfx.text(x + CONTENT_W - 200, y + 18,
                 results.empty() ? tr("A — ввести запрос") : tr("A — играть · Y — новый поиск"), pal::textMute, 1);

        int listY = y + fieldH + 24;
        if (!sc::configured()) {
            gfx.text(x, listY, tr("SoundCloud требует client_id."), pal::accent, 2);
            gfx.text(x, listY + 34, tr("Открой настройки — там можно ввести ключ и включить офлайн-кэш."), pal::textDim, 1, CONTENT_W);
            return;
        }
        if (searching) {
            gfx.spinner(x + CONTENT_W / 2, listY + 60, 18, spin, pal::accent);
            gfx.textCenter(x, listY + 90, CONTENT_W, tr("Ищу на SoundCloud…"), pal::textDim, 1);
            return;
        }
        if (results.empty()) {
            gfx.text(x, listY, tr("Результаты появятся здесь."), pal::textMute, 1);
            gfx.text(x, listY + 22, tr("Стриминговый API SoundCloud работает только с твоим client_id."), pal::textMute, 1, CONTENT_W);
            return;
        }
        gfx.text(x, listY - 22, util::fmt(tr("Найдено: %d"), (int)results.size()).c_str(), pal::textDim, 1);
        int visible = std::max(1, (CONTENT_H - 24) / ROW_H);
        for (int i = 0; i < visible; i++) {
            int idx = scroll[SCR_SEARCH] + i;
            if (idx >= (int)results.size()) break;
            Track& t = results[idx];
            bool isPlaying = hasCurrent && current.id == t.id && player.loaded();
            drawRow(x, listY + i * ROW_H, CONTENT_W, t, idx == sel[SCR_SEARCH], isPlaying);
        }
    }

    void drawPlaylistRow(int x, int y, int w, const sc::PlaylistInfo& p, bool selected) {
        int h = ROW_H + 8;
        if (selected) {
            gfx.fillRect(x, y, w, h, pal::cardHover);
            gfx.fillRect(x, y, 4, h, pal::accent);
        } else {
            gfx.fillRect(x, y, w, h, pal::card);
        }
        gfx.artwork(x + 12, y + 8, h - 16, p.artwork.empty() ? p.title : p.artwork);
        gfx.text(x + 76, y + 12, p.title.c_str(), selected ? pal::text : pal::text, 2, w - 220);
        std::string sub = p.user + util::fmt(tr("  ·  %d треков"), p.trackCount);
        gfx.text(x + 76, y + 42, sub.c_str(), pal::textDim, 1, w - 220);
    }

    void drawPlaylists() {
        int x = CONTENT_X, y = CONTENT_Y;
        int fieldH = 56;
        gfx.fillRect(x, y, CONTENT_W, fieldH, pal::card);
        gfx.frame(x, y, CONTENT_W, fieldH, pal::line);
        std::string q = playlistQuery.empty() ? tr("Y — поиск плейлистов, X — ссылка на аккаунт") : playlistQuery;
        gfx.text(x + 18, y + 18, q.c_str(), playlistQuery.empty() ? pal::textMute : pal::text, 2, CONTENT_W - 36);
        if (job && (job->kind == 4 || job->kind == 6)) {
            gfx.text(x + CONTENT_W - 120, y + 18, tr("ищу…"), pal::textDim, 1);
        }

        int listY = y + fieldH + 24;
        if (playlists.empty()) {
            gfx.text(x, listY, emptyText(SCR_PLAYLISTS), pal::textMute, 1, CONTENT_W);
            gfx.text(x, listY + 26, tr("Пример: https://soundcloud.com/artist/sets/playlist"), pal::textMute, 1, CONTENT_W);
            return;
        }
        int visible = std::max(1, (CONTENT_H - 80) / (ROW_H + 8));
        for (int i = 0; i < visible; i++) {
            int idx = scroll[SCR_PLAYLISTS] + i;
            if (idx >= (int)playlists.size()) break;
            drawPlaylistRow(x, listY + i * (ROW_H + 8), CONTENT_W, playlists[idx],
                            idx == sel[SCR_PLAYLISTS]);
        }
        gfx.text(x, y + CONTENT_H - 8, tr("A — открыть   X — ссылка   L — мои плейлисты   ZL/ZR — разделы"),
                 pal::textMute, 1);
    }

    void drawPlaylistDetail() {
        int x = CONTENT_X, y = CONTENT_Y;
        gfx.text(x, y, playlistTitle.empty() ? tr("Плейлист") : playlistTitle.c_str(), pal::text, 2, CONTENT_W);
        std::string sub = util::fmt(tr("%d треков"), (int)playlistTracks.size());
        if (!playlistUser.empty()) sub = playlistUser + "  ·  " + sub;
        gfx.text(x, y + 32, sub.c_str(), pal::textDim, 1);
        gfx.text(x + CONTENT_W - 300, y + 4, tr("Y — скачать весь плейлист"), pal::accent, 1);
        gfx.text(x + CONTENT_W - 300, y + 22, tr("A — играть трек, B — назад"), pal::textMute, 1);

        int listY = y + 64;
        gfx.setClip(x, listY, CONTENT_W, CONTENT_H - 64);
        int visible = std::max(1, (CONTENT_H - 90) / ROW_H);
        for (int i = 0; i < visible; i++) {
            int idx = scroll[SCR_PLAYLIST] + i;
            if (idx >= (int)playlistTracks.size()) break;
            Track& t = playlistTracks[idx];
            bool isPlaying = hasCurrent && current.id == t.id && player.loaded();
            drawRow(x, listY + i * ROW_H, CONTENT_W, t, idx == sel[SCR_PLAYLIST], isPlaying);
        }
        if (playlistTracks.empty()) gfx.text(x, listY + 8, emptyText(SCR_PLAYLIST), pal::textMute, 1, CONTENT_W);
        gfx.resetClip();
    }

    void drawSettings() {
        int x = CONTENT_X, y = CONTENT_Y;
        struct Item { std::string label; std::string value; std::string hint; };
        std::string cid = tr("не задан");
        if (!cfg.client_id.empty()) {
            if (cfg.client_id.size() <= 10) cid = cfg.client_id;
            else cid = cfg.client_id.substr(0, 8) + "…" + cfg.client_id.substr(cfg.client_id.size() - 4);
        }
        std::string oauth = tr("нет");
        if (!cfg.oauth_token.empty()) {
            oauth = cfg.oauth_token.size() > 10
                ? cfg.oauth_token.substr(0, 6) + "…" + cfg.oauth_token.substr(cfg.oauth_token.size() - 4)
                : tr("задан");
        }
        std::vector<Item> items = {
            {"client_id SoundCloud", cid, tr("A — изменить")},
            {tr("Язык / Language"), g_english ? "English" : "Русский", tr("A — переключить")},
            {tr("Папка музыки"), cfg.music_dir, tr("A — изменить")},
            {tr("Прокси / обход"), cfg.proxy.empty() ? tr("выключен") : cfg.proxy, "A — socks5://IP:PORT"},
            {tr("OAuth token (аккаунт)"), oauth, tr("A — вставить токен")},
            {tr("TLS без проверки"), cfg.insecure_tls ? tr("включено") : tr("выключено"), tr("A — переключить")},
            {tr("Пересканировать библиотеку"), util::fmt(tr("%d треков"), (int)localTracks.size()), tr("A — сканировать")},
            {tr("Сканировать всю SD"), tr("ищет mp3/flac/ogg везде"), tr("A — сканировать")},
        };
        for (size_t i = 0; i < items.size(); i++) {
            int ry = y + (int)i * 56;
            bool active = sel[SCR_SETTINGS] == (int)i;
            gfx.fillRect(x, ry, CONTENT_W, 48, active ? pal::cardHover : pal::card);
            if (active) gfx.fillRect(x, ry, 4, 48, pal::accent);
            gfx.text(x + 20, ry + 6, items[i].label.c_str(), pal::text, 2);
            gfx.text(x + 20, ry + 32, items[i].hint.c_str(), pal::textMute, 1);
            int vw = gfx.textWidth(items[i].value.c_str(), 1);
            gfx.text(x + CONTENT_W - 20 - vw, ry + 18, items[i].value.c_str(),
                     i == 0 && !sc::configured() ? pal::accent : pal::textDim, 1);
        }
        int hy = y + 8 * 56 + 6;
        gfx.text(x, hy, "Client ID:", pal::accent, 1);
        gfx.text(x, hy + 18, tr("soundcloud.com → F12 → Network → скопируй client_id из запроса к api-v2."), pal::textDim, 1);
        gfx.text(x, hy + 34, tr("OAuth: войди на soundcloud.com → F12 → Cookies → oauth_token (для своих плейлистов)."), pal::textDim, 1);
        std::string tls = net::isTlsSecure() ? tr("TLS: проверка сертификата включена") : tr("TLS: проверка отключена — трафик не защищён от MITM");
        gfx.text(x, hy + 52, tls.c_str(), net::isTlsSecure() ? pal::green : pal::red, 1);
    }

    void drawDownloads() {
        int x = CONTENT_X, y = CONTENT_Y;
        uint64_t total = 0;
        for (auto& t : cachedTracks) total += util::fileSize(t.path);
        gfx.text(x, y, util::fmt(tr("Офлайн-кэш: %d треков · %.1f МБ"), (int)cachedTracks.size(),
                                 (double)total / (1024.0 * 1024.0)).c_str(), pal::textDim, 1);
        gfx.text(x + CONTENT_W - 200, y, tr("X — удалить трек"), pal::textMute, 1);
        gfx.setClip(CONTENT_X, CONTENT_Y + 16, CONTENT_W, CONTENT_H - 16);
        // reuse list drawing but shifted
        int savedY = CONTENT_Y;
        (void)savedY;
        int visible = std::max(1, (CONTENT_H - 24) / ROW_H);
        for (int i = 0; i < visible; i++) {
            int idx = scroll[SCR_DOWNLOADS] + i;
            if (idx >= (int)cachedTracks.size()) break;
            Track& t = cachedTracks[idx];
            bool isPlaying = hasCurrent && current.id == t.id && player.loaded();
            drawRow(x, y + 34 + i * ROW_H, CONTENT_W, t, idx == sel[SCR_DOWNLOADS], isPlaying);
        }
        if (cachedTracks.empty()) gfx.text(x, y + 50, emptyText(SCR_DOWNLOADS), pal::textMute, 1, CONTENT_W);
        gfx.resetClip();
    }

    void drawPlayerBar() {
        int y = H - PLAYER_H;
        gfx.fillRect(0, y, W, PLAYER_H, pal::sidebar);
        gfx.hLine(0, y, W, pal::line);

        bool have = hasCurrent;
        if (have) gfx.artwork(20, y + 18, PLAYER_H - 36, current.cover);

        std::string title = have ? current.title : tr("Ничего не играет");
        std::string artist = have ? current.artist : tr("Выбери трек в библиотеке или поиске");
        gfx.text(20 + PLAYER_H - 20, y + 24, title.c_str(), pal::text, 2, 340);
        gfx.text(20 + PLAYER_H - 20, y + 54, artist.c_str(), pal::textDim, 1, 340);

        double dur = player.duration() > 0 ? player.duration() : (have ? current.duration : 0);
        double pos = player.position();

        // touch/click controls: prev, play/pause, next
        int iconY = y + 36;
        gfx.iconPrev(460, iconY, 20, have ? pal::text : pal::textMute);
        if (have && player.playing()) gfx.iconPause(504, iconY, 22, pal::text);
        else gfx.iconPlay(504, iconY, 22, have ? pal::text : pal::textMute);
        gfx.iconNext(548, iconY, 20, have ? pal::text : pal::textMute);

        int barX = 600, barW = 340, barY = y + 46;
        float prog = dur > 0 ? (float)(pos / dur) : 0.0f;
        if (prog < 0) prog = 0;
        if (prog > 1) prog = 1;
        gfx.wave(barX, barY - 22, barW, 44, have ? current.id : "idle", prog, pal::accent, rgba(255, 255, 255, 40));
        std::string pt = util::formatTime((int)pos), dt = util::formatTime((int)dur);
        gfx.text(barX, barY + 30, pt.c_str(), pal::textDim, 1);
        gfx.text(barX + barW - gfx.textWidth(dt.c_str(), 1), barY + 30, dt.c_str(), pal::textDim, 1);

        if (!audioOk) gfx.text(barX, y + 72, tr("аудио: applet mode? запусти через title override"), pal::red, 1, 360);

        // volume
        int vx = 1000, vy = y + 54, vw = 150;
        gfx.text(vx - 44, y + 44, "VOL", pal::textMute, 1);
        gfx.fillRect(vx, vy, vw, 6, pal::line);
        gfx.fillRect(vx, vy, (int)(vw * player.volume()), 6, pal::accent);
        gfx.text(vx + vw + 12, y + 44, util::fmt("%d", (int)(player.volume() * 100)).c_str(), pal::textMute, 1);
        if (shuffle) gfx.text(vx, y + 24, "SHUF", pal::accent, 1);

        std::string hint = tr("A ок   X ♥   Y микш   ZL/ZR ");
        hint += (screen == SCR_PLAYER) ? tr("трек") : tr("разделы");
        hint += tr("   L/R громкость   − экран");
        gfx.text(20, y + PLAYER_H - 22, hint.c_str(), pal::textMute, 1);
    }

    void drawFullPlayer() {
        int x = CONTENT_X, y = CONTENT_Y;
        gfx.fillRect(x, y, CONTENT_W, CONTENT_H, pal::card);
        int art = 300;
        gfx.artwork(x + 40, y + 60, art, hasCurrent ? current.cover : "idle");
        int tx = x + 40 + art + 48;
        gfx.text(tx, y + 70, hasCurrent ? current.title.c_str() : tr("Ничего не играет"), pal::text, 3, CONTENT_W - (tx - x) - 40);
        gfx.text(tx, y + 132, hasCurrent ? current.artist.c_str() : "", pal::textDim, 2, CONTENT_W - (tx - x) - 40);
        if (hasCurrent && current.cloud)
            gfx.text(tx, y + 168, current.cached ? tr("офлайн-копия") : "SoundCloud", pal::textMute, 1);

        double dur = player.duration() > 0 ? player.duration() : (hasCurrent ? current.duration : 0);
        double pos = player.position();
        float prog = dur > 0 ? (float)(pos / dur) : 0.0f;
        gfx.wave(tx, y + 214, CONTENT_W - (tx - x) - 40, 88, hasCurrent ? current.id : "idle", prog,
                 pal::accent, rgba(255, 255, 255, 40));
        std::string pt = util::formatTime((int)pos) + " / " + util::formatTime((int)dur);
        gfx.text(tx, y + 312, pt.c_str(), pal::textDim, 1);
        if (hasCurrent && current.liked) gfx.text(tx + CONTENT_W - (tx - x) - 52, y + 312, "♥", pal::accent, 1);

        gfx.iconPrev(tx, y + 342, 26, hasCurrent ? pal::text : pal::textMute);
        if (hasCurrent && player.playing()) gfx.iconPause(tx + 58, y + 338, 30, pal::text);
        else gfx.iconPlay(tx + 58, y + 338, 30, hasCurrent ? pal::text : pal::textMute);
        gfx.iconNext(tx + 120, y + 342, 26, hasCurrent ? pal::text : pal::textMute);

        gfx.text(x + 40, y + 430, tr("Очередь"), pal::text, 2);
        int qx = x + 40, qy = y + 466;
        int shown = 0;
        for (int i = std::max(0, queueIndex); i < (int)queue.size() && shown < 4; i++, shown++) {
            Track& t = queue[i];
            bool act = i == queueIndex;
            std::string row = (act ? std::string("▶ ") : std::string("   ")) + t.title + " — " + t.artist;
            gfx.text(qx, qy + shown * 22, row.c_str(),
                     act ? pal::accent : pal::textDim, 1, CONTENT_W - 80);
        }
        if (queue.empty()) gfx.text(qx, qy, tr("Пусто"), pal::textMute, 1);
    }

    void drawToast() {
        if (toast.empty() || nowSeconds() > toastUntil) return;
        int tw = std::min(CONTENT_W, gfx.textWidth(toast.c_str(), 1) + 36);
        int x = CONTENT_X + (CONTENT_W - tw) / 2;
        int y = H - PLAYER_H - 56;
        gfx.fillRect(x, y, tw, 36, rgba(20, 20, 20, 240));
        gfx.frame(x, y, tw, 36, pal::line);
        gfx.text(x + 18, y + 10, toast.c_str(), pal::text, 1, tw - 36);
    }

    void draw() {
        if (screen != SCR_PLAYER) clampView(screen);
        gfx.clear(pal::bg);
        drawSidebar();
        drawTopbar();

        if (screen == SCR_HOME) drawHome();
        else if (screen == SCR_SEARCH) drawSearch();
        else if (screen == SCR_PLAYLISTS) drawPlaylists();
        else if (screen == SCR_PLAYLIST) drawPlaylistDetail();
        else if (screen == SCR_SETTINGS) drawSettings();
        else if (screen == SCR_DOWNLOADS) drawDownloads();
        else if (screen == SCR_PLAYER) drawFullPlayer();
        else {
            const std::vector<Track>& list = (screen == SCR_LIKED) ? likedList : localTracks;
            gfx.text(CONTENT_X, CONTENT_Y,
                     screen == SCR_LIBRARY
                         ? util::fmt(tr("Локальные файлы: %d"), (int)localTracks.size()).c_str()
                         : util::fmt(tr("Понравилось: %d"), (int)likedList.size()).c_str(),
                     pal::textDim, 1);
            gfx.setClip(CONTENT_X, CONTENT_Y + 16, CONTENT_W, CONTENT_H - 16);
            int visible = std::max(1, (CONTENT_H - 24) / ROW_H);
            for (int i = 0; i < visible; i++) {
                int idx = scroll[screen] + i;
                if (idx >= (int)list.size()) break;
                const Track& t = list[idx];
                bool isPlaying = hasCurrent && current.id == t.id && player.loaded();
                drawRow(CONTENT_X, CONTENT_Y + 24 + i * ROW_H, CONTENT_W, t, idx == sel[screen], isPlaying);
            }
            if (list.empty()) gfx.text(CONTENT_X, CONTENT_Y + 60, emptyText(screen), pal::textMute, 1, CONTENT_W);
            gfx.resetClip();
        }

        if (job && (job->kind == 2 || job->kind == 7)) {
            float p = job->progress.load();
            std::string txt = job->kind == 7 ? tr("Плейлист: ") : tr("Загрузка: ");
            txt += p >= 0 ? util::fmt("%d%%", (int)(p * 100)) : "…";
            gfx.textCenter(CONTENT_X, CONTENT_Y + CONTENT_H - 26, CONTENT_W, txt.c_str(), pal::textDim, 1);
        }
        if (!audioOk) {
            int by = CONTENT_Y;
            gfx.fillRect(CONTENT_X, by, CONTENT_W, 34, rgba(170, 24, 24, 235));
            gfx.text(CONTENT_X + 12, by + 9,
                     tr("НЕТ АУДИО: hbmenu запущен в Applet Mode — перезапусти через title override (R + игра)"),
                     pal::text, 1, CONTENT_W - 24);
        }
        drawPlayerBar();
        drawToast();
    }
};

std::unique_ptr<App> g_app;

} // namespace

int main(int argc, char** argv) {
    fsdevMountSdmc();

    util::ensureDir("sdmc:/switch/scswitch");
    util::ensureDir("sdmc:/switch/scswitch/music");
    cfg.load();
    g_english = util::lower(cfg.lang) == "en";
    util::ensureDir(cfg.cache_dir);
    util::ensureDir(cfg.music_dir);
    loadLikes();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    if (!gfx.init()) {
        util::log("fatal: framebuffer init failed");
        return 1;
    }

    g_app = std::make_unique<App>();
    util::log("scswitch build %s %s", __DATE__, __TIME__);
    util::log("applet: type=%d aruid=%llu", (int)appletGetAppletType(),
              (unsigned long long)appletGetAppletResourceUserId());
    g_app->audioOk = g_app->player.init();
    if (!g_app->audioOk)
        g_app->showToast(tr("Аудио недоступно: запусти hbmenu через title override (удерживай R на игре)"), 9);
    net::init();
    g_app->cachedTracks.clear();
    library::loadCache(g_app->cachedTracks);
    for (auto& t : g_app->cachedTracks) t.liked = isLiked(t.id);
    g_app->rescan();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);

        double now = nowSeconds();
        if (now - g_app->netCheckAt > 2.0) {
            g_app->netCheckAt = now;
            g_app->netOnline = net::online();
        }
        g_app->reapJob();
        if (g_app->player.consumeEnded()) g_app->playNext(true);
        g_app->spin += 0.09f;

        g_app->handleInput(down, held);
        g_app->handleStick(padGetStickPos(&pad, 0).y);
        HidTouchScreenState touchState{};
        hidGetTouchScreenStates(&touchState, 1);
        bool touchDown = touchState.count > 0;
        g_app->handleTouch(touchDown ? (int)touchState.touches[0].x : -1,
                           touchDown ? (int)touchState.touches[0].y : -1, touchDown);

        gfx.begin();
        g_app->draw();
        gfx.end();

        svcSleepThread(16000000ULL);
    }

    g_app->player.shutdown();
    gfx.shutdown();
    // A download may still be running; don't free mbedTLS under it.
    if (!g_app->job) net::shutdown();
    fsdevUnmountAll();
    return 0;
}
