// GameVault for PS5 - Plain HTTP for the native app, and its background workers.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/gv_net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace gv
{

namespace
{

struct Url
{
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parse_url(const std::string &url, Url *out)
{
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::string rest = url.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out->path = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::size_t colon = hostport.find(':');
    if (colon != std::string::npos)
    {
        out->port = std::atoi(hostport.c_str() + colon + 1);
        hostport.resize(colon);
    }
    out->host = hostport;
    return !out->host.empty() && out->port > 0 && out->port < 65536;
}

int connect_to(const Url &u, int timeout_seconds, std::string *error)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(u.port));
    if (inet_pton(AF_INET, u.host.c_str(), &addr.sin_addr) != 1)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *found = nullptr;
        if (getaddrinfo(u.host.c_str(), nullptr, &hints, &found) != 0 || !found)
        {
            *error = "Can't find \"" + u.host + "\": use the NAS's IP address";
            return -1;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr;
        freeaddrinfo(found);
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    timeval tv{timeout_seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0)
    {
        *error = "Can't connect to " + u.host + ":" + std::to_string(u.port) + " (" + std::strerror(errno) + ")";
        close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const char *p, std::size_t n)
{
    while (n)
    {
        const ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

bool ieq_prefix(const char *s, const char *prefix)
{
    for (; *prefix; ++s, ++prefix)
    {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

} // namespace

HttpResult http(const std::string &method, const std::string &url, const std::string &body,
                int timeout_seconds, std::size_t max_body)
{
    HttpResult result;
    Url u;
    if (!parse_url(url, &u))
    {
        result.error = "Not an http:// address: " + url;
        return result;
    }
    const int fd = connect_to(u, timeout_seconds, &result.error);
    if (fd < 0)
        return result;
    result.connected = true;

    std::string request = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" +
                          std::to_string(u.port) +
                          "\r\nUser-Agent: GameVault-PS5-App\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (!body.empty() || method == "POST" || method == "PUT")
        request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n";
    request += body;
    if (!send_all(fd, request.data(), request.size()))
    {
        result.error = "Couldn't send the request";
        close(fd);
        return result;
    }

    std::string raw;
    char buf[65536];
    for (;;)
    {
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        raw.append(buf, static_cast<std::size_t>(n));
        if (raw.size() > max_body + 65536)
        {
            result.error = "The answer was too large";
            break;
        }
    }
    close(fd);

    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        if (result.error.empty())
            result.error = raw.empty() ? "No answer" : "An incomplete answer";
        return result;
    }
    std::sscanf(raw.c_str(), "HTTP/%*s %d", &result.status);
    bool chunked = false;
    for (std::size_t line = raw.find("\r\n"); line < header_end; line = raw.find("\r\n", line + 2))
    {
        const char *h = raw.c_str() + line + 2;
        if (ieq_prefix(h, "transfer-encoding:") && std::strstr(h, "chunked") && std::strstr(h, "chunked") < raw.c_str() + header_end)
            chunked = true;
    }
    std::string data = raw.substr(header_end + 4);
    if (chunked)
    {
        std::string decoded;
        std::size_t pos = 0;
        while (pos < data.size())
        {
            const long size = std::strtol(data.c_str() + pos, nullptr, 16);
            const std::size_t crlf = data.find("\r\n", pos);
            if (crlf == std::string::npos || size <= 0)
                break;
            pos = crlf + 2;
            decoded.append(data, pos, static_cast<std::size_t>(size));
            pos += static_cast<std::size_t>(size) + 2;
        }
        data.swap(decoded);
    }
    result.body = std::move(data);
    if (!result.ok() && result.error.empty())
    {
        result.error = "HTTP " + std::to_string(result.status);
        const std::size_t e = result.body.find("\"error\":\"");
        if (e != std::string::npos)
        {
            const std::size_t end = result.body.find('"', e + 9);
            if (end != std::string::npos)
                result.error = result.body.substr(e + 9, end - e - 9);
        }
    }
    return result;
}

bool send_file_to_local_port(const std::string &path, int port, std::string *error)
{
    const int file = open(path.c_str(), O_RDONLY);
    if (file < 0)
    {
        *error = "Can't read " + path;
        return false;
    }
    Url u;
    u.host = "127.0.0.1";
    u.port = port;
    const int fd = connect_to(u, 10, error);
    if (fd < 0)
    {
        close(file);
        return false;
    }
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    for (;;)
    {
        const ssize_t n = read(file, buf.data(), buf.size());
        if (n < 0)
        {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (!send_all(fd, buf.data(), static_cast<std::size_t>(n)))
        {
            ok = false;
            break;
        }
    }
    close(file);
    close(fd);
    if (!ok)
        *error = "The ELF loader stopped reading";
    return ok;
}

std::string url_encode(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("-_.~/", c))
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ---- Workers --------------------------------------------------------------------

Workers::Workers(int threads)
{
    for (int i = 0; i < threads; ++i)
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 1024 * 1024);
        pthread_t thread;
        if (pthread_create(&thread, &attributes, &Workers::thread_main, this) == 0)
            threads_.push_back(thread);
        pthread_attr_destroy(&attributes);
    }
}

Workers::~Workers()
{
    {
        std::lock_guard<std::mutex> hold(lock_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (pthread_t t : threads_)
        pthread_join(t, nullptr);
}

void *Workers::thread_main(void *self)
{
    static_cast<Workers *>(self)->loop();
    return nullptr;
}

void Workers::loop()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> hold(lock_);
            wake_.wait(hold, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++busy_;
        }
        job();
        std::lock_guard<std::mutex> hold(lock_);
        --busy_;
    }
}

void Workers::run(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> hold(lock_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Workers::post(std::function<void()> done)
{
    std::lock_guard<std::mutex> hold(lock_);
    posted_.push_back(std::move(done));
}

void Workers::run_posted()
{
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> hold(lock_);
        ready.swap(posted_);
    }
    for (auto &f : ready)
        f();
}

std::size_t Workers::pending() const
{
    std::lock_guard<std::mutex> hold(lock_);
    return jobs_.size() + static_cast<std::size_t>(busy_);
}

} // namespace gv
