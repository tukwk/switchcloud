#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <switch.h>
#include <stdint.h>

// The Switch libstdc++ has no <mutex>; this tiny spinlock is enough for our
// short critical sections.
class SpinLock {
public:
    void lock() {
        while (m_flag.test_and_set(std::memory_order_acquire)) svcSleepThread(1000);
    }
    void unlock() { m_flag.clear(std::memory_order_release); }
private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

template <typename L>
struct ScopedLock {
    explicit ScopedLock(L& l) : m_l(l) { m_l.lock(); }
    ~ScopedLock() { m_l.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
private:
    L& m_l;
};

struct Config {
    std::string client_id;
    std::string music_dir = "sdmc:/switch/scswitch/music";
    std::string cache_dir = "sdmc:/switch/scswitch/cache";
    std::string ca_path   = "sdmc:/switch/scswitch/cacert.pem";
    std::string proxy;     // e.g. socks5://192.168.1.10:10808
    std::string oauth_token; // optional SoundCloud account token (private playlists)
    std::string lang = "en";
    bool insecure_tls     = false;

    bool load();
    bool save() const;
};

extern Config cfg;

struct Track {
    std::string id;         // local: path, cloud: "sc:<numeric id>"
    std::string title;
    std::string artist;
    std::string album;
    std::string path;       // playable local file (cache for cloud tracks)
    std::string sc_id;      // SoundCloud numeric id
    std::string permalink;
    std::string cover;      // artwork seed
    int duration = 0;       // seconds
    bool cloud = false;
    bool cached = false;
    bool liked = false;
    float uploadProgress = -1.0f; // 0..1 while downloading, -1 otherwise
};

namespace util {
std::string readFile(const std::string& path);
bool writeFile(const std::string& path, const std::string& data);
bool ensureDir(const std::string& path);   // recursive, sdmc:/ aware
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
uint64_t fileSize(const std::string& path);
bool removeFile(const std::string& path);
std::string basename(const std::string& p);
std::string dirname(const std::string& p);
std::string joinPath(const std::string& a, const std::string& b);
std::string lower(std::string s);
std::string trim(const std::string& s);
bool endsWith(const std::string& s, const std::string& suffix);
std::string formatTime(int seconds);
std::string urlEncode(const std::string& s);
std::string fmt(const char* f, ...);
void log(const char* fmt, ...);
std::vector<std::string> listDir(const std::string& path);
uint32_t hashStr(const std::string& s);
}

extern std::vector<std::string> g_likedIds;
bool isLiked(const std::string& id);
void setLiked(const std::string& id, bool liked);
void saveLikes();
void loadLikes();
