/*
 * Navi5 — offline private IPv4 discovery checks; injected getifaddrs only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "network_address.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <vector>

static std::vector<ifaddrs> nodes;
static std::vector<sockaddr_in> v4;
static sockaddr_in6 v6;
static ifaddrs *freed;
static int frees, result;

static void add(const char *ip, unsigned flags, int family = AF_INET) {
    ifaddrs n{};
    n.ifa_flags = flags;
    if (family == AF_INET6) {
        v6.sin6_family = AF_INET6;
        n.ifa_addr = reinterpret_cast<sockaddr *>(&v6);
    } else if (ip) {
        sockaddr_in s{};
        s.sin_family = AF_INET;
        assert(inet_pton(AF_INET, ip, &s.sin_addr) == 1);
        v4.push_back(s);
        n.ifa_addr = reinterpret_cast<sockaddr *>(&v4.back());
    }
    nodes.push_back(n);
}
static void reset() { nodes.clear(); v4.clear(); v4.reserve(32); freed = nullptr; frees = 0; result = 0; }
static int fake_get(ifaddrs **out) {
    for (size_t i = 0; i + 1 < nodes.size(); i++) nodes[i].ifa_next = &nodes[i + 1];
    if (!nodes.empty()) nodes.back().ifa_next = nullptr;
    *out = result ? reinterpret_cast<ifaddrs *>(0x1) : (nodes.empty() ? nullptr : nodes.data());
    return result;
}
static void fake_free(ifaddrs *p) { freed = p; frees++; }
using V = std::vector<std::string>;

int main() {
    const unsigned up = IFF_UP;
    reset();
    add("127.0.0.1", up | IFF_LOOPBACK);
    add("192.168.1.20", 0);           // down
    add(nullptr, up, AF_INET6);       // IPv6
    add(nullptr, up);                 // no address
    add("8.8.8.8", up);               // public
    add("0.0.0.0", up);               // wildcard
    add("169.254.1.1", up);           // link-local
    add("100.64.0.1", up);            // CGNAT
    add("172.15.255.255", up);
    add("172.32.0.0", up);
    add("11.0.0.1", up);
    add("192.169.0.1", up);
    add("10.0.0.5", up);
    add("172.16.0.1", up);
    add("172.31.255.254", up);
    add("192.168.0.9", up);
    add("10.0.0.5", up);              // duplicate, other interface
    assert((netaddr::private_ipv4(fake_get, fake_free) ==
            V{"10.0.0.5", "172.16.0.1", "172.31.255.254", "192.168.0.9"}));
    assert(frees == 1 && freed == nodes.data());

    reset();                          // single candidate
    add("192.168.50.2", up);
    assert((netaddr::private_ipv4(fake_get, fake_free) == V{"192.168.50.2"}) && frees == 1);

    reset();                          // nothing usable: still freed
    add("127.0.0.1", up | IFF_LOOPBACK);
    assert(netaddr::private_ipv4(fake_get, fake_free).empty() && frees == 1);

    reset();                          // empty list
    assert(netaddr::private_ipv4(fake_get, fake_free).empty() && frees == 0);

    reset();                          // API failure: garbage out-pointer never freed or read
    add("10.1.1.1", up);
    result = -1;
    assert(netaddr::private_ipv4(fake_get, fake_free).empty() && frees == 0);
    return 0;
}
