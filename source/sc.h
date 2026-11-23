#pragma once
#include <string>
#include <vector>

namespace sc {

struct PlaylistInfo {
    std::string id;
    std::string title;
    std::string user;
    std::string artwork;
    std::string permalink;
    int trackCount = 0;
};

struct TrackInfo {
    std::string id;
    std::string title;
    std::string user;
    std::string permalink;
    std::string artwork;
    int durationMs = 0;
    bool streamable = true;
};

bool configured();
// Returns false and fills err on failure.
bool search(const std::string& query, std::vector<TrackInfo>& out, std::string& err, int limit = 30);
bool resolveProgressiveUrl(const std::string& trackId, std::string& url, std::string& err);
bool resolveHlsUrl(const std::string& trackId, std::string& url, std::string& err);
bool searchPlaylists(const std::string& query, std::vector<PlaylistInfo>& out, std::string& err, int limit = 30);
// Accepts a playlist URL or a profile URL; a profile returns all its public playlists.
bool resolvePlaylistUrl(const std::string& url, std::vector<PlaylistInfo>& out, std::string& err);
bool getPlaylistTracks(const std::string& playlistId, std::vector<TrackInfo>& out,
                       std::string& title, std::string& err);
bool myPlaylists(std::vector<PlaylistInfo>& out, std::string& err);
// Extra header (Authorization: OAuth ...) for media requests when logged in.
std::string authHeader();
std::string cachePath(const std::string& trackId);
std::string hlsCachePath(const std::string& trackId);
// Returns the existing cached file (mp3 or m4a), or an empty string.
std::string findCached(const std::string& trackId);

} // namespace sc
