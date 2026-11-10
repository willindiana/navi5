/* Navi5 — real loopback listener checks, synthetic credentials only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing_listener.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <thread>
#include <iostream>
using namespace std::chrono_literals;
namespace {
struct Running {
    pairing::Listener listener;
    std::thread worker;
    std::string url, authority, path;
    sockaddr_in endpoint{};
    Running(pairing::Session::Validator validator = [](const auto &, subsonic::LoginFailure &) { return true; }, int ttl = 20) {
        worker = std::thread([this, validator, ttl] { assert(listener.run(validator, "127.0.0.1", false, std::chrono::seconds(ttl))); });
        for (int i = 0; i < 1000 && url.empty(); ++i) { url = listener.url(); std::this_thread::sleep_for(2ms); }
        assert(!url.empty());
        auto ready = listener.ready(); assert(ready && ready->url == url);
        assert(ready->deadline == listener.ready()->deadline);
        assert(listener.status() == pairing::Listener::Status::Running);
        auto slash = url.find('/', 7);
        authority = url.substr(7, slash - 7); path = url.substr(slash);
        endpoint.sin_family = AF_INET; endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(std::stoi(authority.substr(authority.find(':') + 1)));
    }
    ~Running() { listener.cancel(); if (worker.joinable()) worker.join(); }
    int connect_client() {
        int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
        timeval timeout{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        assert(connect(fd, reinterpret_cast<sockaddr *>(&endpoint), sizeof endpoint) == 0); return fd;
    }
    void no_listener() {
        int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
        assert(connect(fd, reinterpret_cast<sockaddr *>(&endpoint), sizeof endpoint) != 0); close(fd);
    }
    void closed() {
        worker.join(); no_listener();
        assert(listener.status() == pairing::Listener::Status::Finished);
        assert(!listener.ready());
    }
    std::string get() { return "GET " + path + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n"; }
    std::string post() {
        std::string body = "csrf=" + path.substr(6) + "&consent=yes&server=https%3A%2F%2Fexample.invalid&username=fake&password=fake&api_key=";
        return "POST " + path + " HTTP/1.1\r\nHost: " + authority + "\r\nOrigin: http://" + authority +
            "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    std::string exchange(const std::string &request) {
        int fd = connect_client();
        assert(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
        std::string response; char bytes[2048]; ssize_t n;
        while ((n = recv(fd, bytes, sizeof bytes, 0)) > 0) response.append(bytes, n);
        close(fd); return response;
    }
};
}
int main() {
    { pairing::Listener l; assert(!l.run([](const auto &, subsonic::LoginFailure &) { return true; }, "0.0.0.0")); assert(l.url().empty()); }
    { pairing::Listener l; assert(!l.run([](const auto &, subsonic::LoginFailure &) { return true; }, "192.168.1.1")); }
    { Running r; assert(r.exchange(r.get()).starts_with("HTTP/1.1 200"));
      assert(r.exchange(r.post()).starts_with("HTTP/1.1 200")); r.closed();
      auto c = r.listener.take(); assert(c && c->username == "fake"); assert(!r.listener.take()); }
    { int calls = 0; Running r([&](const auto &, subsonic::LoginFailure &f) {
          f = subsonic::LoginFailure::Authentication; return ++calls == 2;
      });
      const auto deadline = r.listener.ready()->deadline;
      auto failed = r.exchange(r.post());
      assert(failed.starts_with("HTTP/1.1 403") && failed.find("Wrong username or password.") != std::string::npos);
      assert(r.listener.url() == r.url && r.listener.ready()->deadline == deadline);
      assert(r.exchange(r.get()).starts_with("HTTP/1.1 200"));
      assert(r.exchange(r.post()).starts_with("HTTP/1.1 200")); r.closed(); assert(r.listener.take()); }
    { Running r([](const auto &, subsonic::LoginFailure &) { return false; }); for (int i = 0; i < 5; ++i) assert(r.exchange(r.post()).starts_with("HTTP/1.1 403")); r.closed(); assert(!r.listener.take()); }
    { Running r([](const auto &, subsonic::LoginFailure &) -> bool { throw 1; }); for (int i = 0; i < 5; ++i) assert(r.exchange(r.post()).starts_with("HTTP/1.1 403")); r.closed(); assert(!r.listener.take()); }
    { Running r; for (const auto &tail : {std::string("Content-Length: 4097\r\n\r\n"),
          std::string("Content-Length: 0001\r\n\r\n"), std::string("Content-Length: 1\r\nContent-Length: 1\r\n\r\n"),
          std::string("Transfer-Encoding: chunked\r\n\r\n"), std::string("X: ") + std::string(4096, 'x')}) {
          assert(r.exchange("GET " + r.path + " HTTP/1.1\r\nHost: " + r.authority + "\r\n" + tail).find("<main>") != std::string::npos); }
      assert(r.exchange(r.get() + r.get()).find("<main>") != std::string::npos); assert(r.exchange(r.get()).starts_with("HTTP/1.1 200")); }
    { Running r; int fd = r.connect_client(); assert(send(fd, "G", 1, 0) == 1);
      auto begin = std::chrono::steady_clock::now(); char c; assert(recv(fd, &c, 1, 0) > 0);
      assert(std::chrono::steady_clock::now() - begin < 4s); close(fd); }
    { Running r; int fd = r.connect_client(); assert(send(fd, "GET ", 4, 0) == 4);
      auto begin = std::chrono::steady_clock::now(); r.listener.cancel(); r.closed();
      assert(std::chrono::steady_clock::now() - begin < 500ms); char c; assert(recv(fd, &c, 1, 0) <= 0); close(fd); }
    { Running r([](const auto &, subsonic::LoginFailure &) { return true; }, 1); r.closed(); assert(!r.listener.active()); }
    { Running r([](const auto &, subsonic::LoginFailure &) { std::this_thread::sleep_for(1500ms); return true; }, 1);
      assert(r.exchange(r.post()).empty()); r.closed();
      std::this_thread::sleep_for(600ms); assert(!r.listener.take()); }
    { Running r; int fd = r.connect_client();
      auto header = "POST " + r.path + " HTTP/1.1\r\nHost: " + r.authority + "\r\nContent-Length: 5\r\n\r\nx";
      assert(send(fd, header.data(), header.size(), 0) == static_cast<ssize_t>(header.size()));
      char c; assert(recv(fd, &c, 1, 0) > 0); close(fd); }
    { Running r; for (int i = 0; i < 32; ++i) assert(r.exchange("bad\r\n\r\n").starts_with("HTTP/1.1 400")); r.closed(); }
    // Cancellation while validation runs closes sockets and invalidates result;
    // release callback before fixture exits, no detached test work left behind.
    { std::atomic<bool> entered{false}, release{false}, finished{false};
      Running r([&](const auto &, subsonic::LoginFailure &) { entered = true; while (!release) std::this_thread::sleep_for(1ms); finished = true; return true; });
      int fd = r.connect_client(); auto request = r.post(); assert(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
      for (int i = 0; i < 1000 && !entered; ++i) std::this_thread::sleep_for(1ms); assert(entered);
      std::this_thread::sleep_for(100ms); int queued = r.connect_client(); close(queued);
      r.listener.cancel(); r.closed(); close(fd); release = true;
      while (!finished) std::this_thread::sleep_for(1ms);
      std::this_thread::sleep_for(50ms); assert(!r.listener.take()); }
    std::cout << "pairing listener checks passed\n";
}
