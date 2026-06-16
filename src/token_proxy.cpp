/*
 * token_proxy.cpp — Phase 4: LLM API Token Tracker Proxy
 *
 * A local HTTP proxy that intercepts LLM API calls, counts tokens,
 * and estimates the datacenter carbon footprint.
 *
 * How it works:
 *   1. Client sends requests to localhost:8888 with X-Forward-Host header
 *   2. Proxy opens HTTPS connection to the real API and forwards the request
 *   3. Parses response JSON for token usage counts
 *   4. Estimates CO₂ emissions using published per-token energy figures
 *   5. Returns the unmodified API response to the client
 *
 * Usage:
 *   ./build/token_proxy              # Start proxy on port 8888
 *   curl http://localhost:8888/stats  # View cumulative carbon stats
 *
 * Compile:
 *   clang++ -std=c++17 -O2 \
 *     -framework Security -framework CoreFoundation \
 *     src/token_proxy.cpp -o build/token_proxy
 */

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <utility>
#include <cctype>
#include <cstdlib>

// POSIX sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

// macOS TLS (SecureTransport — deprecated but functional, pure-C API)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#pragma clang diagnostic pop

// ─── Constants ──────────────────────────────────────────────────────────────

static const int    PROXY_PORT              = 8888;
static const int    BACKLOG                 = 10;
static const size_t RECV_BUF_SIZE           = 8192;

// Carbon estimation constants
// Sources: Patterson et al. (2022), Luccioni et al. (2023), IEA
// GPT-4-class datacenter energy: ~3 Wh per 1000 tokens (includes cooling/PUE)
static const double ENERGY_PER_1K_TOKENS_WH    = 3.0;
// Global average grid carbon intensity (grams CO₂ per kWh)
static const double CARBON_INTENSITY_G_PER_KWH = 400.0;

// ANSI colors for console output
#define CLR_RESET   "\033[0m"
#define CLR_GREEN   "\033[38;5;114m"
#define CLR_YELLOW  "\033[38;5;221m"
#define CLR_CYAN    "\033[38;5;117m"
#define CLR_RED     "\033[38;5;203m"
#define CLR_DIM     "\033[38;5;244m"
#define CLR_BOLD    "\033[1m"
#define CLR_LEAF    "\033[38;5;71m"

// ─── Data Structures ────────────────────────────────────────────────────────

struct RequestLog {
    std::string timestamp;
    std::string source;
    std::string model;
    std::string provider_host;
    std::string path;
    int prompt_tokens      = 0;
    int completion_tokens  = 0;
    int total_tokens       = 0;
    double co2_grams       = 0.0;
    double energy_wh       = 0.0;
};

struct ProxyStats {
    int       total_requests          = 0;
    long long total_prompt_tokens     = 0;
    long long total_completion_tokens = 0;
    long long total_tokens            = 0;
    double    total_co2_grams         = 0.0;
    double    total_energy_wh         = 0.0;
    std::string last_source;
    std::string last_model;
    std::string last_platform;        // "chatgpt", "gemini", "claude", or "unknown"
    std::vector<RequestLog> logs;
};

struct HttpMessage {
    std::string method;
    std::string path;
    std::string version;
    // Headers stored as (original-case-key, value) pairs to preserve order+casing
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // Case-insensitive header lookup
    std::string getHeader(const std::string& key_lower) const {
        for (const auto& h : headers) {
            std::string k = h.first;
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            if (k == key_lower) return h.second;
        }
        return "";
    }
};

struct UsageCounts {
    int prompt_tokens     = 0;
    int completion_tokens = 0;
    int total_tokens      = 0;
    bool found            = false;
};

// ─── Globals ────────────────────────────────────────────────────────────────

static std::mutex        g_stats_mutex;
static ProxyStats        g_stats;
static std::atomic<bool> g_running(true);
static int               g_server_fd = -1;

int configuredPort() {
    const char* env_port = std::getenv("CARBON_PROXY_PORT");
    if (!env_port || !*env_port) return PROXY_PORT;

    try {
        int port = std::stoi(env_port);
        if (port > 0 && port < 65536) return port;
    } catch (...) {
    }

    return PROXY_PORT;
}

// ─── Signal Handler ─────────────────────────────────────────────────────────

void signalHandler(int) {
    std::cout << "\n" << CLR_YELLOW << "⚡ Shutting down proxy..." << CLR_RESET << std::endl;
    g_running = false;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

// ─── Timestamp Helper ───────────────────────────────────────────────────────

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// ─── JSON Helpers ───────────────────────────────────────────────────────────
// Lightweight scanners — no external JSON library needed.

int extractJsonInt(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return -1;

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;

    std::string num;
    while (pos < json.size() && (std::isdigit(static_cast<unsigned char>(json[pos])) || json[pos] == '-'))
        num += json[pos++];

    if (num.empty()) return -1;
    try { return std::stoi(num); } catch (...) { return -1; }
}

std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;

    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            result += json[++pos];
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream oss;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b";  break;
            case '\f': oss << "\\f";  break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            default:
                if (ch < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    oss << static_cast<char>(ch);
                }
        }
    }
    return oss.str();
}

int firstPositiveJsonInt(const std::string& json, const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        int value = extractJsonInt(json, key);
        if (value >= 0) return value;
    }
    return -1;
}

UsageCounts extractUsageCounts(const std::string& response_body) {
    UsageCounts usage;

    size_t usage_pos = response_body.find("\"usage\"");
    if (usage_pos == std::string::npos) return usage;

    usage.found = true;
    std::string usage_section = response_body.substr(usage_pos);

    int prompt = firstPositiveJsonInt(usage_section, {
        "prompt_tokens",
        "input_tokens",
        "input_token_count"
    });

    int completion = firstPositiveJsonInt(usage_section, {
        "completion_tokens",
        "output_tokens",
        "generated_tokens",
        "output_token_count"
    });

    int total = firstPositiveJsonInt(usage_section, {
        "total_tokens",
        "total_token_count"
    });

    usage.prompt_tokens     = std::max(0, prompt);
    usage.completion_tokens = std::max(0, completion);
    usage.total_tokens      = total > 0 ? total : usage.prompt_tokens + usage.completion_tokens;

    return usage;
}

void recordUsage(const std::string& source,
                 const std::string& model,
                 const std::string& provider_host,
                 const std::string& path,
                 int prompt_tokens,
                 int completion_tokens,
                 int total_tokens) {
    if (prompt_tokens < 0) prompt_tokens = 0;
    if (completion_tokens < 0) completion_tokens = 0;
    if (total_tokens <= 0) total_tokens = prompt_tokens + completion_tokens;

    double energy_wh = (total_tokens / 1000.0) * ENERGY_PER_1K_TOKENS_WH;
    double co2_grams = energy_wh * CARBON_INTENSITY_G_PER_KWH / 1000.0;

    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_stats.total_requests++;
    g_stats.total_prompt_tokens     += prompt_tokens;
    g_stats.total_completion_tokens += completion_tokens;
    g_stats.total_tokens            += total_tokens;
    g_stats.total_co2_grams         += co2_grams;
    g_stats.total_energy_wh         += energy_wh;
    g_stats.last_source              = source;
    g_stats.last_model               = model;

    // Derive platform from source string (e.g. "chatgpt-extension" -> "chatgpt")
    if (source.find("chatgpt") != std::string::npos || source.find("openai") != std::string::npos)
        g_stats.last_platform = "chatgpt";
    else if (source.find("gemini") != std::string::npos)
        g_stats.last_platform = "gemini";
    else if (source.find("claude") != std::string::npos)
        g_stats.last_platform = "claude";
    else if (!source.empty())
        g_stats.last_platform = "unknown";

    RequestLog log;
    log.timestamp         = getCurrentTimestamp();
    log.source            = source;
    log.model             = model;
    log.provider_host     = provider_host;
    log.path              = path;
    log.prompt_tokens     = prompt_tokens;
    log.completion_tokens = completion_tokens;
    log.total_tokens      = total_tokens;
    log.co2_grams         = co2_grams;
    log.energy_wh         = energy_wh;
    g_stats.logs.push_back(log);
}

// ─── HTTP Parsing ───────────────────────────────────────────────────────────

// Read a full HTTP message (headers + body) from a socket
std::string readFullHttp(int fd) {
    std::string data;
    char buf[RECV_BUF_SIZE];

    // Read until we find the end-of-headers marker
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }

    size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) return data;

    size_t body_start = header_end + 4;

    // Parse Content-Length from headers to know how much body to read
    std::string lower_data = data.substr(0, header_end);
    std::transform(lower_data.begin(), lower_data.end(), lower_data.begin(), ::tolower);
    size_t cl_pos = lower_data.find("content-length:");
    int content_length = 0;

    if (cl_pos != std::string::npos) {
        size_t val_start = cl_pos + 15;
        while (val_start < lower_data.size() && lower_data[val_start] == ' ') val_start++;
        std::string cl_str;
        while (val_start < lower_data.size() && std::isdigit(static_cast<unsigned char>(lower_data[val_start])))
            cl_str += lower_data[val_start++];
        if (!cl_str.empty()) content_length = std::stoi(cl_str);
    }

    // Read remaining body bytes
    while (static_cast<int>(data.size() - body_start) < content_length) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }

    return data;
}

HttpMessage parseHttpRequest(const std::string& raw) {
    HttpMessage msg;

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return msg;

    std::string header_block = raw.substr(0, header_end);
    msg.body = raw.substr(header_end + 4);

    // Parse request line
    std::istringstream iss(header_block);
    std::string line;
    if (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream lss(line);
        lss >> msg.method >> msg.path >> msg.version;
    }

    // Parse headers into ordered pairs
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            size_t vstart = val.find_first_not_of(' ');
            if (vstart != std::string::npos) val = val.substr(vstart);
            msg.headers.emplace_back(key, val);
        }
    }

    return msg;
}

// ─── TLS Client via SecureTransport ─────────────────────────────────────────
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static OSStatus tlsRead(SSLConnectionRef conn, void* data, size_t* dataLength) {
    int fd = *static_cast<const int*>(conn);
    size_t requested = *dataLength;
    ssize_t n = read(fd, data, requested);
    if (n > 0) {
        *dataLength = static_cast<size_t>(n);
        return (static_cast<size_t>(n) < requested) ? errSSLWouldBlock : noErr;
    } else if (n == 0) {
        *dataLength = 0;
        return errSSLClosedGraceful;
    } else {
        *dataLength = 0;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock : errSSLClosedAbort;
    }
}

static OSStatus tlsWrite(SSLConnectionRef conn, const void* data, size_t* dataLength) {
    int fd = *static_cast<const int*>(conn);
    ssize_t n = write(fd, data, *dataLength);
    if (n > 0) {
        *dataLength = static_cast<size_t>(n);
        return noErr;
    } else {
        *dataLength = 0;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock : errSSLClosedAbort;
    }
}

/// Open an HTTPS connection to `host:443`, send `request`, return full response.
std::string forwardHttps(const std::string& host, const std::string& request) {
    // 1. DNS resolve
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || !res) {
        std::cerr << CLR_RED << "  ✗ DNS resolution failed for " << host << CLR_RESET << std::endl;
        return "";
    }

    // 2. TCP connect
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return ""; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        std::cerr << CLR_RED << "  ✗ TCP connect failed to " << host << ":443" << CLR_RESET << std::endl;
        close(fd);
        freeaddrinfo(res);
        return "";
    }
    freeaddrinfo(res);

    // 3. TLS handshake via SecureTransport
    SSLContextRef ssl = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
    if (!ssl) { close(fd); return ""; }

    SSLSetIOFuncs(ssl, tlsRead, tlsWrite);
    SSLSetConnection(ssl, &fd);
    SSLSetPeerDomainName(ssl, host.c_str(), host.length());

    OSStatus status;
    do {
        status = SSLHandshake(ssl);
    } while (status == errSSLWouldBlock);

    if (status != noErr) {
        std::cerr << CLR_RED << "  ✗ TLS handshake failed (OSStatus " << status << ")" << CLR_RESET << std::endl;
        CFRelease(ssl);
        close(fd);
        return "";
    }

    // 4. Send request
    size_t total_written = 0;
    while (total_written < request.size()) {
        size_t written = 0;
        status = SSLWrite(ssl, request.c_str() + total_written,
                          request.size() - total_written, &written);
        total_written += written;
        if (status != noErr && status != errSSLWouldBlock) break;
    }

    // 5. Read response
    std::string response;
    char buf[RECV_BUF_SIZE];
    bool   headers_done   = false;
    int    content_length  = -1;
    bool   chunked         = false;
    size_t body_start      = 0;

    while (true) {
        size_t bytesRead = 0;
        status = SSLRead(ssl, buf, sizeof(buf), &bytesRead);

        if (bytesRead > 0)
            response.append(buf, bytesRead);

        // Detect end of headers
        if (!headers_done) {
            size_t hend = response.find("\r\n\r\n");
            if (hend != std::string::npos) {
                headers_done = true;
                body_start   = hend + 4;

                std::string hdr_lower = response.substr(0, hend);
                std::transform(hdr_lower.begin(), hdr_lower.end(), hdr_lower.begin(), ::tolower);

                size_t cl_pos = hdr_lower.find("content-length:");
                if (cl_pos != std::string::npos) {
                    size_t vs = cl_pos + 15;
                    while (vs < hdr_lower.size() && hdr_lower[vs] == ' ') vs++;
                    std::string cl_str;
                    while (vs < hdr_lower.size() && std::isdigit(static_cast<unsigned char>(hdr_lower[vs])))
                        cl_str += hdr_lower[vs++];
                    if (!cl_str.empty()) content_length = std::stoi(cl_str);
                }

                chunked = hdr_lower.find("transfer-encoding: chunked") != std::string::npos;
            }
        }

        // Check if we have the full response body
        if (headers_done) {
            if (content_length >= 0) {
                if (static_cast<int>(response.size() - body_start) >= content_length) break;
            } else if (chunked) {
                // Final chunk marker: "0\r\n\r\n" (possibly preceded by \r\n)
                std::string body_so_far = response.substr(body_start);
                if (body_so_far.find("\r\n0\r\n\r\n") != std::string::npos ||
                    (body_so_far.size() >= 5 && body_so_far.substr(0, 5) == "0\r\n\r\n"))
                    break;
            }
        }

        if (status == errSSLClosedGraceful || status == errSSLClosedAbort) break;
        if (status != noErr && status != errSSLWouldBlock) break;
    }

    // 6. Cleanup
    SSLClose(ssl);
    CFRelease(ssl);
    close(fd);

    return response;
}

#pragma clang diagnostic pop

// ─── Decode Chunked Transfer-Encoding ───────────────────────────────────────

std::string decodeChunked(const std::string& chunked_body) {
    std::string result;
    size_t pos = 0;

    while (pos < chunked_body.size()) {
        size_t line_end = chunked_body.find("\r\n", pos);
        if (line_end == std::string::npos) break;

        std::string size_str = chunked_body.substr(pos, line_end - pos);
        // Strip chunk extensions (e.g. ";ext=value")
        size_t semi = size_str.find(';');
        if (semi != std::string::npos) size_str = size_str.substr(0, semi);

        int chunk_size = 0;
        try { chunk_size = std::stoi(size_str, nullptr, 16); } catch (...) { break; }
        if (chunk_size == 0) break; // final chunk

        size_t data_start = line_end + 2;
        if (data_start + static_cast<size_t>(chunk_size) > chunked_body.size()) break;

        result.append(chunked_body, data_start, static_cast<size_t>(chunk_size));
        pos = data_start + static_cast<size_t>(chunk_size) + 2; // skip data + \r\n
    }

    return result;
}

// ─── Build Stats JSON ───────────────────────────────────────────────────────

std::string buildStatsJson() {
    std::lock_guard<std::mutex> lock(g_stats_mutex);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << "{\n";
    oss << "  \"total_requests\": "          << g_stats.total_requests          << ",\n";
    oss << "  \"total_prompt_tokens\": "     << g_stats.total_prompt_tokens     << ",\n";
    oss << "  \"total_completion_tokens\": " << g_stats.total_completion_tokens << ",\n";
    oss << "  \"total_tokens\": "            << g_stats.total_tokens            << ",\n";
    oss << "  \"total_co2_grams\": "         << g_stats.total_co2_grams        << ",\n";
    oss << "  \"total_energy_wh\": "         << g_stats.total_energy_wh        << ",\n";
    oss << "  \"energy_per_1k_tokens_wh\": " << ENERGY_PER_1K_TOKENS_WH        << ",\n";
    oss << "  \"carbon_intensity_g_per_kwh\": " << CARBON_INTENSITY_G_PER_KWH    << ",\n";
    oss << "  \"last_platform\": \""     << jsonEscape(g_stats.last_platform) << "\",\n";
    oss << "  \"last_source\": \""       << jsonEscape(g_stats.last_source)   << "\",\n";
    oss << "  \"last_model\": \""        << jsonEscape(g_stats.last_model)    << "\",\n";
    oss << "  \"requests\": [\n";

    for (size_t i = 0; i < g_stats.logs.size(); ++i) {
        const auto& log = g_stats.logs[i];
        oss << "    {\n";
        oss << "      \"timestamp\": \""         << jsonEscape(log.timestamp)     << "\",\n";
        oss << "      \"source\": \""            << jsonEscape(log.source)        << "\",\n";
        oss << "      \"provider_host\": \""     << jsonEscape(log.provider_host) << "\",\n";
        oss << "      \"path\": \""              << jsonEscape(log.path)          << "\",\n";
        oss << "      \"model\": \""             << jsonEscape(log.model)         << "\",\n";
        oss << "      \"prompt_tokens\": "       << log.prompt_tokens    << ",\n";
        oss << "      \"completion_tokens\": "   << log.completion_tokens << ",\n";
        oss << "      \"total_tokens\": "        << log.total_tokens     << ",\n";
        oss << "      \"co2_grams\": "           << log.co2_grams        << ",\n";
        oss << "      \"energy_wh\": "           << log.energy_wh        << "\n";
        oss << "    }" << (i + 1 < g_stats.logs.size() ? "," : "") << "\n";
    }

    oss << "  ]\n";
    oss << "}\n";

    return oss.str();
}

void resetStats() {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_stats = ProxyStats{};
}

// ─── Send HTTP Response to Client ───────────────────────────────────────────

void sendResponse(int client_fd, int status_code, const std::string& status_text,
                  const std::string& content_type, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << body;

    std::string resp = oss.str();
    send(client_fd, resp.c_str(), resp.size(), 0);
}

// ─── Handle a Single Client Connection ──────────────────────────────────────

void handleClient(int client_fd) {
    // 1. Read the full HTTP request from the client
    std::string raw = readFullHttp(client_fd);
    if (raw.empty()) {
        close(client_fd);
        return;
    }

    HttpMessage req = parseHttpRequest(raw);

    // ── /stats endpoint ──────────────────────────────────────────────────
    if (req.method == "GET" && req.path == "/stats") {
        std::string json = buildStatsJson();
        sendResponse(client_fd, 200, "OK", "application/json", json);
        close(client_fd);
        return;
    }

    // ── /health endpoint ────────────────────────────────────────────────
    if (req.method == "GET" && req.path == "/health") {
        sendResponse(client_fd, 200, "OK", "application/json", "{\"status\":\"ok\"}\n");
        close(client_fd);
        return;
    }

    // ── /stats/reset endpoint ───────────────────────────────────────────
    if (req.method == "POST" && req.path == "/stats/reset") {
        resetStats();
        sendResponse(client_fd, 200, "OK", "application/json", "{\"status\":\"reset\"}\n");
        close(client_fd);
        return;
    }

    // ── Browser extension usage estimates ───────────────────────────────
    if (req.method == "POST" && req.path == "/browser-usage") {
        int prompt_tokens = firstPositiveJsonInt(req.body, {
            "prompt_tokens",
            "input_tokens"
        });
        int completion_tokens = firstPositiveJsonInt(req.body, {
            "completion_tokens",
            "output_tokens"
        });
        int total_tokens = firstPositiveJsonInt(req.body, {
            "total_tokens"
        });

        if (prompt_tokens < 0) prompt_tokens = 0;
        if (completion_tokens < 0) completion_tokens = 0;
        if (total_tokens <= 0) total_tokens = prompt_tokens + completion_tokens;

        if (total_tokens <= 0) {
            sendResponse(client_fd, 400, "Bad Request", "application/json",
                "{\"error\":\"No positive token estimate provided\"}\n");
            close(client_fd);
            return;
        }

        std::string model = extractJsonString(req.body, "model");
        std::string source = extractJsonString(req.body, "source");
        if (source.empty()) source = "browser-extension";

        // Derive provider_host from the source field instead of hardcoding
        std::string provider_host = "unknown";
        std::string display_name = "Unknown";
        if (source.find("chatgpt") != std::string::npos || source.find("openai") != std::string::npos) {
            provider_host = "chatgpt.com";
            display_name = "ChatGPT";
            if (model.empty()) model = "chatgpt.com-visible-estimate";
        } else if (source.find("gemini") != std::string::npos) {
            provider_host = "gemini.google.com";
            display_name = "Gemini";
            if (model.empty()) model = "gemini-visible-estimate";
        } else if (source.find("claude") != std::string::npos) {
            provider_host = "claude.ai";
            display_name = "Claude";
            if (model.empty()) model = "claude-visible-estimate";
        } else {
            if (model.empty()) model = "unknown-estimate";
        }

        std::string page_url = extractJsonString(req.body, "page_url");
        if (page_url.empty()) page_url = "/browser-usage";

        recordUsage(source, model, provider_host, page_url,
                    prompt_tokens, completion_tokens, total_tokens);

        double energy_wh = (total_tokens / 1000.0) * ENERGY_PER_1K_TOKENS_WH;
        double co2_grams = energy_wh * CARBON_INTENSITY_G_PER_KWH / 1000.0;

        std::ostringstream body;
        body << std::fixed << std::setprecision(6)
             << "{\"status\":\"ok\",\"total_tokens\":" << total_tokens
             << ",\"co2_grams\":" << co2_grams << "}\n";
        sendResponse(client_fd, 200, "OK", "application/json", body.str());
        close(client_fd);

        std::cout << CLR_GREEN << "  ✓ " << CLR_RESET
                  << CLR_BOLD << display_name << " estimate" << CLR_RESET << " | "
                  << CLR_CYAN << "Tokens: " << total_tokens
                  << CLR_DIM << " (prompt:" << prompt_tokens
                  << " + completion:" << completion_tokens << ")" << CLR_RESET
                  << " | " << CLR_LEAF << "🌿 " << std::fixed << std::setprecision(4)
                  << co2_grams << "g CO₂" << CLR_RESET << std::endl;
        return;
    }

    // ── CORS preflight ───────────────────────────────────────────────────
    if (req.method == "OPTIONS") {
        std::ostringstream oss;
        oss << "HTTP/1.1 204 No Content\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: *\r\n"
            << "Connection: close\r\n\r\n";
        std::string resp = oss.str();
        send(client_fd, resp.c_str(), resp.size(), 0);
        close(client_fd);
        return;
    }

    // ── Determine forward host ───────────────────────────────────────────
    std::string forward_host = req.getHeader("x-forward-host");
    if (forward_host.empty()) {
        std::string host_hdr = req.getHeader("host");
        size_t colon = host_hdr.find(':');
        forward_host = (colon != std::string::npos) ? host_hdr.substr(0, colon) : host_hdr;
    }

    if (forward_host.empty() || forward_host == "localhost" || forward_host == "127.0.0.1") {
        sendResponse(client_fd, 400, "Bad Request", "application/json",
            "{\"error\": \"Missing X-Forward-Host header. "
            "Set it to the target API host (e.g. api.openai.com)\"}");
        close(client_fd);
        return;
    }

    std::cout << CLR_CYAN << "  ▸ " << CLR_BOLD << req.method << " " << req.path
              << CLR_RESET << CLR_DIM << " → " << forward_host << CLR_RESET << std::endl;

    // ── Reconstruct outgoing HTTPS request ───────────────────────────────
    std::ostringstream out_req;
    out_req << req.method << " " << req.path << " HTTP/1.1\r\n";
    out_req << "Host: " << forward_host << "\r\n";

    for (const auto& hdr : req.headers) {
        std::string k_lower = hdr.first;
        std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
        // Skip headers we've already set or that shouldn't be forwarded
        if (k_lower == "host" || k_lower == "x-forward-host" || k_lower == "connection")
            continue;
        out_req << hdr.first << ": " << hdr.second << "\r\n";
    }
    out_req << "Connection: close\r\n";
    out_req << "\r\n";
    out_req << req.body;

    // ── Forward to real API via HTTPS ────────────────────────────────────
    std::string api_response = forwardHttps(forward_host, out_req.str());

    if (api_response.empty()) {
        sendResponse(client_fd, 502, "Bad Gateway", "application/json",
            "{\"error\": \"Failed to connect to upstream server\"}");
        close(client_fd);
        std::cout << CLR_RED << "  ✗ Failed to connect to " << forward_host << CLR_RESET << std::endl;
        return;
    }

    // ── Parse the response to extract token usage ────────────────────────
    size_t resp_header_end = api_response.find("\r\n\r\n");
    std::string resp_body;

    if (resp_header_end != std::string::npos) {
        std::string resp_headers_str = api_response.substr(0, resp_header_end);
        std::string resp_raw_body    = api_response.substr(resp_header_end + 4);

        // Decode chunked body if needed
        std::string lower_hdrs = resp_headers_str;
        std::transform(lower_hdrs.begin(), lower_hdrs.end(), lower_hdrs.begin(), ::tolower);

        if (lower_hdrs.find("transfer-encoding: chunked") != std::string::npos) {
            resp_body = decodeChunked(resp_raw_body);
        } else {
            resp_body = resp_raw_body;
        }
    }

    std::string model_name = extractJsonString(resp_body, "model");
    if (model_name.empty()) model_name = extractJsonString(req.body, "model");

    UsageCounts usage = extractUsageCounts(resp_body);
    int prompt_tokens     = usage.prompt_tokens;
    int completion_tokens = usage.completion_tokens;
    int total_tokens      = usage.total_tokens;

    // ── Carbon footprint estimation ──────────────────────────────────────
    // Energy (Wh) = (total_tokens / 1000) × ENERGY_PER_1K_TOKENS_WH
    // CO₂ (g)     = Energy (Wh) × CARBON_INTENSITY_G_PER_KWH / 1000
    double energy_wh = (total_tokens / 1000.0) * ENERGY_PER_1K_TOKENS_WH;
    double co2_grams = energy_wh * CARBON_INTENSITY_G_PER_KWH / 1000.0;

    // ── Update global stats ──────────────────────────────────────────────
    recordUsage("api-proxy", model_name, forward_host, req.path,
                prompt_tokens, completion_tokens, total_tokens);

    // ── Colored console log ──────────────────────────────────────────────
    std::cout << CLR_GREEN << "  ✓ " << CLR_RESET;
    if (!model_name.empty())
        std::cout << CLR_BOLD << model_name << CLR_RESET << " | ";
    if (!usage.found)
        std::cout << CLR_YELLOW << "No usage block returned | " << CLR_RESET;
    std::cout << CLR_CYAN << "Tokens: " << total_tokens
              << CLR_DIM << " (prompt:" << prompt_tokens
              << " + completion:" << completion_tokens << ")" << CLR_RESET
              << " | " << CLR_LEAF << "🌿 " << std::fixed << std::setprecision(4)
              << co2_grams << "g CO₂" << CLR_RESET
              << " | " << CLR_YELLOW << "⚡ " << std::setprecision(4)
              << energy_wh << " Wh" << CLR_RESET << std::endl;

    {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        std::cout << CLR_DIM << "    ↳ Session total: " << g_stats.total_tokens
                  << " tokens, " << std::setprecision(4) << g_stats.total_co2_grams
                  << "g CO₂ across " << g_stats.total_requests << " request(s)"
                  << CLR_RESET << std::endl;
    }

    // ── Return the original API response to the client ───────────────────
    send(client_fd, api_response.c_str(), api_response.size(), 0);
    close(client_fd);
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe from disconnected clients

    int proxy_port = configuredPort();

    // Create server socket
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only — safe
    addr.sin_port        = htons(proxy_port);

    if (bind(g_server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << CLR_RED << "Failed to bind to port " << proxy_port
                  << " — is another instance running?" << CLR_RESET << std::endl;
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, BACKLOG) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(g_server_fd);
        return 1;
    }

    // ── Startup banner ───────────────────────────────────────────────────
    std::cout << std::endl;
    std::cout << CLR_LEAF  << "  🌿 Carbon Token Proxy"                                << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  ────────────────────────────────────────"              << CLR_RESET << std::endl;
    std::cout << CLR_CYAN  << "  Listening on " << CLR_BOLD << "http://localhost:" << proxy_port << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  Stats:       http://localhost:" << proxy_port << "/stats"   << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  Health:      http://localhost:" << proxy_port << "/health"  << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  ────────────────────────────────────────"              << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  Set X-Forward-Host header to target API"              << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  Optional: CARBON_PROXY_PORT=" << proxy_port << " overrides the port" << CLR_RESET << std::endl;
    std::cout << CLR_DIM   << "  Press Ctrl+C to stop"                                 << CLR_RESET << std::endl;
    std::cout << std::endl;

    // ── Accept loop ──────────────────────────────────────────────────────
    while (g_running) {
        struct sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(g_server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!g_running) break; // expected during shutdown
            continue;
        }

        // Each connection handled in its own thread (simple & fine for a local proxy)
        std::thread(handleClient, client_fd).detach();
    }

    std::cout << CLR_GREEN << "  ✓ Proxy shut down cleanly." << CLR_RESET << std::endl;
    return 0;
}
