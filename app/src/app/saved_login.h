/* Navi5 — saved login, plaintext in the title's own storage.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Policy (docs/SAVED_LOGIN.md): server URL, username and ONE secret at rest in
 * plaintext, file 0600, directory 0700: a revocable API key when the server
 * can issue or accept one, otherwise the password. Never both. Readable by
 * root/kernel payloads. Never log contents. Never store token+salt. Blocking
 * file I/O: call off the render thread.
 */
#pragma once
#include <string>

namespace app::saved_login {

constexpr const char *kPath = "/download0/navi5/login.json";

// Exactly one of password / api_key is stored. With a key, user is the key's
// account when known (display and history scope only) and may be empty.
struct Login { std::string server, user, password, api_key; };

enum class Load { Ok, Missing, Invalid };

// Missing: no file. Invalid: unreadable, oversize, bad JSON/fields or URL; out untouched.
Load load(Login &out, const char *path = kPath);
// Atomic: temp file (0600, O_EXCL|O_NOFOLLOW) + fsync + rename. false on invalid input or I/O error.
bool save(const Login &login, const char *path = kPath);
// true if removed or already absent.
bool forget(const char *path = kPath);

} // namespace app::saved_login
