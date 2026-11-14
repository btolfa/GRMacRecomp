// SPDX-License-Identifier: GPL-2.0-or-later

package hosttest

import "testing"

// TestEqueue runs testdata/equeue/main.c: the Orbis event queue over a host kqueue (timer and fd
// knotes), runtime-posted events, waking and deleting while threads wait, and the guest shims.
func TestEqueue(t *testing.T) {
	bin := buildHostCWithIncludes(t, "equeue", []string{"../runtime/core"},
		"../runtime/kernel/equeue/ps4_equeue.c")
	runHostCases(t, bin, []string{
		"timer-periodic", "timer-delete", "fd-socket", "post-wakes", "mixed", "timeout",
		"delete-wakes", "shims",
	})
}
