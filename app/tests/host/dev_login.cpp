/* Navi5 — private file boundary regression.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static bool zero_links = false, unsupported_flags = false, deny_fstat = false, wrong_owner = false;
static const char *replace_path = nullptr, *replace_with = nullptr;
static bool replace_before_open = false, interrupt_read = false, fail_read = false, grow_file = false;
static ssize_t platform_read(int fd, void *buf, size_t size) {
    if (interrupt_read) { interrupt_read = false; errno = EINTR; return -1; }
    if (fail_read) { errno = EIO; return -1; }
    return read(fd, buf, size > 3 ? 3 : size); // Exercise short reads.
}
static int platform_lstat(const char *, struct stat *) { errno = EPERM; return -1; }
static int platform_fstat(int fd, struct stat *st) {
    if (deny_fstat) { errno = EPERM; return -1; }
    int rc = fstat(fd, st);
    if (!rc && zero_links) st->st_nlink = 0;
    if (!rc && wrong_owner) st->st_uid = 0; // PS5 hardware: uid 0 while getuid()=1.
    if (!rc && grow_file) st->st_size = 1; // Metadata snapshot precedes growth.
    return rc;
}
static int platform_open(const char *path, int flags) {
    assert((flags & (O_NOFOLLOW | O_NONBLOCK)) == (O_NOFOLLOW | O_NONBLOCK));
    if (unsupported_flags) { errno = EINVAL; return -1; }
    if (replace_before_open) {
        assert(rename(replace_with, replace_path) == 0);
        replace_path = nullptr; replace_before_open = false;
    }
    int fd = open(path, flags);
    if (fd >= 0 && replace_path) {
        assert(rename(replace_with, replace_path) == 0);
        replace_path = nullptr;
    }
    return fd;
}
#define lstat platform_lstat
#define read platform_read
#define fstat platform_fstat
#define open platform_open
#include "app/dev_login.h"
#undef lstat
#undef read
#undef fstat
#undef open
int main() {
    char dir[] = "/tmp/navi5-private-XXXXXX";
    assert(mkdtemp(dir));
    std::string config = std::string(dir) + "/config", secret = std::string(dir) + "/password";
    auto write = [](const std::string &path, const std::string &data, int mode = 0600) {
        FILE *f = fopen(path.c_str(), "wb"); assert(f);
        assert(fwrite(data.data(), 1, data.size(), f) == data.size()); assert(fclose(f) == 0);
        assert(chmod(path.c_str(), mode) == 0);
    };
    std::string s, u, p;
    const std::string good = "{\"server\":\"https://localhost\",\"username\":\"fixture\",\"password_file\":\"" + secret + "\"}";
    write(secret, "fixture-password\n"); write(config, good);
    struct stat denied{};
    assert(platform_lstat(config.c_str(), &denied) == -1 && errno == EPERM);
    assert(app::dev::load(config.c_str(), s, u, p) && p == "fixture-password");
    write(config, good, 0644); assert(!app::dev::load(config.c_str(), s, u, p));
    std::string raw;
    assert(app::dev::read_input(config.c_str(), 8192, raw, true) && raw == good);
    // PS5 hardware fstat: links=0, uid=0 for own file. Both readers must accept it.
    zero_links = true; wrong_owner = true;
    assert(app::dev::read_input(config.c_str(), 8192, raw, true) && raw == good);
    write(config, good);
    assert(app::dev::load(config.c_str(), s, u, p) && p == "fixture-password");
    write(config, good, 0644);
    assert(!app::dev::read_private(config.c_str(), 8192, raw));
    assert(app::dev::import_status.stage == 11);
    zero_links = false; wrong_owner = false; unsupported_flags = true;
    assert(!app::dev::read_input(config.c_str(), 8192, raw, true));
    assert(app::dev::import_status.stage == 6 && app::dev::import_status.error == EINVAL);
    unsupported_flags = false;
    deny_fstat = true;
    assert(!app::dev::read_input(config.c_str(), 8192, raw, true));
    assert(app::dev::import_status.stage == 7 && app::dev::import_status.error == EPERM);
    deny_fstat = false;
    write(config, good);
    // Replacement after open must not redirect reads to a different inode.
    const std::string replacement = std::string(dir) + "/replacement";
    write(replacement, "replacement");
    replace_path = config.c_str(); replace_with = replacement.c_str();
    // rename unlinks old inode (nlink 0, accepted for PS5); descriptor still reads old bytes.
    assert(app::dev::read_private(config.c_str(), 8192, raw) && raw == good);
    write(config, good);
    assert(symlink(secret.c_str(), replacement.c_str()) == 0);
    replace_path = config.c_str(); replace_with = replacement.c_str(); replace_before_open = true;
    assert(!app::dev::read_private(config.c_str(), 8192, raw));
    assert(app::dev::import_status.stage == 6 && app::dev::import_status.error == ELOOP);
    unlink(config.c_str()); write(config, good);
    interrupt_read = true;
    assert(app::dev::read_private(config.c_str(), 8192, raw) && raw == good);
    fail_read = true;
    assert(!app::dev::read_private(config.c_str(), 8192, raw) && raw.empty());
    assert(app::dev::import_status.stage == 13 && app::dev::import_status.error == EIO);
    fail_read = false; grow_file = true;
    assert(!app::dev::read_private(config.c_str(), 3, raw) && raw.empty());
    assert(app::dev::import_status.stage == 13);
    grow_file = false;
    assert(!app::dev::read_private(config.c_str(), SIZE_MAX, raw));
    assert(!app::dev::read_private(nullptr, 8192, raw));
    assert(!app::dev::read_private(config.c_str(), 0, raw));
    write(config, "");
    assert(!app::dev::read_private(config.c_str(), 8192, raw));
    assert(app::dev::import_status.stage == 5);
    write(config, good);
    // Temporary import must never follow arbitrary password_file paths.
    assert(!app::dev::load(config.c_str(), s, u, p, true));
    for (const auto &bad : {good + "x", std::string("{}"), std::string(8193, 'x'),
         std::string("{\"server\":\"https://localhost\",\"server\":\"https://localhost\",\"username\":\"fixture\"}")}) {
        write(config, bad); assert(!app::dev::load(config.c_str(), s, u, p));
        assert(s.empty() && u.empty() && p.empty());
    }
    write(config, good); write(secret, std::string("bad\0secret", 10));
    assert(!app::dev::load(config.c_str(), s, u, p));
    write(secret, "fixture", 0644); assert(!app::dev::load(config.c_str(), s, u, p));
    write(secret, "fixture", 0600);
    unlink(config.c_str()); assert(symlink(secret.c_str(), config.c_str()) == 0);
    assert(!app::dev::load(config.c_str(), s, u, p));
    assert(!app::dev::read_input(config.c_str(), 8192, raw, true));
    unlink(config.c_str()); write(config, good);
    unlink(secret.c_str()); assert(symlink(config.c_str(), secret.c_str()) == 0);
    assert(!app::dev::load(config.c_str(), s, u, p));
    unlink(secret.c_str()); assert(mkfifo(secret.c_str(), 0600) == 0);
    assert(!app::dev::load(config.c_str(), s, u, p));
    assert(s.empty() && u.empty() && p.empty());
    assert(!app::dev::read_input(secret.c_str(), 1024, raw, true));
    assert(app::dev::import_status.stage == 8);
    unlink(secret.c_str()); write(secret, "fixture");
    const std::string hardlink = std::string(dir) + "/hardlink";
    assert(link(secret.c_str(), hardlink.c_str()) == 0);
    assert(!app::dev::read_input(secret.c_str(), 1024, raw, true));
    assert(app::dev::import_status.stage == 10 && app::dev::import_status.error == 0);
    unlink(hardlink.c_str());
    assert(!app::dev::read_input(hardlink.c_str(), 1024, raw, true));
    assert(app::dev::import_status.stage == 6 && app::dev::import_status.error == ENOENT);
    unlink(config.c_str()); unlink(secret.c_str()); rmdir(dir);
}
