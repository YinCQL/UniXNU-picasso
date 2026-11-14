// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_SNAPSHOT_STORE_CORE_H
#define PICASSO_SNAPSHOT_STORE_CORE_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PICASSO_SNAPSHOT_GET_CLASS = 63, PICASSO_SNAPSHOT_SET_CLASS = 64 };

static bool picasso_snapshot_store_png(const unsigned char *data, size_t size)
{
    static const unsigned char signature[16] = {
        137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'
    };
    if (!data || size < 33 || size > 16 * 1024 * 1024 || memcmp(data, signature, 16)) return false;
    uint32_t width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 |
                     (uint32_t)data[18] << 8 | data[19];
    uint32_t height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 |
                      (uint32_t)data[22] << 8 | data[23];
    return width && height && width <= 1600 && height <= 3000 && data[24] == 8 &&
           (data[25] == 2 || data[25] == 6) && data[26] == 0 && data[27] == 0 && data[28] <= 1;
}

static bool picasso_snapshot_store_beneath(const char *root, const char *parent)
{
    if (!root || !parent || root[0] != '/') return false;
    size_t n = strlen(root);
    return n > 1 && !strncmp(root, parent, n) && (!parent[n] || parent[n] == '/');
}

static bool picasso_snapshot_store_policy(bool unprotected, bool ephemeral, long content,
                                          bool protected_content, bool invalidated)
{
    return unprotected && !ephemeral && content == 0 && !protected_content && !invalidated;
}

// The caller has qualified the private snapshot directory and PNG. Do not
// create or replace a file: recover only the empty inode left by stock save.
static bool picasso_snapshot_store_write(const char *path, const unsigned char *bytes, size_t size)
{
    if (!path || !bytes || !size || size > 16 * 1024 * 1024) return false;
    struct stat initial = {0}, opened = {0};
    if (lstat(path, &initial) || !S_ISREG(initial.st_mode) || initial.st_size ||
        initial.st_nlink != 1 || initial.st_uid != geteuid()) return false;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fstat(fd, &opened) == 0 && opened.st_dev == initial.st_dev &&
              opened.st_ino == initial.st_ino && S_ISREG(opened.st_mode) &&
              opened.st_nlink == 1 && opened.st_uid == geteuid() && opened.st_size == 0;
    int before = ok ? fcntl(fd, PICASSO_SNAPSHOT_GET_CLASS) : -1;
    errno = 0;
    int set = ok && before == 0 ? fcntl(fd, PICASSO_SNAPSHOT_SET_CLASS, 2) : -1;
    int set_error = errno;
    int after = ok ? fcntl(fd, PICASSO_SNAPSHOT_GET_CLASS) : -1;
    // On the explicitly unprotected developer volume, APFS rejects changing
    // class 0 to class 2. Other failures or protection states keep stock failure.
    ok = ok && before == 0 && set == -1 && set_error == EPERM && after == 0;
    size_t written = 0, verified = 0;
    while (ok && written < size) {
        ssize_t count = pwrite(fd, bytes + written, size - written, (off_t)written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > size - written) { ok = false; break; }
        written += (size_t)count;
    }
    if (ok) ok = fsync(fd) == 0;
    unsigned char check[4096];
    while (ok && verified < size) {
        size_t want = size - verified;
        if (want > sizeof(check)) want = sizeof(check);
        ssize_t count = pread(fd, check, want, (off_t)verified);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > want || memcmp(check, bytes + verified, (size_t)count)) {
            ok = false; break;
        }
        verified += (size_t)count;
    }
    struct stat final = {0}, named = {0};
    if (ok) ok = fstat(fd, &final) == 0 && final.st_size == (off_t)size &&
                 final.st_nlink == 1 && lstat(path, &named) == 0 &&
                 named.st_dev == final.st_dev && named.st_ino == final.st_ino &&
                 fcntl(fd, PICASSO_SNAPSHOT_GET_CLASS) == 0;
    if (!ok && written) (void)ftruncate(fd, 0);
    if (close(fd)) ok = false;
    return ok;
}
#endif
