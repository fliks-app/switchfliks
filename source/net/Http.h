#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// devkitPro's switch-curl ships with every TLS backend compiled out, so the
// client talks HTTP/1.1 straight over mbedtls instead. It also gives the
// player a seekable reader to hang ffmpeg's AVIO off, which curl would not.
namespace net {

// Bytes pulled and time spent blocked across every stream, direct play and
// HLS alike. Counted here rather than in the ffmpeg callback because the feed
// reads on its own thread and never passes through one.
int64_t bytesRead();
int64_t stallMs();

struct Url {
    std::string scheme = "http";
    std::string host;
    std::string path = "/";
    int port = 80;
    bool tls = false;

    std::string origin() const;
};

bool parseUrl(const std::string& in, Url& out);
std::string urlEncode(const std::string& in);
std::string joinUrl(const std::string& base, const std::string& ref);

using Header = std::pair<std::string, std::string>;

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<Header> headers;
    std::string body;
    std::string contentType;
    int timeoutMs = 20000;
    int maxRedirects = 5;
};

struct Response {
    int status = 0;
    std::string body;
    std::vector<Header> headers;
    std::string error;

    bool ok() const { return status >= 200 && status < 300; }
    const std::string* header(const std::string& name) const;
};

struct TlsOptions {
    bool verifyPeer = true;
    std::string caFile;
};

void globalInit();
void globalShutdown();
void setTlsOptions(const TlsOptions& opts);
TlsOptions tlsOptions();
// True once a CA bundle has actually been parsed; the server screen surfaces
// this so "insecure" is never silently on.
bool caBundleLoaded();

Response perform(const Request& req);

// Incremental reader used by the player. Range requests make it seekable when
// the server advertises byte ranges, which Fliks does for both the progressive
// stream endpoint and HLS segments.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    bool open(const std::string& url, const std::vector<Header>& headers, int64_t offset = 0);
    int read(uint8_t* buf, int len);
    int64_t seek(int64_t pos, int whence);
    int64_t size() const { return m_size; }
    bool seekable() const { return m_seekable; }
    void close();
    void abort();

private:
    bool reopenAt(int64_t offset);
    bool skipForward(int64_t bytes);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_url;
    std::vector<Header> m_headers;
    int64_t m_pos = 0;
    int64_t m_size = -1;
    int m_retries = 0;
    bool m_seekable = false;
};

} // namespace net
