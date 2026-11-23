#pragma once
#include "core.h"
#include <vector>
#include <string>

namespace library {

void scanLocal(const std::string& root, std::vector<Track>& out, int depth = 0);
// Scans the whole SD card, skipping system/emulator data. Useful when the
// user keeps music somewhere other than the default folders.
void scanWholeSd(std::vector<Track>& out);
void loadCache(std::vector<Track>& out);
void saveCache(const std::vector<Track>& tracks);
std::string idForPath(const std::string& path);

// Reads basic tags; falls back to "Artist - Title" from the filename.
void fillMetadata(Track& track);

} // namespace library
