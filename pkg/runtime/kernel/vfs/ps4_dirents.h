// SPDX-License-Identifier: GPL-2.0-or-later
// Directory enumeration for the guest: host directories served as FreeBSD dirent records.
#ifndef PS4_DIRENTS_H
#define PS4_DIRENTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills buf with FreeBSD dirents for the directory open as fd (getdents/getdirentries). *basep
// (if given) is the entry index; a caller-supplied base restarts enumeration from that index.
// Returns bytes written, 0 at the end, or -1 with errno set.
int ps4_guest_getdirentries(int fd, char *buf, size_t nbytes, int64_t *basep);

// Forgets the listing snapshot of fd; call when the guest closes it.
void ps4_guest_dir_drop_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif // PS4_DIRENTS_H
