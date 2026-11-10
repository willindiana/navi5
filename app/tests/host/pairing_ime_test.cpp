/* Navi5 — IME completion gates, no platform calls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/platform/ime.cpp"
#include <cassert>
namespace {
int outcome = 1, result = 0, init_result = 0;
}
extern "C" {
int sceCommonDialogInitialize() { return 0; }
int sceImeDialogAbort() { return 0; }
int sceImeDialogGetResult(ImeResult *r) { r->outcome = outcome; return result; }
int sceImeDialogGetStatus() { return 2; }
int sceImeDialogInit(const ImeParam *, const void *) { return init_result; }
int sceImeDialogTerm() { return 0; }
int sceSysmoduleLoadModule(uint16_t) { return 0; }
int sceUserServiceGetForegroundUser(int32_t *user) { *user = 0; return 0; }
}
int main() {
    int callbacks = 0;
    auto request = [&] { ime::request(ime::Kind::Text, "", "", [&](const auto &) { ++callbacks; }); };
    request(); ime::poll(); // Normal cancel, not no-callback failure.
    assert(!ime::active() && ime::cancelled() && callbacks == 0);
    init_result = -1; request();
    assert(!ime::active() && !ime::cancelled() && callbacks == 0);
    init_result = 0; result = -1; request(); ime::poll();
    assert(!ime::active() && !ime::cancelled() && callbacks == 0);
    result = 0; outcome = 0; request(); ime::poll();
    assert(!ime::active() && !ime::cancelled() && callbacks == 1);
}
