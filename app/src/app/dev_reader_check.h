/* Navi5 — non-secret native reader primitive check.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace app::dev {
// Bits: exclusive regular creation, metadata, NONBLOCK flag, bytes, cleanup.
// NOFOLLOW rejection, FIFO behavior and hardlink metadata remain UNPROVEN.
struct ReaderCheck {
    std::atomic<int> stage{0}, error{0}, bits{0}, mismatch{0};
    std::atomic<unsigned> mode{0}, uid{0}, expected_uid{0};
    std::atomic<unsigned long long> nlink{0};
    std::atomic<long long> size{0};
};
// Bits: type, link count, permissions, initial size; same policy as read_input.
// Hardware 5f7299b: st_nlink=0 and st_uid=0 (getuid()=1) for this fresh file, so
// links 0/1 pass and owner (bit 4, retired) is reported but not compared.
inline int reader_metadata_mismatch(const struct stat &st) {
    return (!S_ISREG(st.st_mode) ? 1 : 0) | (st.st_nlink > 1 ? 2 : 0) |
           ((st.st_mode & 0777) != 0600 ? 8 : 0) | (st.st_size != 0 ? 16 : 0);
}
inline void check_reader(ReaderCheck &result, const char *path) {
    constexpr char fixture[] = "Navi5 reader fixture\n";
    int bits = 0, error = 0, stage = 1;
    result.bits.store(0);
    result.mismatch.store(0);
    result.mode.store(0); result.nlink.store(0); result.uid.store(0);
    result.expected_uid.store(0); result.size.store(0);
    result.error.store(0);
    result.stage.store(stage);
    // O_EXCL|O_CREAT cannot open an existing FIFO, symlink or private file.
    // Never strip flags or retry; collision means no access and no cleanup.
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) { result.error.store(errno); result.stage.store(2); return; }
    bits |= 1;
    struct stat st{};
    if (fstat(fd, &st)) { stage = 3; error = errno; }
    else {
        const uid_t uid = getuid();
        result.mode.store(st.st_mode); result.nlink.store(st.st_nlink);
        result.uid.store(st.st_uid); result.expected_uid.store(uid);
        result.size.store(st.st_size);
        result.mismatch.store(reader_metadata_mismatch(st));
        if (result.mismatch.load()) stage = 4;
        else {
        bits |= 2;
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0) { stage = 5; error = errno; }
        else if (!(flags & O_NONBLOCK)) stage = 6;
        else {
            bits |= 4;
            char bytes[sizeof fixture]{};
            if (write(fd, fixture, sizeof fixture - 1) != sizeof fixture - 1 ||
                fstat(fd, &st) || st.st_size != sizeof fixture - 1 ||
                lseek(fd, 0, SEEK_SET) != 0 ||
                read(fd, bytes, sizeof bytes) != sizeof fixture - 1 ||
                std::memcmp(bytes, fixture, sizeof fixture - 1)) {
                stage = 7; error = errno;
            } else { bits |= 8; stage = 8; }
        }
        }
    }
    if (close(fd)) { stage = 9; error = errno; }
    if (unlink(path)) { stage = 10; error = errno; }
    else bits |= 16;
    // ponytail: regular-descriptor probe only; NOFOLLOW/FIFO/hardlink rejection stay
    // unproven natively. DEV import accepted on that basis; release never imports.
    result.bits.store(bits);
    result.error.store(error);
    result.stage.store(stage);
}
} // namespace app::dev
