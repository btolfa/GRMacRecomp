package elfloader

import (
	"os"
	"path/filepath"
	"testing"
)

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

// An update overrides the base per file; everything else comes from the base.
func TestAppRootsPath(t *testing.T) {
	base, patch := t.TempDir(), t.TempDir()
	writeFile(t, filepath.Join(base, "eboot.bin"), "base")
	writeFile(t, filepath.Join(patch, "eboot.bin"), "patch")
	writeFile(t, filepath.Join(base, "sce_sys", "icon0.png"), "base")
	r := AppRoots{Base: base, Patch: patch}
	if got := r.Path("eboot.bin"); got != filepath.Join(patch, "eboot.bin") {
		t.Errorf("Path(eboot.bin) = %q, want the update's", got)
	}
	if got := r.Path("sce_sys/icon0.png"); got != filepath.Join(base, "sce_sys", "icon0.png") {
		t.Errorf("Path(sce_sys/icon0.png) = %q, want the base's", got)
	}
	if got := (AppRoots{Base: base}).Path("eboot.bin"); got != filepath.Join(base, "eboot.bin") {
		t.Errorf("without an update Path(eboot.bin) = %q", got)
	}
	if got := r.Overlay(filepath.Join(base, "eboot.bin")); got != filepath.Join(patch, "eboot.bin") {
		t.Errorf("Overlay(base eboot) = %q", got)
	}
	if got := r.Overlay("/elsewhere/eboot.bin"); got != "/elsewhere/eboot.bin" {
		t.Errorf("Overlay(outside path) = %q", got)
	}
}

// A module in both roots is linked once, from the update; base-only modules still link.
func TestDiscoverModulesUpdateOverridesBase(t *testing.T) {
	base, patch := t.TempDir(), t.TempDir()
	writeFile(t, filepath.Join(base, "sce_module", "libA.prx"), "base-a")
	writeFile(t, filepath.Join(base, "sce_module", "libB.prx"), "base-b")
	writeFile(t, filepath.Join(patch, "sce_module", "libA.prx"), "patch-a")
	img := append([]byte{0, 0, 0, 0}, []byte("/app0/sce_module/libA.prx\x00")...)
	refs := DiscoverCompanionModules(filepath.Join(patch, "eboot.bin"), AppRoots{Base: base, Patch: patch}, img)
	got := map[string]string{}
	for _, r := range refs {
		if _, dup := got[filepath.Base(r.Path)]; dup {
			t.Fatalf("%s linked twice: %+v", filepath.Base(r.Path), refs)
		}
		got[filepath.Base(r.Path)] = r.Path
	}
	if len(got) != 2 {
		t.Fatalf("modules = %+v, want libA and libB", refs)
	}
	realPatch, _ := filepath.EvalSymlinks(patch)
	realBase, _ := filepath.EvalSymlinks(base)
	if want := filepath.Join(realPatch, "sce_module", "libA.prx"); got["libA.prx"] != want {
		t.Errorf("libA.prx from %q, want the update's %q", got["libA.prx"], want)
	}
	if want := filepath.Join(realBase, "sce_module", "libB.prx"); got["libB.prx"] != want {
		t.Errorf("libB.prx from %q, want the base's %q", got["libB.prx"], want)
	}
}
