/* Navi5 — disposable native-probe host checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/dev_reader_check.h"
#include <cassert>
#include <cstdlib>
#include <string>
int main() {
    char dir[] = "/tmp/navi5-reader-XXXXXX";
    assert(mkdtemp(dir));
    const auto path = std::string(dir) + "/fixture";
    app::dev::ReaderCheck check;
    app::dev::check_reader(check, path.c_str());
    assert(check.stage == 8 && check.error == 0 && check.bits == 31 && check.mismatch == 0);
    assert(S_ISREG(check.mode) && (check.mode & 0777) == 0600 && check.nlink == 1 &&
           check.uid == check.expected_uid && check.size == 0);
    struct stat bad{};
    bad.st_mode = S_IFDIR | 0644; bad.st_nlink = 2; bad.st_uid = getuid() + 1; bad.st_size = 1;
    assert(app::dev::reader_metadata_mismatch(bad) == 27);
    struct stat ps5{}; // Hardware 5f7299b: regular 0600, size 0, links 0, uid 0.
    ps5.st_mode = S_IFREG | 0600; ps5.st_nlink = 0; ps5.st_uid = 0;
    assert(app::dev::reader_metadata_mismatch(ps5) == 0);
    assert(access(path.c_str(), F_OK) == -1 && errno == ENOENT);
    // Existing objects are never opened, read, replaced or removed; FIFO cannot hang.
    assert(mkfifo(path.c_str(), 0600) == 0);
    app::dev::check_reader(check, path.c_str());
    assert(check.stage == 2 && check.error == EEXIST);
    assert(unlink(path.c_str()) == 0);
    assert(symlink("absent", path.c_str()) == 0);
    app::dev::check_reader(check, path.c_str());
    assert(check.stage == 2 && check.error == EEXIST);
    assert(unlink(path.c_str()) == 0);
    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    const auto alias = std::string(dir) + "/alias";
    assert(link(path.c_str(), alias.c_str()) == 0);
    app::dev::check_reader(check, path.c_str());
    assert(check.stage == 2 && check.error == EEXIST);
    struct stat st{};
    assert(stat(path.c_str(), &st) == 0 && st.st_nlink == 2 && st.st_size == 0);
    assert(unlink(alias.c_str()) == 0 && unlink(path.c_str()) == 0);
    assert(rmdir(dir) == 0);
}
