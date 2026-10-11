#include <winsock2.h>
#include <ws2tcpip.h>

#include "Http.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <thread>

namespace rm::http {
namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kMaxLineBytes = 16 * 1024;
constexpr size_t kMaxHeaderCount = 128;
constexpr int kCancelPollMs = 50;

bool EnsureWinsock() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

int Milliseconds(time_t seconds, time_t microseconds) {
    return static_cast<int>(std::clamp<long long>(
        static_cast<long long>(seconds) * 1000 + microseconds / 1000, 1, INT_MAX));
}

char Lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool EqualsIgnoreCase(const std::string& left, const char* right) {
    const size_t size = std::strlen(right);
    if (left.size() != size) return false;
    for (size_t index = 0; index < size; ++index)
        if (Lower(left[index]) != Lower(right[index])) return false;
    return true;
}

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

bool ParseUint64(const std::string& text, uint64_t& value) {
    if (text.empty() || text.size() > 20) return false;
    value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

std::string Header(const Headers& headers, const std::string& name) {
    auto found = headers.find(name);
    return found == headers.end() ? std::string{} : found->second;
}

std::string PercentDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        ch = Lower(ch);
        return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    };
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded.push_back(' ');
        } else if (value[index] == '%' && index + 2 < value.size() &&
                   hex(value[index + 1]) >= 0 && hex(value[index + 2]) >= 0) {
            decoded.push_back(static_cast<char>(hex(value[index + 1]) * 16 + hex(value[index + 2])));
            index += 2;
        } else {
            decoded.push_back(value[index]);
        }
    }
    return decoded;
}

void ParseQuery(const std::string& query, Params& params) {
    size_t begin = 0;
    while (begin < query.size()) {
        size_t end = query.find('&', begin);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(begin, end - begin);
        const size_t equals = pair.find('=');
        if (!pair.empty())
            params.emplace(PercentDecode(pair.substr(0, equals)),
                           equals == std::string::npos ? std::string{}
                                                       : PercentDecode(pair.substr(equals + 1)));
        begin = end + 1;
    }
}

const char* Reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Content";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default: return "Status";
    }
}

void SetNonBlocking(SOCKET socket) {
    u_long enabled = 1;
    ioctlsocket(socket, FIONBIO, &enabled);
}

// Buffered I/O over a non-blocking socket. Every wait is bounded by the read or
// write timeout and the optional overall deadline, and polls the cancellation
// predicate so another thread can abort a transfer within ~50 ms.
class Stream {
public:
    Stream(SOCKET socket, int readTimeoutMs, int writeTimeoutMs,
           std::function<bool()> cancelled, Clock::time_point deadline)
        : socket_(socket), readTimeoutMs_(readTimeoutMs), writeTimeoutMs_(writeTimeoutMs),
          cancelled_(std::move(cancelled)), deadline_(deadline), buffer_(64 * 1024) {}

    Error error = Error::Success;
    uint64_t received = 0;

    // Returns the number of bytes read, 0 at the end of the stream, -1 on failure.
    int Read(char* output, size_t capacity) {
        if (begin_ < end_) {
            const size_t count = std::min(capacity, end_ - begin_);
            std::memcpy(output, buffer_.data() + begin_, count);
            begin_ += count;
            return static_cast<int>(count);
        }
        return Receive(output, capacity);
    }

    bool ReadLine(std::string& line) {
        line.clear();
        for (;;) {
            while (begin_ < end_) {
                const char ch = buffer_[begin_++];
                if (ch == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                line.push_back(ch);
                if (line.size() > kMaxLineBytes) {
                    error = Error::InvalidResponse;
                    return false;
                }
            }
            const int count = Receive(buffer_.data(), buffer_.size());
            if (count <= 0) {
                if (count == 0 && error == Error::Success) error = Error::Read;
                return false;
            }
            begin_ = 0;
            end_ = static_cast<size_t>(count);
        }
    }

    bool Write(const char* data, size_t size) {
        while (size) {
            const int chunk = static_cast<int>(std::min<size_t>(size, 1024 * 1024));
            const int sent = send(socket_, data, chunk, 0);
            if (sent > 0) {
                data += sent;
                size -= static_cast<size_t>(sent);
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                error = Error::Write;
                return false;
            }
            if (!Wait(true, writeTimeoutMs_)) return false;
        }
        return true;
    }

    bool Write(const std::string& text) { return Write(text.data(), text.size()); }

private:
    int Receive(char* output, size_t capacity) {
        for (;;) {
            const int count = recv(socket_, output,
                static_cast<int>(std::min<size_t>(capacity, INT_MAX)), 0);
            if (count >= 0) {
                received += static_cast<uint64_t>(count);
                return count;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                error = Error::Read;
                return -1;
            }
            if (!Wait(false, readTimeoutMs_)) return -1;
        }
    }

    bool Wait(bool write, int timeoutMs) {
        auto limit = Clock::now() + std::chrono::milliseconds(timeoutMs);
        if (deadline_ != Clock::time_point{} && deadline_ < limit) limit = deadline_;
        for (;;) {
            if (cancelled_ && cancelled_()) {
                error = Error::Canceled;
                return false;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                limit - Clock::now()).count();
            if (remaining <= 0) {
                error = write ? Error::Write : Error::Read;
                return false;
            }
            const long slice = static_cast<long>(std::min<long long>(remaining, kCancelPollMs));
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(socket_, &ready);
            fd_set failed;
            FD_ZERO(&failed);
            FD_SET(socket_, &failed);
            timeval wait{slice / 1000, (slice % 1000) * 1000};
            const int state = select(0, write ? nullptr : &ready, write ? &ready : nullptr,
                                     &failed, &wait);
            if (state > 0) return true;
            if (state < 0) {
                error = write ? Error::Write : Error::Read;
                return false;
            }
        }
    }

    SOCKET socket_;
    int readTimeoutMs_;
    int writeTimeoutMs_;
    std::function<bool()> cancelled_;
    Clock::time_point deadline_;
    std::vector<char> buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
};

bool ReadHeaders(Stream& stream, Headers& headers) {
    std::string line;
    for (size_t count = 0;; ++count) {
        if (!stream.ReadLine(line)) return false;
        if (line.empty()) return true;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || count >= kMaxHeaderCount) {
            stream.error = Error::InvalidResponse;
            return false;
        }
        headers.emplace(line.substr(0, colon), Trim(line.substr(colon + 1)));
    }
}

// Reads a body framed by Content-Length, chunked encoding or connection close.
bool ReadBody(Stream& stream, const Headers& headers, const ContentReceiver& receiver,
              bool& reusable) {
    std::vector<char> buffer(64 * 1024);
    auto copy = [&](uint64_t remaining) {
        while (remaining) {
            const int count = stream.Read(
                buffer.data(), static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining)));
            if (count <= 0) return false;
            remaining -= static_cast<uint64_t>(count);
            if (!receiver(buffer.data(), static_cast<size_t>(count))) {
                stream.error = Error::Canceled;
                return false;
            }
        }
        return true;
    };
    const std::string encoding = Header(headers, "Transfer-Encoding");
    if (!encoding.empty() && !EqualsIgnoreCase(encoding, "identity")) {
        std::string line;
        for (;;) {
            if (!stream.ReadLine(line)) return false;
            const std::string sizeText = line.substr(0, line.find(';'));
            uint64_t size = 0;
            if (sizeText.empty() || sizeText.size() > 16 ||
                sizeText.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                stream.error = Error::InvalidResponse;
                return false;
            }
            size = std::stoull(sizeText, nullptr, 16);
            if (size == 0) {
                while (stream.ReadLine(line) && !line.empty()) {
                }
                return stream.error == Error::Success;
            }
            if (!copy(size) || !stream.ReadLine(line)) return false;
        }
    }
    const std::string lengthText = Header(headers, "Content-Length");
    if (!lengthText.empty()) {
        uint64_t length = 0;
        if (!ParseUint64(lengthText, length)) {
            stream.error = Error::InvalidResponse;
            return false;
        }
        return copy(length);
    }
    reusable = false;
    for (;;) {
        const int count = stream.Read(buffer.data(), buffer.size());
        if (count == 0) return true;
        if (count < 0) return false;
        if (!receiver(buffer.data(), static_cast<size_t>(count))) {
            stream.error = Error::Canceled;
            return false;
        }
    }
}

}  // namespace

bool CaseInsensitiveLess::operator()(const std::string& left, const std::string& right) const {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [](char a, char b) { return Lower(a) < Lower(b); });
}

bool Request::has_header(const std::string& name) const { return headers.contains(name); }
std::string Request::get_header_value(const std::string& name) const {
    return Header(headers, name);
}
bool Request::has_param(const std::string& name) const { return params.contains(name); }
std::string Request::get_param_value(const std::string& name) const {
    auto found = params.find(name);
    return found == params.end() ? std::string{} : found->second;
}

std::string Response::get_header_value(const std::string& name) const {
    return Header(headers, name);
}
uint64_t Response::get_header_value_u64(const std::string& name) const {
    uint64_t value = 0;
    return ParseUint64(Header(headers, name), value) ? value : 0;
}
void Response::set_header(const std::string& name, const std::string& value) {
    headers.erase(name);
    headers.emplace(name, value);
}
void Response::set_content(std::string content, const std::string& contentType) {
    body = std::move(content);
    set_header("Content-Type", contentType);
}

std::string to_string(Error error) {
    switch (error) {
        case Error::Success: return "Success";
        case Error::Connection: return "Could not establish connection";
        case Error::Read: return "Failed to read connection";
        case Error::Write: return "Failed to write connection";
        case Error::Canceled: return "Connection handling canceled";
        case Error::Timeout: return "Connection timed out";
        case Error::InvalidResponse: return "Invalid HTTP message";
        case Error::Unknown: break;
    }
    return "Unknown error";
}

// ---------------------------------------------------------------- Server

Server::Server() : listener_(INVALID_SOCKET) { EnsureWinsock(); }

Server::~Server() { stop(); }

void Server::Get(std::string path, Handler handler) {
    get_[std::move(path)] = {std::move(handler), {}};
}
void Server::Post(std::string path, Handler handler) {
    post_[std::move(path)] = {std::move(handler), {}};
}
void Server::Post(std::string path, StreamHandler handler) {
    post_[std::move(path)] = {{}, std::move(handler)};
}
void Server::set_read_timeout(time_t seconds, time_t microseconds) {
    readTimeoutMs_ = Milliseconds(seconds, microseconds);
}
void Server::set_write_timeout(time_t seconds, time_t microseconds) {
    writeTimeoutMs_ = Milliseconds(seconds, microseconds);
}

bool Server::bind_to_port(const std::string& host, int port) {
    if (!EnsureWinsock()) return false;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(socket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(socket);
        return false;
    }
    stop();
    listener_ = socket;
    running_ = true;
    return true;
}

int Server::bind_to_any_port(const std::string& host) {
    if (!bind_to_port(host, 0)) return -1;
    sockaddr_in address{};
    int size = sizeof(address);
    if (getsockname(static_cast<SOCKET>(listener_.load()),
                    reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR)
        return -1;
    return ntohs(address.sin_port);
}

bool Server::listen_after_bind() {
    while (running_.load()) {
        sockaddr_in remote{};
        int size = sizeof(remote);
        const SOCKET client = accept(static_cast<SOCKET>(listener_.load()),
                                     reinterpret_cast<sockaddr*>(&remote), &size);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        char text[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &remote.sin_addr, text, sizeof(text));
        {
            std::scoped_lock lock(connectionsMutex_);
            if (connections_.size() >= 256 || !running_.load()) {
                closesocket(client);
                continue;
            }
            connections_.insert(client);
        }
        std::thread([this, client, address = std::string(text)] {
            Serve(client, address);
            closesocket(client);
            std::scoped_lock lock(connectionsMutex_);
            connections_.erase(client);
            connectionsChanged_.notify_all();
        }).detach();
    }
    return true;
}

void Server::stop() {
    running_ = false;
    const uintptr_t listener = listener_.exchange(INVALID_SOCKET);
    if (listener != INVALID_SOCKET) closesocket(static_cast<SOCKET>(listener));
    std::unique_lock lock(connectionsMutex_);
    for (uintptr_t connection : connections_)
        shutdown(static_cast<SOCKET>(connection), SD_BOTH);
    connectionsChanged_.wait(lock, [&] { return connections_.empty(); });
}

void Server::Serve(uintptr_t socketValue, std::string remoteAddress) {
    const SOCKET socket = static_cast<SOCKET>(socketValue);
    SetNonBlocking(socket);
    BOOL noDelay = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
               sizeof(noDelay));
    Stream stream(socket, readTimeoutMs_, writeTimeoutMs_,
                  [this] { return !running_.load(); }, {});
    for (;;) {
        std::string line;
        if (!stream.ReadLine(line)) return;
        if (line.empty()) continue;
        Request request;
        request.remote_addr = remoteAddress;
        const size_t methodEnd = line.find(' ');
        const size_t targetEnd = line.rfind(' ');
        if (methodEnd == std::string::npos || targetEnd <= methodEnd) return;
        request.method = line.substr(0, methodEnd);
        const std::string target = line.substr(methodEnd + 1, targetEnd - methodEnd - 1);
        const std::string version = line.substr(targetEnd + 1);
        const size_t query = target.find('?');
        request.path = target.substr(0, query);
        if (query != std::string::npos) ParseQuery(target.substr(query + 1), request.params);
        if (!ReadHeaders(stream, request.headers)) return;

        const std::string connection = request.get_header_value("Connection");
        bool keepAlive = version == "HTTP/1.1" ? !EqualsIgnoreCase(connection, "close")
                                               : EqualsIgnoreCase(connection, "keep-alive");
        uint64_t remaining = 0;
        const std::string lengthText = request.get_header_value("Content-Length");
        Response response;
        bool valid = lengthText.empty() || ParseUint64(lengthText, remaining);
        const std::string encoding = request.get_header_value("Transfer-Encoding");
        if (!encoding.empty() && !EqualsIgnoreCase(encoding, "identity")) {
            valid = false;
            response.status = 411;
        } else if (!valid) {
            response.status = 400;
        }
        const auto& routes = request.method == "GET" ? get_ : post_;
        auto route = routes.find(request.path);
        if (valid && (route == routes.end() ||
                      (request.method != "GET" && request.method != "POST"))) {
            response.status = 404;
        } else if (valid && route->second.handler) {
            if (remaining > maxBufferedBody_) {
                response.status = 413;
            } else {
                request.body.resize(static_cast<size_t>(remaining));
                size_t offset = 0;
                while (remaining) {
                    const int count = stream.Read(request.body.data() + offset,
                                                  static_cast<size_t>(remaining));
                    if (count <= 0) return;
                    offset += static_cast<size_t>(count);
                    remaining -= static_cast<uint64_t>(count);
                }
                try {
                    route->second.handler(request, response);
                } catch (...) {
                    response = {};
                    response.status = 500;
                }
            }
        } else if (valid) {
            std::vector<char> buffer;
            ContentReader reader = [&](const ContentReceiver& receiver) {
                if (buffer.empty()) buffer.resize(64 * 1024);
                while (remaining) {
                    const int count = stream.Read(
                        buffer.data(),
                        static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining)));
                    if (count <= 0) return false;
                    remaining -= static_cast<uint64_t>(count);
                    if (!receiver(buffer.data(), static_cast<size_t>(count))) return false;
                }
                return true;
            };
            try {
                route->second.stream(request, response, reader);
            } catch (...) {
                response = {};
                response.status = 500;
            }
        }
        // An unread request body would be parsed as the next request.
        if (!valid || remaining) keepAlive = false;
        std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
            Reason(response.status) + "\r\n";
        for (const auto& [name, value] : response.headers) {
            if (EqualsIgnoreCase(name, "Content-Length") || EqualsIgnoreCase(name, "Connection"))
                continue;
            head += name + ": " + value + "\r\n";
        }
        head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        if (!stream.Write(head) || !stream.Write(response.body) || !keepAlive) return;
    }
}

// ---------------------------------------------------------------- Client

struct Client::Outgoing {
    const char* method = "GET";
    const std::string* path = nullptr;
    const Headers* headers = nullptr;
    const std::string* body = nullptr;
    size_t length = 0;
    const ContentProvider* provider = nullptr;
    const std::string* contentType = nullptr;
    const ResponseHandler* onResponse = nullptr;
    const ContentReceiver* onContent = nullptr;
};

Client::Client(std::string host, int port)
    : host_(std::move(host)), port_(port), socket_(INVALID_SOCKET) {
    EnsureWinsock();
}

Client::~Client() { Disconnect(); }

void Client::set_connection_timeout(time_t seconds, time_t microseconds) {
    connectTimeoutMs_ = Milliseconds(seconds, microseconds);
}
void Client::set_read_timeout(time_t seconds, time_t microseconds) {
    readTimeoutMs_ = Milliseconds(seconds, microseconds);
}
void Client::set_write_timeout(time_t seconds, time_t microseconds) {
    writeTimeoutMs_ = Milliseconds(seconds, microseconds);
}
void Client::set_cancellation(const std::atomic<bool>* first, const std::atomic<bool>* second) {
    cancelFirst_ = first;
    cancelSecond_ = second;
}

bool Client::Cancelled() const {
    return stopped_.load() || (cancelFirst_ && cancelFirst_->load()) ||
        (cancelSecond_ && cancelSecond_->load());
}

void Client::stop() {
    stopped_ = true;
    const uintptr_t socket = socket_.load();
    if (socket != INVALID_SOCKET) shutdown(static_cast<SOCKET>(socket), SD_BOTH);
}

void Client::Disconnect() {
    const uintptr_t socket = socket_.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) closesocket(static_cast<SOCKET>(socket));
}

Result Client::Get(const std::string& path, const Headers& headers) {
    Outgoing request;
    request.path = &path;
    request.headers = &headers;
    return Send(request);
}

Result Client::Get(const std::string& path, const Headers& headers,
                   ResponseHandler onResponse, ContentReceiver onContent) {
    Outgoing request;
    request.path = &path;
    request.headers = &headers;
    request.onResponse = &onResponse;
    request.onContent = &onContent;
    return Send(request);
}

Result Client::Post(const std::string& path, const Headers& headers,
                    const std::string& body, const std::string& contentType) {
    Outgoing request;
    request.method = "POST";
    request.path = &path;
    request.headers = &headers;
    request.body = &body;
    request.length = body.size();
    request.contentType = &contentType;
    return Send(request);
}

Result Client::Post(const std::string& path, const Headers& headers, size_t length,
                    ContentProvider provider, const std::string& contentType) {
    Outgoing request;
    request.method = "POST";
    request.path = &path;
    request.headers = &headers;
    request.length = length;
    request.provider = &provider;
    request.contentType = &contentType;
    return Send(request);
}

Result Client::Send(const Outgoing& request) {
    if (Cancelled()) return {nullptr, Error::Canceled};
    bool retryable = false;
    Result result = SendOnce(request, retryable);
    // A reused keep-alive connection may have been closed by the server while
    // idle. Repeat once on a fresh connection when nothing was received.
    if (!result && retryable && !Cancelled()) result = SendOnce(request, retryable);
    return result;
}

Result Client::SendOnce(const Outgoing& request, bool& retryable) {
    retryable = false;
    const auto started = Clock::now();
    const Clock::time_point deadline = maxTimeoutMs_ > 0
        ? started + std::chrono::milliseconds(maxTimeoutMs_) : Clock::time_point{};
    const bool reused = socket_.load() != INVALID_SOCKET;
    if (!reused) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &found) != 0 ||
            !found)
            return {nullptr, Error::Connection};
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            freeaddrinfo(found);
            return {nullptr, Error::Connection};
        }
        SetNonBlocking(socket);
        BOOL noDelay = TRUE;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                   sizeof(noDelay));
        const int connected = connect(socket, found->ai_addr, static_cast<int>(found->ai_addrlen));
        freeaddrinfo(found);
        socket_ = socket;
        if (connected == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            Disconnect();
            return {nullptr, Error::Connection};
        }
        Error connectError = Error::Success;
        const auto limit = Clock::now() + std::chrono::milliseconds(connectTimeoutMs_);
        while (connected == SOCKET_ERROR) {
            if (Cancelled()) {
                connectError = Error::Canceled;
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                limit - Clock::now()).count();
            if (remaining <= 0) {
                connectError = Error::Timeout;
                break;
            }
            const long slice = static_cast<long>(std::min<long long>(remaining, kCancelPollMs));
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            fd_set failed;
            FD_ZERO(&failed);
            FD_SET(socket, &failed);
            timeval wait{slice / 1000, (slice % 1000) * 1000};
            const int state = select(0, nullptr, &writable, &failed, &wait);
            if (state < 0 || FD_ISSET(socket, &failed)) {
                connectError = Error::Connection;
                break;
            }
            if (state > 0) break;
        }
        if (connectError != Error::Success) {
            Disconnect();
            return {nullptr, connectError};
        }
    }

    const SOCKET socket = static_cast<SOCKET>(socket_.load());
    Stream stream(socket, readTimeoutMs_, writeTimeoutMs_, [this] { return Cancelled(); },
                  deadline);
    auto fail = [&](bool allowRetry) -> Result {
        retryable = allowRetry && reused && stream.received == 0 &&
            stream.error != Error::Canceled;
        const Error error = stream.error == Error::Success ? Error::Unknown : stream.error;
        Disconnect();
        return {nullptr, error};
    };

    std::string head = std::string(request.method) + " " + *request.path + " HTTP/1.1\r\n";
    head += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    for (const auto& [name, value] : *request.headers) {
        if (EqualsIgnoreCase(name, "Content-Length") || EqualsIgnoreCase(name, "Connection") ||
            EqualsIgnoreCase(name, "Host"))
            continue;
        head += name + ": " + value + "\r\n";
    }
    if (std::strcmp(request.method, "POST") == 0) {
        if (request.contentType && !request.headers->contains("Content-Type"))
            head += "Content-Type: " + *request.contentType + "\r\n";
        head += "Content-Length: " + std::to_string(request.length) + "\r\n";
    }
    head += keepAlive_ ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (!stream.Write(head)) return fail(true);
    if (request.body) {
        if (!stream.Write(*request.body)) return fail(true);
    } else if (request.provider) {
        size_t offset = 0;
        bool sinkFailed = false;
        DataSink sink;
        sink.write = [&](const char* data, size_t size) {
            if (size > request.length - offset || !stream.Write(data, size)) {
                sinkFailed = true;
                return false;
            }
            offset += size;
            return true;
        };
        while (offset < request.length) {
            const size_t before = offset;
            if (!(*request.provider)(offset, request.length - offset, sink) || sinkFailed ||
                offset == before) {
                if (stream.error == Error::Success)
                    stream.error = Cancelled() ? Error::Canceled : Error::Write;
                return fail(true);
            }
        }
    }

    std::string line;
    if (!stream.ReadLine(line)) return fail(true);
    auto response = std::make_unique<Response>();
    const size_t firstSpace = line.find(' ');
    uint64_t status = 0;
    if (line.rfind("HTTP/1.", 0) != 0 || firstSpace == std::string::npos ||
        !ParseUint64(line.substr(firstSpace + 1, 3), status) || status < 100 || status > 999) {
        stream.error = Error::InvalidResponse;
        return fail(false);
    }
    response->status = static_cast<int>(status);
    if (!ReadHeaders(stream, response->headers)) return fail(false);
    if (request.onResponse && *request.onResponse && !(*request.onResponse)(*response)) {
        Disconnect();
        return {nullptr, Error::Canceled};
    }
    bool reusable = keepAlive_ && line.rfind("HTTP/1.1", 0) == 0 &&
        !EqualsIgnoreCase(response->get_header_value("Connection"), "close");
    const bool streamed = request.onContent && *request.onContent;
    Response* target = response.get();
    const ContentReceiver receiver = streamed
        ? *request.onContent
        : ContentReceiver([target](const char* data, size_t size) {
              target->body.append(data, size);
              return true;
          });
    if (!ReadBody(stream, response->headers, receiver, reusable)) return fail(false);
    if (!reusable) Disconnect();
    return {std::move(response), Error::Success};
}

}  // namespace rm::http
