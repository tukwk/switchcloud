#pragma once
#include <string>
#include "http.h"

namespace hls {

// Downloads an HLS (fMP4) playlist into a single .m4a file: EXT-X-MAP init
// segment followed by every media segment in order. This produces a fragmented
// MP4 that libavformat's MOV demuxer can open.
bool download(const std::string& m3u8Url, const std::string& destPath,
              net::Progress progress, std::string& error,
              const std::string& extraHeaders = "");

} // namespace hls
