#include "net/Http.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <switch.h>
#include <sys/socket.h>

#include "util/Log.h"

namespace net {

// One line per run is enough; this is a property of the stack, not the request.
bool g_socketLogged = false;
std::atomic<int64_t> g_bytesRead{ 0 };
std::atomic<int64_t> g_stallMs{ 0 };
// A read blocking this long is a network event, not jitter, and it is long
// enough to empty the audio device.
constexpr double kStallLogMs = 250.0;

int64_t bytesRead() { return g_bytesRead.load(); }
int64_t stallMs() { return g_stallMs.load(); }

namespace {

mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_drbg;
mbedtls_x509_crt g_cacert;
std::mutex g_rngMutex;
TlsOptions g_tls;
bool g_caLoaded = false;
bool g_inited = false;

// mbedtls is built here without MBEDTLS_THREADING_C, so the shared DRBG needs
// its own lock before worker threads can pull from it concurrently.
int rngLocked(void* ctx, unsigned char* out, size_t len)
{
    std::lock_guard<std::mutex> lock(g_rngMutex);
    return mbedtls_ctr_drbg_random(ctx, out, len);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

std::string Url::origin() const
{
    const bool defaultPort = (tls && port == 443) || (!tls && port == 80);
    char buf[512];
    if (defaultPort)
        std::snprintf(buf, sizeof(buf), "%s://%s", scheme.c_str(), host.c_str());
    else
        std::snprintf(buf, sizeof(buf), "%s://%s:%d", scheme.c_str(), host.c_str(), port);
    return buf;
}

bool parseUrl(const std::string& in, Url& out)
{
    const size_t schemeEnd = in.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = lower(in.substr(0, schemeEnd));
    out.tls = out.scheme == "https";
    if (!out.tls && out.scheme != "http") return false;
    out.port = out.tls ? 443 : 80;

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = in.find('/', hostStart);
    std::string authority = pathStart == std::string::npos ? in.substr(hostStart)
                                                           : in.substr(hostStart, pathStart - hostStart);
    out.path = pathStart == std::string::npos ? "/" : in.substr(pathStart);
    if (out.path.empty()) out.path = "/";

    const size_t at = authority.find('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            out.port = std::atoi(authority.c_str() + close + 2);
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = std::atoi(authority.c_str() + colon + 1);
        } else {
            out.host = authority;
        }
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

std::string urlEncode(const std::string& in)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string joinUrl(const std::string& base, const std::string& ref)
{
    if (ref.find("://") != std::string::npos) return ref;
    Url b;
    if (!parseUrl(base, b)) return ref;
    if (!ref.empty() && ref[0] == '/') return b.origin() + ref;

    std::string dir = b.path;
    const size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
    return b.origin() + dir + ref;
}

void globalInit()
{
    if (g_inited) return;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_cacert);
    static const char* pers = "fliks-switch";
    mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                          reinterpret_cast<const unsigned char*>(pers), std::strlen(pers));
    g_inited = true;
}

void globalShutdown()
{
    if (!g_inited) return;
    mbedtls_x509_crt_free(&g_cacert);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    g_inited = false;
    g_caLoaded = false;
}

void setTlsOptions(const TlsOptions& opts)
{
    g_tls = opts;
    g_caLoaded = false;
    if (!opts.caFile.empty()) {
        mbedtls_x509_crt_free(&g_cacert);
        mbedtls_x509_crt_init(&g_cacert);
        g_caLoaded = mbedtls_x509_crt_parse_file(&g_cacert, opts.caFile.c_str()) == 0;
    }
}

TlsOptions tlsOptions() { return g_tls; }
bool caBundleLoaded() { return g_caLoaded; }

const std::string* Response::header(const std::string& name) const
{
    const std::string want = lower(name);
    for (const auto& h : headers)
        if (lower(h.first) == want) return &h.second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

namespace {

// A stalled segment fetch should surface as an error long before the applet
// looks unresponsive.
constexpr int kReadTimeoutMs = 10000;
// Beyond this a fresh ranged request beats reading through the gap.
constexpr int64_t kSkipLimit = 512 * 1024;
// Consecutive resume attempts before a stream is called dead. Reset by any
// successful read, so a long film may resume many times overall.
constexpr int kMaxResumes = 5;

class Connection {
public:
    ~Connection() { close(); }

    bool connect(const Url& url, int timeoutMs)
    {
        m_tls = url.tls;
        mbedtls_net_init(&m_net);
        char portStr[16];
        std::snprintf(portStr, sizeof(portStr), "%d", url.port);
        const uint64_t connectStart = armGetSystemTick();
        if (mbedtls_net_connect(&m_net, url.host.c_str(), portStr, MBEDTLS_NET_PROTO_TCP) != 0) {
            m_error = "connect failed";
            return false;
        }

        // Throughput over one TCP connection is the receive window divided by
        // the round trip. Both are logged once: if the window came back far
        // smaller than main.cpp asked for, a slow stream is this app's fault
        // and not the link's.
        if (!g_socketLogged) {
            g_socketLogged = true;
            int rcvBuf = 0;
            socklen_t optLen = sizeof(rcvBuf);
            const bool got =
                getsockopt(m_net.fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, &optLen) == 0;
            const double rttMs = armTicksToNs(armGetSystemTick() - connectStart) / 1.0e6;
            FLIKS_LOG("net: socket rcvbuf %s%d KiB, connect %.0fms -> ceiling ~%.1f Mbit/s",
                      got ? "" : "unknown ", rcvBuf / 1024, rttMs,
                      rttMs > 0.0 ? rcvBuf * 8.0 / (rttMs / 1000.0) / 1.0e6 : 0.0);
        }
        if (!m_tls) return true;

        mbedtls_ssl_init(&m_ssl);
        mbedtls_ssl_config_init(&m_conf);
        if (mbedtls_ssl_config_defaults(&m_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            m_error = "tls config failed";
            return false;
        }
        const bool verify = g_tls.verifyPeer && g_caLoaded;
        mbedtls_ssl_conf_authmode(&m_conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                  : MBEDTLS_SSL_VERIFY_NONE);
        if (verify) mbedtls_ssl_conf_ca_chain(&m_conf, &g_cacert, nullptr);
        mbedtls_ssl_conf_rng(&m_conf, rngLocked, &g_drbg);
        mbedtls_ssl_conf_read_timeout(&m_conf, static_cast<uint32_t>(timeoutMs));

        if (mbedtls_ssl_setup(&m_ssl, &m_conf) != 0) {
            m_error = "tls setup failed";
            return false;
        }
        mbedtls_ssl_set_hostname(&m_ssl, url.host.c_str());
        mbedtls_ssl_set_bio(&m_ssl, &m_net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);

        int rc;
        while ((rc = mbedtls_ssl_handshake(&m_ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char buf[128];
                mbedtls_strerror(rc, buf, sizeof(buf));
                m_error = std::string("tls handshake: ") + buf;
                return false;
            }
        }
        m_sslReady = true;
        return true;
    }

    void close()
    {
        if (m_sslReady) {
            mbedtls_ssl_close_notify(&m_ssl);
            mbedtls_ssl_free(&m_ssl);
            mbedtls_ssl_config_free(&m_conf);
            m_sslReady = false;
        } else if (m_tls) {
            mbedtls_ssl_free(&m_ssl);
            mbedtls_ssl_config_free(&m_conf);
        }
        mbedtls_net_free(&m_net);
        m_tls = false;
    }

    bool writeAll(const char* data, size_t len)
    {
        size_t sent = 0;
        while (sent < len) {
            int rc = m_tls
                         ? mbedtls_ssl_write(&m_ssl, reinterpret_cast<const unsigned char*>(data + sent),
                                             len - sent)
                         : mbedtls_net_send(&m_net, reinterpret_cast<const unsigned char*>(data + sent),
                                            len - sent);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc <= 0) return false;
            sent += static_cast<size_t>(rc);
        }
        return true;
    }

    // Returns 0 on clean EOF, negative on error.
    int readSome(uint8_t* buf, size_t len)
    {
        for (;;) {
            int rc = m_tls ? mbedtls_ssl_read(&m_ssl, buf, len)
                           : mbedtls_net_recv_timeout(&m_net, buf, len, m_timeoutMs);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return -1;
            return rc;
        }
    }

    void setTimeout(int ms) { m_timeoutMs = static_cast<uint32_t>(ms); }
    const std::string& error() const { return m_error; }

private:
    mbedtls_net_context m_net{};
    mbedtls_ssl_context m_ssl{};
    mbedtls_ssl_config m_conf{};
    bool m_tls = false;
    bool m_sslReady = false;
    uint32_t m_timeoutMs = 20000;
    std::string m_error;
};

// Buffers the socket so header parsing can work line-by-line without a
// one-byte-at-a-time read on a TLS record.
class Reader {
public:
    explicit Reader(Connection& conn) : m_conn(conn) {}

    bool line(std::string& out)
    {
        out.clear();
        for (;;) {
            const size_t nl = m_buf.find('\n', m_pos);
            if (nl != std::string::npos) {
                out = m_buf.substr(m_pos, nl - m_pos);
                m_pos = nl + 1;
                if (!out.empty() && out.back() == '\r') out.pop_back();
                compact();
                return true;
            }
            if (!fill()) return false;
        }
    }

    // Copies up to `len` bytes; returns 0 at EOF, negative on error.
    int read(uint8_t* dst, size_t len)
    {
        // Once the header buffer is drained, a body read has no reason to go
        // through it: recv straight into the caller's buffer, which is 64 KiB
        // where this staging buffer was 8. Four times fewer syscalls per
        // megabyte, and no copy.
        if (m_pos >= m_buf.size() && len >= kDirectRead) {
            if (m_eof) return 0;
            const int rc = m_conn.readSome(dst, len);
            if (rc < 0) {
                m_failed = true;
                m_eof = true;
                return -1;
            }
            if (rc == 0) {
                m_eof = true;
                return 0;
            }
            return rc;
        }
        if (m_pos >= m_buf.size() && !fill()) return m_eof ? 0 : -1;
        const size_t avail = m_buf.size() - m_pos;
        const size_t n = std::min(avail, len);
        std::memcpy(dst, m_buf.data() + m_pos, n);
        m_pos += n;
        compact();
        return static_cast<int>(n);
    }

    bool eof() const { return m_eof && m_pos >= m_buf.size(); }
    bool failed() const { return m_failed; }

private:
    // Above this, a read bypasses the staging buffer entirely.
    static constexpr size_t kDirectRead = 16384;

    bool fill()
    {
        if (m_eof) return false;
        uint8_t tmp[8192];
        const int rc = m_conn.readSome(tmp, sizeof(tmp));
        if (rc < 0) m_failed = true;
        if (rc <= 0) {
            m_eof = true;
            return false;
        }
        m_buf.append(reinterpret_cast<char*>(tmp), static_cast<size_t>(rc));
        return true;
    }

    void compact()
    {
        if (m_pos > 32768) {
            m_buf.erase(0, m_pos);
            m_pos = 0;
        }
    }

    Connection& m_conn;
    std::string m_buf;
    size_t m_pos = 0;
    bool m_eof = false;
    bool m_failed = false;
};

std::string buildRequest(const Request& req, const Url& url, int64_t rangeStart)
{
    std::string out;
    out.reserve(512);
    out += req.method + " " + url.path + " HTTP/1.1\r\n";
    out += "Host: " + url.host;
    if (!((url.tls && url.port == 443) || (!url.tls && url.port == 80)))
        out += ":" + std::to_string(url.port);
    out += "\r\n";
    out += "User-Agent: Fliks-Switch/" APP_VERSION "\r\n";
    out += "Accept: */*\r\n";
    out += "Connection: close\r\n";
    if (rangeStart > 0) out += "Range: bytes=" + std::to_string(rangeStart) + "-\r\n";
    for (const auto& h : req.headers) out += h.first + ": " + h.second + "\r\n";
    if (!req.body.empty()) {
        out += "Content-Type: " +
               (req.contentType.empty() ? std::string("application/json") : req.contentType) +
               "\r\n";
        out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    out += "\r\n";
    out += req.body;
    return out;
}

bool readStatusAndHeaders(Reader& reader, Response& res)
{
    std::string statusLine;
    if (!reader.line(statusLine)) {
        res.error = "no response";
        return false;
    }
    const size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) {
        res.error = "bad status line";
        return false;
    }
    res.status = std::atoi(statusLine.c_str() + sp + 1);

    std::string line;
    while (reader.line(line)) {
        if (line.empty()) return true;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        res.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    res.error = "truncated headers";
    return false;
}

// Consumes a chunked or content-length body into `out`.
bool readBody(Reader& reader, const Response& res, std::string& out)
{
    const std::string* te = res.header("transfer-encoding");
    if (te && lower(*te).find("chunked") != std::string::npos) {
        for (;;) {
            std::string sizeLine;
            if (!reader.line(sizeLine)) return false;
            const size_t semi = sizeLine.find(';');
            if (semi != std::string::npos) sizeLine.resize(semi);
            const long chunk = std::strtol(sizeLine.c_str(), nullptr, 16);
            if (chunk <= 0) break;
            size_t remaining = static_cast<size_t>(chunk);
            uint8_t buf[8192];
            while (remaining > 0) {
                const int n = reader.read(buf, std::min(remaining, sizeof(buf)));
                if (n <= 0) return false;
                out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
                remaining -= static_cast<size_t>(n);
            }
            std::string crlf;
            reader.line(crlf);
        }
        return true;
    }

    const std::string* cl = res.header("content-length");
    uint8_t buf[8192];
    if (cl) {
        size_t remaining = static_cast<size_t>(std::strtoull(cl->c_str(), nullptr, 10));
        out.reserve(remaining);
        while (remaining > 0) {
            const int n = reader.read(buf, std::min(remaining, sizeof(buf)));
            if (n <= 0) return n == 0;
            out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }

    for (;;) {
        const int n = reader.read(buf, sizeof(buf));
        if (n <= 0) return true;
        out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
    }
}

} // namespace

Response perform(const Request& req)
{
    Response res;
    std::string target = req.url;

    for (int hop = 0; hop <= req.maxRedirects; hop++) {
        Url url;
        if (!parseUrl(target, url)) {
            res.error = "bad url: " + target;
            return res;
        }

        Connection conn;
        conn.setTimeout(req.timeoutMs);
        if (!conn.connect(url, req.timeoutMs)) {
            res.error = conn.error();
            return res;
        }

        const std::string wire = buildRequest(req, url, 0);
        if (!conn.writeAll(wire.data(), wire.size())) {
            res.error = "write failed";
            return res;
        }

        res = Response{};
        Reader reader(conn);
        if (!readStatusAndHeaders(reader, res)) return res;

        if (res.status >= 300 && res.status < 400) {
            const std::string* loc = res.header("location");
            if (loc && hop < req.maxRedirects) {
                target = joinUrl(target, *loc);
                continue;
            }
        }
        if (!readBody(reader, res, res.body)) {
            if (res.body.empty()) res.error = "body read failed";
        }
        return res;
    }

    res.error = "too many redirects";
    return res;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

struct Stream::Impl {
    Connection conn;
    std::unique_ptr<Reader> reader;
    bool open = false;
    volatile bool aborted = false;
};

Stream::Stream() : m_impl(new Impl) {}
Stream::~Stream() { close(); }

bool Stream::open(const std::string& url, const std::vector<Header>& headers, int64_t offset)
{
    m_url = url;
    m_headers = headers;
    return reopenAt(offset);
}

bool Stream::reopenAt(int64_t offset)
{
    close();
    m_impl.reset(new Impl);

    std::string target = m_url;
    for (int hop = 0; hop < 5; hop++) {
        Url url;
        if (!parseUrl(target, url)) return false;

        m_impl->conn.setTimeout(kReadTimeoutMs);
        if (!m_impl->conn.connect(url, kReadTimeoutMs)) return false;

        Request req;
        req.headers = m_headers;
        const std::string wire = buildRequest(req, url, offset);
        if (!m_impl->conn.writeAll(wire.data(), wire.size())) return false;

        m_impl->reader.reset(new Reader(m_impl->conn));
        Response res;
        if (!readStatusAndHeaders(*m_impl->reader, res)) return false;

        if (res.status >= 300 && res.status < 400) {
            const std::string* loc = res.header("location");
            if (loc) {
                target = joinUrl(target, *loc);
                m_impl.reset(new Impl);
                continue;
            }
        }
        if (!res.ok()) return false;

        if (const std::string* cr = res.header("content-range")) {
            const size_t slash = cr->rfind('/');
            if (slash != std::string::npos)
                m_size = std::strtoll(cr->c_str() + slash + 1, nullptr, 10);
        } else if (const std::string* cl = res.header("content-length")) {
            m_size = std::strtoll(cl->c_str(), nullptr, 10);
        }

        m_pos = 0;
        m_impl->open = true;

        // A server may answer a Range request with the whole entity. Taking
        // 200 at face value would leave every later byte offset by the amount
        // we asked to skip, which reads as a corrupt container rather than as
        // an error.
        if (offset > 0 && res.status != 206) {
            if (!skipForward(offset)) return false;
        } else {
            m_pos = offset;
            if (offset > 0 && m_size >= 0) m_size += 0;   // content-range already absolute
        }

        // Knowing the length is enough to be seekable: a seek that cannot be
        // served from the current position is just another request.
        m_seekable = m_size >= 0;
        return true;
    }
    return false;
}

bool Stream::skipForward(int64_t bytes)
{
    uint8_t scratch[8192];
    while (bytes > 0) {
        const int want = static_cast<int>(std::min<int64_t>(sizeof(scratch), bytes));
        const int n = read(scratch, want);
        if (n <= 0) return false;
        bytes -= n;
    }
    return true;
}

int Stream::read(uint8_t* buf, int len)
{
    if (!m_impl->open || m_impl->aborted) return -1;

    for (;;) {
        const uint64_t start = armGetSystemTick();
        const int n = m_impl->reader->read(buf, static_cast<size_t>(len));
        const double waitedMs = armTicksToNs(armGetSystemTick() - start) / 1.0e6;
        if (waitedMs >= kStallLogMs) {
            g_stallMs.fetch_add(static_cast<int64_t>(waitedMs));
            FLIKS_LOG("net: stalled %.0fms at %lld", waitedMs,
                      static_cast<long long>(m_pos));
        }
        if (n > 0) {
            g_bytesRead.fetch_add(n);
            m_pos += n;
            m_retries = 0;
            return n;
        }
        // A clean end of stream is not a failure; a dropped connection is,
        // and must not read back as one.
        if (n == 0 && !m_impl->reader->failed()) return 0;
        if (m_impl->aborted) return -1;

        // Reading half a gigabyte over one connection outlives server and
        // proxy idle timeouts as a matter of course. Resume from where the
        // read stopped instead of ending playback.
        if (!m_seekable || m_retries >= kMaxResumes) return -1;
        m_retries++;
        const int64_t resume = m_pos;
        FLIKS_LOG("http: connection dropped at %lld, resuming (attempt %d)",
                  static_cast<long long>(resume), m_retries);
        if (!reopenAt(resume) || m_pos != resume) return -1;
    }
}

int64_t Stream::seek(int64_t pos, int whence)
{
    int64_t target = pos;
    if (whence == SEEK_CUR) target = m_pos + pos;
    else if (whence == SEEK_END) {
        if (m_size < 0) return -1;
        target = m_size + pos;
    }
    if (target < 0) return -1;
    if (target == m_pos) return m_pos;

    // A short hop forward costs less read through than re-requested, and it
    // keeps the connection — which matters most inside a container the
    // demuxer is walking in small steps.
    if (target > m_pos && target - m_pos <= kSkipLimit) {
        if (!skipForward(target - m_pos)) return -1;
        return m_pos;
    }
    if (m_size < 0) return -1;
    if (!reopenAt(target)) return -1;
    return m_pos;
}

void Stream::close()
{
    if (m_impl && m_impl->open) {
        m_impl->reader.reset();
        m_impl->conn.close();
        m_impl->open = false;
    }
}

void Stream::abort()
{
    if (m_impl) m_impl->aborted = true;
}

} // namespace net
