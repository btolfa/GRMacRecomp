// SPDX-License-Identifier: GPL-2.0-or-later

package hosttest

import "testing"

// TestVfsOverlay runs testdata/vfs_overlay/main.c: /app0 with an installed update layered over
// the base application per file, and directory listings that merge both.
func TestVfsOverlay(t *testing.T) {
	bin := buildHostC(t, "vfs_overlay",
		"../runtime/kernel/vfs/ps4_vfs.c",
		"../runtime/kernel/vfs/ps4_pak.c",
		"../runtime/kernel/vfs/ps4_dirents.c")
	runHostCases(t, bin, []string{"update-first", "env-overrides-hint", "no-update", "lower-dir", "union-listing"})
}
