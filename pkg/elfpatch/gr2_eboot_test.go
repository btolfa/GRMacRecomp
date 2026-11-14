package elfpatch

import (
	"bytes"
	"debug/elf"
	"encoding/hex"
	"os"
	"testing"

	"ps4-recomp/pkg/elfloader"
)

// gr2v111Bytes are bytes of the Gravity Rush 2 v1.11 eboot.bin (CUSA04943 and the other regions)
// at GoldHEN patch addresses (p_vaddr + ExecutableLoadBase): the sites its known patches and
// detail settings write. The table is the one GR2port (github.com/junminlee2004/GR2port,
// GPL-2.0-or-later, src/core/game_check/game_check.cpp) checks a dump against.
var gr2v111Bytes = []struct {
	addr  uint64
	bytes string
}{
	{0x0137a6d8, "75"},
	{0x0137a738, "488b3d31529600"},
	{0x0137b041, "0f84aaf7ffff"},
	{0x01377bbc, "75"},
	{0x01377c1c, "488b3d4d7d9600"},
	{0x0143e71f, "e81c8c1d00"},
	{0x0143ea18, "e823891d00"},
	{0x014365f4, "7309"},
	{0x01c320c4, "820e0e36"},
	{0x01c3220c, "820c0c36"},
	{0x01c32354, "820c0c36"},
	{0x01c32498, "82101036"},
	{0x01c3266c, "f6020c7e"},
	{0x01c32761, "00"},
	{0x01c33bd9, "00"},
	{0x0142dea0, "bafe010000"},
	{0x0142deb7, "bafe010000"},
	{0x0143ef3d, "83c20783c307c1ea03c1eb030fafda"},
	{0x00a317cc, "c5fa58059883e800"},
	{0x01c1b3d8, "6f82611400000000"},
	{0x008501da, "2f617263"},
	{0x0086c7da, "2f617263"},
	{0x0086c8b6, "2f617263"},
	{0x0086cb23, "2f617263"},
	{0x0086cbdc, "2f617263"},
	{0x0086cfca, "2f617263"},
	{0x014ce352, "2f617263"},
	{0x014e0b7a, "2f617263"},
	{0x01515f92, "2f617263"},
	{0x0151caf1, "2f617263"},
	{0x018ce7de, "2f617263"},
	{0x018af120, "0000000000000c4200005c420000a0420000dc42000011430000394300006643"},
	{0x018af140, "00000000000070410000c841"},
	{0x0098284e, "c5fa11053ae22f01c5fa110d2ae22f01c5fa111526e22f01"},
	{0x00982881, "c5fa1105efe12f01c5fa110ddfe12f01c5fa1115dbe12f01"},
	{0x009828a5, "c5fa1105d7e12f01c5fa110dc7e12f01c5fa1115c3e12f01"},
	{0x0183e088, "00008dba00000000"},
	{0x0141d7c1, "c783880b000000006000"},
	{0x0141d7d4, "b900006000"},
	{0x0141d864, "b900004000"},
	{0x0141d889, "c7835c0b000000004000"},
	{0x014e6705, "b9001c0000"},
	{0x018a9878, "0000000a00000000"},
}

// TestGravityRush2V111Eboot proves that the eboot.bin in PS4_GR2_EBOOT is the v1.11 build that
// patch addresses for Gravity Rush 2 are written for: every listed site holds the listed bytes.
func TestGravityRush2V111Eboot(t *testing.T) {
	path := os.Getenv("PS4_GR2_EBOOT")
	if path == "" {
		t.Skip("PS4_GR2_EBOOT is not set (path of the Gravity Rush 2 v1.11 eboot.bin)")
	}
	data, err := elfloader.ReadELFImage(path)
	if err != nil {
		t.Fatal(err)
	}
	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range gr2v111Bytes {
		wantBytes, err := hex.DecodeString(want.bytes)
		if err != nil {
			t.Fatal(err)
		}
		va := want.addr - ExecutableLoadBase
		var got []byte
		for _, p := range f.Progs {
			if p.Type == elf.PT_LOAD && va >= p.Vaddr && va+uint64(len(wantBytes)) <= p.Vaddr+p.Filesz {
				off := p.Off + va - p.Vaddr
				got = data[off : off+uint64(len(wantBytes))]
				break
			}
		}
		if got == nil {
			t.Errorf("0x%08x: not in a loaded segment", want.addr)
			continue
		}
		if !bytes.Equal(got, wantBytes) {
			t.Errorf("0x%08x: %x, want %s", want.addr, got, want.bytes)
		}
	}
}
