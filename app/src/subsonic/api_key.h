/* Navi5 — issue a revocable Navidrome API key from a password login.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Navidrome (OpenSubsonic apiKeyAuthentication v1) has no issuing endpoint in
 * the Subsonic API: a key is a client-chosen `nds_` + 22 base62 secret written
 * to a player record through the native API, and only its SHA-256 is stored
 * server-side. So: POST /auth/login for a short-lived session token, POST
 * /api/player with the new key, then prove the key with a Subsonic ping.
 * The password is used once and is not needed again; see docs/SAVED_LOGIN.md.
 *
 * Blocking; run off the render thread. Nothing here logs, and server text is
 * never returned. Other servers have no known issuing API: Unsupported.
 */
#pragma once

#include "subsonic.h"

namespace subsonic {

/* Player name shown in Navidrome's Players list; revoke or delete it there. */
constexpr const char *kApiKeyPlayerName = "Navi5 (PS5)";

/* Navidrome advertising apiKeyAuthentication version 1. */
bool can_issue_api_key(const ServerInfo &info, const std::vector<Extension> &extensions);

/* `nds_` + 22 uniform base62 characters (~131 bits) from /dev/urandom.
 * Empty when entropy is unavailable; never a weak fallback. */
std::string generate_api_key();

enum class IssueResult {
    Ok,
    Entropy,   /* no key generated; nothing sent */
    Login,     /* native login refused or unusable; nothing created */
    Create,    /* player/key creation refused */
    Verify,    /* key created but did not authenticate; removal attempted */
};

/* On Ok, *api_key authenticates as `username` and the password can be dropped.
 * On any failure *api_key is empty and the caller keeps its password session.
 * A cancelled caller may leave one unused player behind on the server. */
IssueResult issue_api_key(const std::string &server, const std::string &username,
                          const std::string &password, const HttpGet &get, const HttpSend &send,
                          std::string *api_key);

} // namespace subsonic
