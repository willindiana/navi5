/*
 * Navi5 — Navidrome/Subsonic music client for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console's own private LAN IPv4 addresses, for binding the pairing listener.
 * Blocking: call off the render thread. Never log the result.
 */
#pragma once

#include <string>
#include <vector>

struct ifaddrs;

namespace netaddr {

using GetIfaddrs = int (*)(struct ifaddrs **);
using FreeIfaddrs = void (*)(struct ifaddrs *);

// Up, non-loopback, RFC1918 interface addresses in dotted form, deduplicated,
// enumeration order. Empty means failure or none: fall back to manual entry.
// More than one: caller asks the viewer; never pick arbitrarily.
// Parameters exist for offline tests only.
std::vector<std::string> private_ipv4(GetIfaddrs get = nullptr, FreeIfaddrs release = nullptr);

} // namespace netaddr
