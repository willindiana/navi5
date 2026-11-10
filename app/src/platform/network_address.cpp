/*
 * Navi5 — Navidrome/Subsonic music client for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "network_address.h"

#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace netaddr {

static bool rfc1918(uint32_t a) { // host byte order
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
}

std::vector<std::string> private_ipv4(GetIfaddrs get, FreeIfaddrs release) {
    if (!get) get = ::getifaddrs;
    if (!release) release = ::freeifaddrs;
    std::vector<std::string> out;
    struct ifaddrs *list = nullptr;
    if (get(&list) != 0) return out; // list undefined on failure: never freed
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr in = reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr;
        if (!rfc1918(ntohl(in.s_addr))) continue;
        char text[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &in, text, sizeof text)) continue;
        if (std::find(out.begin(), out.end(), text) == out.end()) out.emplace_back(text);
    }
    if (list) release(list);
    return out;
}

} // namespace netaddr
