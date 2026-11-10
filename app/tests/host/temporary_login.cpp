/* Navi5 — one-shot runtime import boundary checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
static std::string base;
static bool deny_unlink;
static std::string mapped(const char *p) {
    assert(std::string(p).find("/app0/") == 0);
    return base + (p + 5);
}
static int test_open(const char *p, int flags) { return open(mapped(p).c_str(), flags); }
static int test_unlink(const char *p) { return deny_unlink ? -1 : unlink(mapped(p).c_str()); }
#define NAVI5_DEV_TOOLS 1
#define open test_open
#define unlink test_unlink
#include "app/dev_login.h"
#undef open
#undef unlink
int main() {
    char dir[] = "/tmp/navi5-temporary-XXXXXX";
    assert(mkdtemp(dir)); base = dir;
    const std::string config = base + "/.navi5-dev-login.tmp", secret = base + "/.navi5-dev-password.tmp";
    auto fixture = [&] {
        FILE *f = fopen(config.c_str(), "wb"); assert(f);
        fputs("{\"server\":\"https://localhost\",\"username\":\"fixture\",\"password_file\":\"/app0/.navi5-dev-password.tmp\"}", f);
        assert(!fclose(f)); assert(!chmod(config.c_str(), 0644));
        f = fopen(secret.c_str(), "wb"); assert(f); fputs("fixture-secret\n", f);
        assert(!fclose(f)); assert(!chmod(secret.c_str(), 0644));
    };
    std::string s, u, p; bool failed;
    fixture();
    assert(app::dev::import_temporary(s, u, p, failed) && !failed);
    assert(s == "https://localhost" && u == "fixture" && p == "fixture-secret");
    assert(access(config.c_str(), F_OK) && access(secret.c_str(), F_OK));
    assert(!app::dev::import_temporary(s, u, p, failed));
    fixture(); deny_unlink = true;
    assert(app::dev::import_temporary(s, u, p, failed) && failed);
    deny_unlink = false;
    unlink(secret.c_str()); assert(!symlink(config.c_str(), secret.c_str()));
    assert(!app::dev::import_temporary(s, u, p, failed));
    unlink(secret.c_str()); assert(!mkfifo(secret.c_str(), 0600));
    assert(!app::dev::import_temporary(s, u, p, failed));
    unlink(secret.c_str()); unlink(config.c_str()); rmdir(base.c_str());
}
