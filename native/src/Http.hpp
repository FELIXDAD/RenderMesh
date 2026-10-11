#pragma once

// Minimal HTTP/1.1 client and server for the RenderMesh LAN protocol.
//
// It implements exactly what the node protocol needs: literal routes, query
// parameters, Content-Length bodies (streamed in both directions), keep-alive
// and cooperative cancellation. Its wire format stays compatible with peers
// that still run the earlier cpp-httplib based releases.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rm::http {

struct CaseInsensitiveLess {
    bool operator()(const std::string& left, const std::string& right) const;
};
using Headers = std::multimap<std::string, std::string, CaseInsensitiveLess>;
using Params = std::multimap<std::string, std::string>;

struct Request {
    std::string method;
    std::string path;
    std::string remote_addr;
    std::string body;
    Headers headers;
    Params params;

    bool has_header(const std::string& name) const;
    std::string get_header_value(const std::string& name) const;
    bool has_param(const std::string& name) const;
    std::string get_param_value(const std::string& name) const;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;

    std::string get_header_value(const std::string& name) const;
    uint64_t get_header_value_u64(const std::string& name) const;
    void set_header(const std::string& name, const std::string& value);
    void set_content(std::string content, const std::string& contentType);
};

using ContentReceiver = std::function<bool(const char* data, size_t length)>;
using ContentReader = std::function<bool(const ContentReceiver& receiver)>;
using Handler = std::function<void(const Request&, Response&)>;
using StreamHandler = std::function<void(const Request&, Response&, const ContentReader&)>;

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void Get(std::string path, Handler handler);
    void Post(std::string path, Handler handler);
    void Post(std::string path, StreamHandler handler);
    void set_read_timeout(time_t seconds, time_t microseconds = 0);
    void set_write_timeout(time_t seconds, time_t microseconds = 0);
    // Upper bound for bodies that are buffered into Request::body.
    void set_payload_max_length(uint64_t bytes) { maxBufferedBody_ = bytes; }

    bool bind_to_port(const std::string& host, int port);
    int bind_to_any_port(const std::string& host);
    bool listen_after_bind();
    void stop();

private:
    struct Route {
        Handler handler;
        StreamHandler stream;
    };
    void Serve(uintptr_t socket, std::string remoteAddress);

    std::map<std::string, Route> get_;
    std::map<std::string, Route> post_;
    std::atomic<uintptr_t> listener_;
    std::atomic<bool> running_{false};
    int readTimeoutMs_ = 5000;
    int writeTimeoutMs_ = 5000;
    uint64_t maxBufferedBody_ = 64ull * 1024 * 1024;
    std::mutex connectionsMutex_;
    std::condition_variable connectionsChanged_;
    std::set<uintptr_t> connections_;
};

enum class Error {
    Success,
    Unknown,
    Connection,
    Read,
    Write,
    Canceled,
    Timeout,
    InvalidResponse,
};
std::string to_string(Error error);

class Result {
public:
    Result() = default;
    Result(std::unique_ptr<Response> response, Error error)
        : response_(std::move(response)), error_(error) {}
    explicit operator bool() const { return response_ != nullptr; }
    const Response& operator*() const { return *response_; }
    const Response* operator->() const { return response_.get(); }
    Error error() const { return error_; }

private:
    std::unique_ptr<Response> response_;
    Error error_ = Error::Unknown;
};

struct DataSink {
    std::function<bool(const char* data, size_t length)> write;
};
using ContentProvider = std::function<bool(size_t offset, size_t length, DataSink& sink)>;
using ResponseHandler = std::function<bool(const Response& response)>;

class Client {
public:
    Client(std::string host, int port);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void set_connection_timeout(time_t seconds, time_t microseconds = 0);
    void set_read_timeout(time_t seconds, time_t microseconds = 0);
    void set_write_timeout(time_t seconds, time_t microseconds = 0);
    // Overall deadline for one request, in milliseconds (0 = none).
    void set_max_timeout(time_t milliseconds) { maxTimeoutMs_ = milliseconds; }
    // Reuse the connection for the next request when the server allows it.
    void set_keep_alive(bool enabled) { keepAlive_ = enabled; }
    // Requests fail with Error::Canceled shortly after either flag turns true.
    void set_cancellation(const std::atomic<bool>* first,
                          const std::atomic<bool>* second = nullptr);

    Result Get(const std::string& path, const Headers& headers = {});
    Result Get(const std::string& path, const Headers& headers,
               ResponseHandler onResponse, ContentReceiver onContent);
    Result Post(const std::string& path, const Headers& headers,
                const std::string& body, const std::string& contentType);
    Result Post(const std::string& path, const Headers& headers, size_t length,
                ContentProvider provider, const std::string& contentType);
    // Aborts a request that is running on another thread.
    void stop();

private:
    struct Outgoing;
    Result Send(const Outgoing& request);
    Result SendOnce(const Outgoing& request, bool& retryable);
    bool Cancelled() const;
    void Disconnect();

    std::string host_;
    int port_ = 0;
    int connectTimeoutMs_ = 3000;
    int readTimeoutMs_ = 5000;
    int writeTimeoutMs_ = 5000;
    time_t maxTimeoutMs_ = 0;
    bool keepAlive_ = false;
    const std::atomic<bool>* cancelFirst_ = nullptr;
    const std::atomic<bool>* cancelSecond_ = nullptr;
    std::atomic<bool> stopped_{false};
    std::atomic<uintptr_t> socket_;
};

}  // namespace rm::http
