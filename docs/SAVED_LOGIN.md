# Saved login: recommendation and threat model

## Recommendation

Store a revocable server-side API key instead of the account password whenever
the server allows it, and keep the plaintext password only as the fallback.
The console still has no secret store, so the improvement comes from changing
*what* is stored, not from hiding it better.

Implemented:

- **Password login, Navidrome with API keys.** After the normal Subsonic ping
  succeeds, Navi5 asks `getOpenSubsonicExtensions`. If the server is Navidrome
  and advertises `apiKeyAuthentication` version 1, it signs in once to the
  native API (`POST /auth/login`), creates a player named `Navi5 (PS5)` with a
  freshly generated key (`POST /api/player`), proves the key with a Subsonic
  ping, switches the session to the key and saves only the key. The password is
  dropped from memory and never written.
- **Existing saved password.** The same check runs on every saved-password
  auto-connect, so a console upgrades itself the first time it meets a server
  that can issue keys. The file is rewritten without the password.
- **API key typed on the phone form.** Now saved too (it used to be RAM-only).
  `tokenInfo` supplies the account name for display and history scope.
- **Everything else.** Servers that cannot issue keys, or any failure while
  issuing one, keep today's behavior: the session continues on the password and
  the password is saved. Issuance never fails a login.
- **Errors.** Subsonic 44 (invalid or revoked key) is a credential failure:
  the app asks for a new sign-in and keeps the file until it is replaced or
  forgotten. 42/43 (mechanism refused) are handled the same way. Neither ever
  triggers a silent retry with a password.

`/download0/navi5/login.json` holds exactly three string members: `server`,
`user`, and either `api_key` or `password`. A file with both, or with anything
else, is rejected. Mode 0600, directory 0700, atomic replace (unchanged).

### Availability

Navidrome merged API key authentication in PR #6219 on 2026-09-28. The latest
stable release when this was written (v0.64.2, 2026-09-24) predates it, so
stable servers stay on the password fallback until the next release. Nothing
needs to change in Navi5 then: the upgrade is driven by what the server
advertises.

### Verification status

Offline host tests cover the storage format, error mapping, the issuing
sequence against a mock transport and the worker's choice of secret. The
native build compiles and links.

Live check, 2026-11-12: a throwaway local `deluan/navidrome:develop` container
(`0.64.2-SNAPSHOT (94a6e59f)`, loopback HTTP, disposable account) driven by a
host program using the app's own transport and `subsonic/api_key.cpp`. Password
ping and extension check passed, a key was issued, and the key alone passed
ping, `tokenInfo` (correct owner) and `getAlbumList2`. An unknown key and the
issued key after its player was deleted both returned Subsonic 44, mapped to
`ApiKeyRejected`. The player appeared in Navidrome as `Navi5 (PS5)`.

Hardware check, 2026-11-12: DEV title `NAVD00001` (game category, installed
beside the untouched `NAVI00001`) on a PS5, kernel build `13.42`, against the
same throwaway develop container reached over LAN HTTP, observed over Remote
Play. All with a disposable account:

- QR phone-form password sign-in connected, and Navidrome then listed a
  `Navi5 (PS5)` player with a key.
- After the account password was changed on the server, a relaunch still
  auto-connected, so the saved secret is the key, not the password.
- After that player was deleted, a relaunch showed the login form with
  "API key is invalid or revoked. Sign in again." and did not retry.
- Forget showed the revoke-the-player note; the next launch had no saved login.

Hardware check, 2026-11-13, same setup: a password saved against Navidrome
0.64.2 (no API keys) was replaced by a key on the first auto-connect after the
server was swapped to the develop build on the same data; a relaunch after
changing the account password still connected. A track then played from the
key-only session (album page, fullscreen player, progress advancing).

The saved file itself was not read: the helper gets EACCES on the app's 0700
data directory. HTTPS issuance is covered on the host by the live check in
[CI_VALIDATION.md](CI_VALIDATION.md), not on hardware. **Not yet verified:**
HTTPS issuance on hardware, a stable Navidrome release, and the Media
`--release` build. Request shapes were taken from Navidrome source at `a7f19c7`
(2026-11-11): `model/player.go`, `persistence/player_repository.go`,
`server/auth.go`, `server/subsonic/middlewares.go`.

## What the key is

`nds_` followed by 22 base62 characters (about 131 bits) read from
`/dev/urandom` with rejection sampling; no fallback generator. Navidrome stores
only its SHA-256. The key is bound to one player and one user, does not expire,
and stops working when the player is deleted or its key is cleared in
Navidrome's Players page.

## Threat model

The console is jailbroken. Anything running as root or in the kernel, other
homebrew with filesystem access, an FTP payload and anyone holding the
console's storage can read `/download0/navi5/login.json`. No option below
changes that.

| Who | Saved password | Saved API key |
|---|---|---|
| Reads the file | Full account: Subsonic API, Navidrome web UI, admin functions if the user is an admin, password change, and any other service where that password is reused | Subsonic API as that user only |
| After the owner notices | Password must be changed everywhere it is used; every client signs in again | Delete one player in Navidrome; other clients and the password are unaffected |
| Sniffs HTTP to the music server | Token+salt per request (replayable); password itself during issuance | Key in every request URL (replayable until revoked); password once, during issuance |
| Reads server logs or proxy logs | Token+salt in URLs | Key in URLs |

What the key does **not** protect: the music library and the account's
Subsonic-level actions (playlists, stars, scrobbles, shares) remain exposed to
a file reader until the key is revoked. An admin user's key is still a key for
that user's Subsonic permissions. Use HTTPS so the key is not visible on the
network, and a non-admin account where practical.

Residual behavior worth knowing:

- **Logout and Forget do not revoke.** Revoking through the native API needs a
  session token, which needs the password Navi5 deliberately no longer has.
  Both delete the local file and tell the user to delete the `Navi5 (PS5)` player.
- **Where they are (v1.0).** Signed in: Options, Log out, confirm. That stops
  playback through the usual account reset, deletes the file through the Forget
  path and clears server, username and secrets from memory. Not signed in:
  "Forget saved login" is on the saved-login-failed screen, beside Retry, shown
  for network and credential failures alike. It is no longer on the login list.
- **Each password sign-in issues a new key.** A previous `Navi5 (PS5)` player
  is left in place, because a second console may be using it. Stale players
  are harmless but should be deleted when a console is retired or a key is
  suspected leaked.
- **Cancelled or failed issuance.** If the new key fails its ping the player is
  deleted again. If the user cancels mid-issue, one unused player can remain.
- **Memory.** Secrets live in ordinary C++ strings; wiping is best effort.
- **Older builds** cannot read a key-only file and fall back to the login form.

## Options considered

| Option | Verdict |
|---|---|
| Server-issued API key (this change) | Adopted. Only option that limits and makes revocable what a file reader gets. |
| PS5 savedata | Rejected. SDK v0.43 ships no `libSceSaveData` stub or sealed-key API, and savedata is readable on a jailbroken console anyway, so it would add platform risk for no confidentiality. |
| Encrypt the file with an app-held key | Rejected. The key would sit in the same sandbox or binary; anyone who can read the file can read the key. Obfuscation, and it would hide the real exposure from users. |
| Store token+salt instead of the password | Rejected. Replayable and password-equivalent for the Subsonic API, not revocable without a password change. |
| Never persist; QR sign-in every launch | Safest at rest, but defeats auto-connect. Still available: Forget, then sign in each time. |
| Navidrome reverse-proxy or JWT auth | Not a client-side answer; depends on each user's deployment. |

## Follow-ups

- Verify HTTPS issuance on hardware when a trusted-certificate test server is available.
- When a stable Navidrome release ships it, update the README security note.
- Optional: reuse or rotate this console's own player instead of adding one
  per password sign-in (needs a per-console identity to stay safe with two
  consoles).
