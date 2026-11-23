#include "http.h"
#include "i18n.h"
#include "core.h"
#include "ca_bundle.h"

#include <switch.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace net {

namespace {

mbedtls_x509_crt g_ca;
mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_drbg;
bool g_initialized = false;
bool g_caLoaded = false;
bool g_secure = false;

struct Url {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

bool parseUrl(const std::string& url, Url& out) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = url.substr(0, schemeEnd);
    if (out.scheme != "https" && out.scheme != "http") return false;
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string hostPort = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        out.host = hostPort.substr(0, colon);
        out.port = hostPort.substr(colon + 1);
    } else {
        out.host = hostPort;
        out.port = out.scheme == "https" ? "443" : "80";
    }
    return !out.host.empty();
}

void tlsError(const char* what, int ret) {
    char buf[256] = {0};
    mbedtls_strerror(ret, buf, sizeof(buf));
    util::log("net: %s failed: -0x%04X %s", what, (unsigned)-ret, buf);
}

struct Buffered {
    mbedtls_ssl_context* ssl = nullptr;
    char buf[8192];
    size_t pos = 0, len = 0;
    bool eof = false;

    int fill() {
        if (pos < len) return 1;
        int ret = mbedtls_ssl_read(ssl, (unsigned char*)buf, sizeof(buf));
        if (ret > 0) { pos = 0; len = (size_t)ret; return 1; }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { eof = true; return 0; }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 1;
        return -1;
    }

    // returns 1 = line read, 0 = eof, -1 = error
    int getLine(std::string& out) {
        out.clear();
        while (true) {
            if (!fill()) return out.empty() ? 0 : 1;
            while (pos < len) {
                char c = buf[pos++];
                if (c == '\n') {
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    return 1;
                }
                out += c;
                if (out.size() > 32768) return -1;
            }
        }
    }

    int readExact(char* dst, size_t n) {
        while (n > 0) {
            if (!fill()) return 0;
            size_t take = std::min(n, len - pos);
            memcpy(dst, buf + pos, take);
            pos += take;
            dst += take;
            n -= take;
        }
        return 1;
    }
};

typedef bool (*Sink)(void* user, const char* data, size_t len);
typedef void (*TotalFn)(void* user, uint64_t total);

struct RequestResult {
    int status = 0;
    std::string location;
    std::string error;
    uint64_t contentLength = 0;
    bool chunked = false;
};


struct ProxyConfig {
    bool on = false;
    bool socks5 = false;
    std::string host;
    std::string port;
    std::string user;
    std::string pass;
};

static bool parseProxyUrl(const std::string& value, ProxyConfig& p) {
    if (value.empty()) return false;
    size_t sep = value.find("://");
    if (sep == std::string::npos) return false;
    std::string scheme = util::lower(value.substr(0, sep));
    p.socks5 = (scheme == "socks5" || scheme == "socks5h" || scheme == "socks");
    if (!p.socks5 && scheme != "http" && scheme != "https") return false;
    std::string rest = value.substr(sep + 3);
    size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        std::string cred = rest.substr(0, at);
        rest = rest.substr(at + 1);
        size_t colon = cred.find(':');
        if (colon == std::string::npos) p.user = cred;
        else { p.user = cred.substr(0, colon); p.pass = cred.substr(colon + 1); }
    }
    size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    size_t colon = rest.rfind(':');
    if (colon == std::string::npos) { p.host = rest; p.port = p.socks5 ? "1080" : "8080"; }
    else { p.host = rest.substr(0, colon); p.port = rest.substr(colon + 1); }
    p.on = !p.host.empty() && !p.port.empty();
    return p.on;
}

static std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static bool rawSend(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int n = (int)send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool rawRecv(int fd, void* data, size_t len) {
    char* p = (char*)data;
    while (len > 0) {
        int n = (int)recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool readHttpHeaders(int fd, std::string& out) {
    out.clear();
    char c;
    while (out.size() < 8192) {
        if (!rawRecv(fd, &c, 1)) return false;
        out += c;
        if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) return true;
    }
    return false;
}

// Opens a tunnel to target host/port through the configured proxy.
static bool proxyHandshake(int fd, const ProxyConfig& p, const std::string& targetHost, const std::string& targetPort) {
    if (p.socks5) {
        uint8_t greet[4];
        size_t greetLen = 0;
        bool auth = !p.user.empty();
        greet[greetLen++] = 5;
        greet[greetLen++] = auth ? 2 : 1;
        greet[greetLen++] = 0;
        if (auth) greet[greetLen++] = 2;
        if (!rawSend(fd, greet, greetLen)) return false;
        uint8_t resp[2];
        if (!rawRecv(fd, resp, 2) || resp[0] != 5 || resp[1] == 0xFF) return false;
        if (resp[1] == 2) {
            if (p.user.size() > 255 || p.pass.size() > 255) return false;
            std::string authReq;
            authReq += (char)1;
            authReq += (char)p.user.size(); authReq += p.user;
            authReq += (char)p.pass.size(); authReq += p.pass;
            if (!rawSend(fd, authReq.data(), authReq.size())) return false;
            uint8_t ar[2];
            if (!rawRecv(fd, ar, 2) || ar[1] != 0) return false;
        }
        unsigned portNum = (unsigned)atoi(targetPort.c_str());
        std::string req;
        req += (char)5; req += (char)1; req += (char)0; req += (char)3;
        req += (char)targetHost.size(); req += targetHost;
        req += (char)((portNum >> 8) & 0xFF);
        req += (char)(portNum & 0xFF);
        if (!rawSend(fd, req.data(), req.size())) return false;
        uint8_t head[4];
        if (!rawRecv(fd, head, 4) || head[0] != 5 || head[1] != 0) return false;
        size_t skip = 0;
        if (head[3] == 1) skip = 4;
        else if (head[3] == 4) skip = 16;
        else if (head[3] == 3) {
            uint8_t l = 0;
            if (!rawRecv(fd, &l, 1)) return false;
            skip = l;
        } else return false;
        char tmp[256 + 2];
        if (skip + 2 > sizeof(tmp)) return false;
        return rawRecv(fd, tmp, skip + 2);
    }

    std::string req = "CONNECT " + targetHost + ":" + targetPort + " HTTP/1.1\r\n";
    req += "Host: " + targetHost + ":" + targetPort + "\r\n";
    if (!p.user.empty()) req += "Proxy-Authorization: Basic " + base64(p.user + ":" + p.pass) + "\r\n";
    req += "Proxy-Connection: Keep-Alive\r\n\r\n";
    if (!rawSend(fd, req.data(), req.size())) return false;
    std::string headers;
    if (!readHttpHeaders(fd, headers)) return false;
    size_t sp = headers.find(' ');
    if (sp == std::string::npos) return false;
    int code = atoi(headers.c_str() + sp + 1);
    return code == 200;
}

RequestResult perform(const Url& url, const std::string& extraHeaders, Sink sink, void* sinkUser,
                      TotalFn onTotal = nullptr, void* totalUser = nullptr, int timeoutMs = 20000) {
    RequestResult res;
    mbedtls_net_context sctx;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_net_init(&sctx);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    auto cleanup = [&] {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_session_reset(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&sctx);
    };

    int ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret) { tlsError("config", ret); res.error = "tls config"; cleanup(); return res; }

    int authmode = MBEDTLS_SSL_VERIFY_NONE;
    if (g_caLoaded && !cfg.insecure_tls) authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
    mbedtls_ssl_conf_authmode(&conf, authmode);
    if (g_caLoaded) mbedtls_ssl_conf_ca_chain(&conf, &g_ca, nullptr);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_read_timeout(&conf, (uint32_t)timeoutMs);

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret) { tlsError("setup", ret); res.error = "tls setup"; cleanup(); return res; }
    mbedtls_ssl_set_hostname(&ssl, url.host.c_str());

    ProxyConfig proxy;
    bool useProxy = parseProxyUrl(cfg.proxy, proxy);
    const char* connectHost = useProxy ? proxy.host.c_str() : url.host.c_str();
    const char* connectPort = useProxy ? proxy.port.c_str() : url.port.c_str();
    if (useProxy) util::log("net: via %s proxy %s:%s", proxy.socks5 ? "socks5" : "http",
                            proxy.host.c_str(), proxy.port.c_str());

    ret = mbedtls_net_connect(&sctx, connectHost, connectPort, MBEDTLS_NET_PROTO_TCP);
    if (ret && useProxy) {
        // The proxy may be on another LAN/offline. Don't lose search entirely:
        // retry this request directly once.
        util::log("net: proxy %s:%s unreachable (%d), trying direct", connectHost, connectPort, ret);
        mbedtls_net_free(&sctx);
        mbedtls_net_init(&sctx);
        useProxy = false;
        connectHost = url.host.c_str();
        connectPort = url.port.c_str();
        ret = mbedtls_net_connect(&sctx, connectHost, connectPort, MBEDTLS_NET_PROTO_TCP);
    }
    if (ret) {
        util::log("net: connect %s:%s failed (%d)", connectHost, connectPort, ret);
        res.error = useProxy ? "proxy connect" : "connect";
        cleanup();
        return res;
    }
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(sctx.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sctx.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (useProxy && !proxyHandshake(sctx.fd, proxy, url.host, url.port)) {
        util::log("net: proxy handshake to %s:%s failed", url.host.c_str(), url.port.c_str());
        res.error = "proxy handshake";
        cleanup();
        return res;
    }

    mbedtls_ssl_set_bio(&ssl, &sctx, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tlsError("handshake", ret);
            res.error = "tls handshake";
            cleanup();
            return res;
        }
    }

    std::string req;
    req += "GET " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + "\r\n";
    req += "User-Agent: scswitch/0.1 (Nintendo Switch; homebrew)\r\n";
    req += "Accept: */*\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "Connection: close\r\n";
    if (!extraHeaders.empty()) req += extraHeaders + "\r\n";
    req += "\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char*)req.data() + sent, req.size() - sent);
        if (ret <= 0) {
            tlsError("write", ret);
            res.error = "tls write";
            cleanup();
            return res;
        }
        sent += (size_t)ret;
    }

    Buffered reader;
    reader.ssl = &ssl;

    // headers
    std::string line;
    if (reader.getLine(line) <= 0) { res.error = "no response"; cleanup(); return res; }
    if (line.compare(0, 5, "HTTP/") != 0) { res.error = "bad status line"; cleanup(); return res; }
    size_t sp = line.find(' ');
    if (sp != std::string::npos) res.status = atoi(line.c_str() + sp + 1);

    std::string headers;
    while (reader.getLine(line) == 1) {
        if (line.empty()) break;
        headers += line + "\n";
        std::string l = util::lower(line);
        if (l.compare(0, 10, "location: ") == 0) res.location = util::trim(line.substr(10));
        else if (l.compare(0, 16, "content-length: ") == 0) res.contentLength = strtoull(line.c_str() + 16, nullptr, 10);
        else if (l.compare(0, 19, "transfer-encoding: ") == 0 && l.find("chunked") != std::string::npos) res.chunked = true;
    }

    if (!res.location.empty() && res.status >= 300 && res.status < 400) {
        cleanup();
        return res;
    }
    if (onTotal) onTotal(totalUser, res.contentLength);

    char tmp[8192];
    if (res.chunked) {
        while (true) {
            if (reader.getLine(line) <= 0) { res.error = "chunk read"; break; }
            size_t semi = line.find(';');
            if (semi != std::string::npos) line = line.substr(0, semi);
            unsigned long chunk = strtoul(line.c_str(), nullptr, 16);
            if (chunk == 0) {
                while (reader.getLine(line) == 1 && !line.empty()) {}
                break;
            }
            size_t left = chunk;
            while (left > 0) {
                size_t take = std::min(left, sizeof(tmp));
                if (reader.readExact(tmp, take) <= 0) { res.error = "chunk body"; left = 0; break; }
                if (!sink(sinkUser, tmp, take)) { res.error = "sink"; cleanup(); return res; }
                left -= take;
            }
            reader.getLine(line); // trailing CRLF
            if (reader.eof) break;
        }
    } else {
        uint64_t total = 0;
        while (true) {
            if (res.contentLength && total >= res.contentLength) break;
            if (reader.pos >= reader.len && reader.eof) break;
            if (reader.pos >= reader.len) {
                int r = reader.fill();
                if (r <= 0) break;
            }
            size_t take = reader.len - reader.pos;
            if (res.contentLength) take = std::min<uint64_t>(take, res.contentLength - total);
            if (!sink(sinkUser, reader.buf + reader.pos, take)) { res.error = "sink"; cleanup(); return res; }
            reader.pos += take;
            total += take;
        }
    }

    if (res.status < 200 || res.status >= 300) {
        if (res.error.empty()) res.error = util::fmt("http %d", res.status);
    }
    cleanup();
    return res;
}

} // namespace

bool init() {
    if (g_initialized) return true;
    socketInitializeDefault();
    nifmInitialize(NifmServiceType_User);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    int ret = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, nullptr, 0);
    if (ret) { tlsError("drbg seed", ret); return false; }
    mbedtls_x509_crt_init(&g_ca);

    std::string pem = util::readFile(cfg.ca_path);
    bool fromSd = !pem.empty();
    if (pem.empty()) pem = kCABundle;
    if (!pem.empty()) {
        int rc = mbedtls_x509_crt_parse(&g_ca, (const unsigned char*)pem.data(), pem.size() + 1);
        if (rc >= 0) {
            g_caLoaded = true;
            util::log("net: CA bundle loaded (%s, %u bytes, %d failures)", fromSd ? "SD" : "embedded",
                      (unsigned)pem.size(), rc);
        } else {
            tlsError("CA parse", rc);
        }
    }
    g_secure = g_caLoaded && !cfg.insecure_tls;
    g_initialized = true;
    return true;
}

void shutdown() {
    if (!g_initialized) return;
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    nifmExit();
    socketExit();
    g_initialized = false;
}

bool online() {
    NifmInternetConnectionType type;
    u32 strength;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}

bool isTlsSecure() { return g_secure; }

Response get(const std::string& url, const std::string& extraHeaders, int timeoutMs) {
    Response out;
    if (!g_initialized && !init()) { out.error = tr("сеть не инициализирована"); return out; }
    std::string current = url;
    for (int hop = 0; hop < 6; hop++) {
        Url parsed;
        if (!parseUrl(current, parsed)) { out.error = "bad url"; return out; }
        if (parsed.scheme != "https") { out.error = "only https supported"; return out; }
        out.body.clear();
        RequestResult r = perform(parsed, extraHeaders, [](void* user, const char* d, size_t n) {
            auto* body = (std::string*)user;
            body->append(d, n);
            return body->size() < (size_t)48 * 1024 * 1024;
        }, &out.body, nullptr, nullptr, timeoutMs);
        out.status = r.status;
        out.error = r.error;
        if (r.status >= 300 && r.status < 400 && !r.location.empty()) {
            current = r.location;
            if (current.compare(0, 1, "/") == 0) {
                current = "https://" + parsed.host + current;
            }
            continue;
        }
        return out;
    }
    out.error = "too many redirects";
    return out;
}

struct DownloadSink {
    FILE* f;
    uint64_t written;
    uint64_t total;
    Progress progress;
};

bool download(const std::string& url, const std::string& destPath,
              Progress progress, std::string& error, const std::string& extraHeaders, int timeoutMs) {
    if (!g_initialized && !init()) { error = tr("сеть не инициализирована"); return false; }
    std::string current = url;
    for (int hop = 0; hop < 6; hop++) {
        Url parsed;
        if (!parseUrl(current, parsed)) { error = "bad url"; return false; }
        if (parsed.scheme != "https") { error = "only https supported"; return false; }
        util::ensureDir(util::dirname(destPath));
        FILE* f = fopen(destPath.c_str(), "wb");
        if (!f) { error = "cannot create file"; return false; }
        DownloadSink ds{f, 0, 0, progress};
        RequestResult r = perform(parsed, extraHeaders, [](void* user, const char* d, size_t n) {
            auto* ds = (DownloadSink*)user;
            if (fwrite(d, 1, n, ds->f) != n) return false;
            ds->written += n;
            ds->progress.call(ds->written, ds->total);
            return true;
        }, &ds, [](void* user, uint64_t total) {
            auto* ds = (DownloadSink*)user;
            ds->total = total;
            ds->progress.call(ds->written, ds->total);
        }, &ds, timeoutMs);
        uint64_t written = ds.written;
        fclose(f);
        if (r.status >= 300 && r.status < 400 && !r.location.empty()) {
            util::removeFile(destPath);
            current = r.location;
            if (current.compare(0, 1, "/") == 0) current = "https://" + parsed.host + current;
            continue;
        }
        if (r.status >= 200 && r.status < 300 && written > 0) {
            progress.call(written, written);
            return true;
        }
        util::removeFile(destPath);
        error = r.error.empty() ? util::fmt("http %d", r.status) : r.error;
        return false;
    }
    error = "too many redirects";
    return false;
}

} // namespace net
