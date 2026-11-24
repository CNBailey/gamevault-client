// GameVault for PS5 - Plain HTTP for the native app, and its background workers.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every request blocks: run them on a Workers thread, never in update().

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

namespace gv
{

struct HttpResult
{
    bool connected = false; // false: `error` says why
    int status = 0;
    std::string body;
    std::string error;

    bool ok() const
    {
        return connected && status >= 200 && status < 300;
    }
};

// One request per connection. url: http://host[:port]/path (IPv4 addresses
// work everywhere; names only where the system can look them up).
HttpResult http(const std::string &method, const std::string &url, const std::string &body = {},
                int timeout_seconds = 10, std::size_t max_body = 16u * 1024u * 1024u);

// Sends a file to 127.0.0.1:port and closes (how a payload reaches the ELF loader).
bool send_file_to_local_port(const std::string &path, int port, std::string *error);

// Percent-encodes a path segment.
std::string url_encode(const std::string &value);

// A few background threads running queued jobs, and a mailbox of results for
// the UI thread (drained by run_posted() in update()).
class Workers
{
  public:
    explicit Workers(int threads);
    ~Workers();
    Workers(const Workers &) = delete;
    Workers &operator=(const Workers &) = delete;

    void run(std::function<void()> job);
    // Called from a job: `done` runs later on the UI thread.
    void post(std::function<void()> done);
    // UI thread: runs what the jobs posted.
    void run_posted();
    std::size_t pending() const;

  private:
    static void *thread_main(void *self);
    void loop();

    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::function<void()>> posted_;
    std::vector<pthread_t> threads_;
    int busy_ = 0;
    bool stopping_ = false;
};

} // namespace gv
