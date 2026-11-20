#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace hmi::server
{
class InferenceSession;

struct HttpOptions
{
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string bearer_token;
    std::string allowed_origin;
    std::size_t workers = 2;
    std::size_t queued_connections = 4;
};

class HttpServer
{
public:
    HttpServer(InferenceSession &session, HttpOptions options);
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    bool bind(std::string *error = nullptr);
    // Blocks until the listener stops and accepted requests finish.
    bool listen();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
