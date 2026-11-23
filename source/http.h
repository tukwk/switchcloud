#pragma once
#include <string>
#include <stdint.h>

namespace net {

bool init();
void shutdown();
bool online();

// Plain callbacks (no std::function: keeps the binary free of RTTI bloat).
struct Progress {
    void (*fn)(void* user, uint64_t done, uint64_t total) = nullptr;
    void* user = nullptr;
    void call(uint64_t done, uint64_t total) const {
        if (fn) fn(user, done, total);
    }
};

struct Response {
    int status = 0;
    std::string body;
    std::string error;
};

// Blocking HTTPS GET. Follows redirects. Returns body in response.
Response get(const std::string& url, const std::string& extraHeaders = "", int timeoutMs = 20000);

// Blocking HTTPS GET into a file.
bool download(const std::string& url, const std::string& destPath,
              Progress progress, std::string& error, const std::string& extraHeaders = "",
              int timeoutMs = 20000);

bool isTlsSecure();

} // namespace net
