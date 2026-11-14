// SPDX-License-Identifier: GPL-2.0-or-later

package hosttest

import "testing"

// TestJson2 runs testdata/json2/main.c: sce::Json values with the firmware's object sizes,
// stored in guest memory from the game's allocator, copied deeply and serialized as the firmware
// does, without leaks.
func TestJson2(t *testing.T) {
	bin := buildHostC(t, "json2", "../runtime/modules/libSceJson2/ps4_json2_core.c")
	runHostCases(t, bin, []string{"sizes", "serialize", "deep-copy", "assign-from-child", "object-ops", "string-ops"})
}
