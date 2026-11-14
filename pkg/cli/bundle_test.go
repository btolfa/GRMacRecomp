package cli

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"ps4-recomp/pkg/elfloader"
	"ps4-recomp/pkg/sfo"
)

// The generated bundle carries the Game Mode keys and valid bundle structure.
func TestPackageAppBundleWithGameMode(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("app bundles are macOS only")
	}
	out := t.TempDir()
	bin := filepath.Join(out, "ps4_app")
	if err := copyFile("/usr/bin/true", bin); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(out, "guest_image.bin"), []byte("guest"), 0o644); err != nil {
		t.Fatal(err)
	}
	bundle, err := packageAppBundle(&Config{OutDir: out}, "eboot", bin)
	if err != nil {
		t.Fatalf("packageAppBundle: %v", err)
	}
	plist := filepath.Join(bundle, "Contents", "Info.plist")
	if o, err := exec.Command("plutil", "-lint", plist).CombinedOutput(); err != nil {
		t.Fatalf("Info.plist does not parse: %v: %s", err, o)
	}
	for key, want := range map[string]string{
		"LSSupportsGameMode":        "true",
		"LSApplicationCategoryType": "public.app-category.games",
		"CFBundleExecutable":        "eboot",
		"CFBundleIdentifier":        "org.ps4recomp.eboot",
	} {
		o, err := exec.Command("plutil", "-extract", key, "raw", plist).Output()
		if err != nil || strings.TrimSpace(string(o)) != want {
			t.Errorf("Info.plist %s = %q (%v), want %q", key, strings.TrimSpace(string(o)), err, want)
		}
	}
	// Check that guest_image.bin is located in SharedSupport, NOT Resources
	guestInShared := filepath.Join(bundle, "Contents", "SharedSupport", "guest_image.bin")
	if _, err := os.Stat(guestInShared); err != nil {
		t.Errorf("expected guest_image.bin in SharedSupport: %v", err)
	}
	guestInResources := filepath.Join(bundle, "Contents", "Resources", "guest_image.bin")
	if _, err := os.Stat(guestInResources); !os.IsNotExist(err) {
		t.Errorf("guest_image.bin must not be in Resources")
	}

	// packaging again succeeds
	if _, err := packageAppBundle(&Config{OutDir: out}, "eboot", bin); err != nil {
		t.Fatalf("second packageAppBundle: %v", err)
	}
}

func TestInfoPlistFromSFO(t *testing.T) {
	p := appInfoPlist("eboot", true, bundleMeta{BundleID: "org.ps4recomp.CUSA04529", DisplayName: "Uncharted 4: A Thief’s End & <Co>", Version: "01.32"})
	for _, want := range []string{
		"<string>org.ps4recomp.CUSA04529</string>",
		"<string>Uncharted 4: A Thief’s End &amp; &lt;Co&gt;</string>",
		"<key>CFBundleShortVersionString</key>\n    <string>01.32</string>",
		"<key>CFBundleExecutable</key>\n    <string>eboot</string>",
	} {
		if !strings.Contains(p, want) {
			t.Errorf("plist lacks %q:\n%s", want, p)
		}
	}
}

func TestBundleMetaFallback(t *testing.T) {
	m := bundleMetaFor(elfloader.AppRoots{Base: t.TempDir()}, "eboot")
	if m.BundleID != "org.ps4recomp.eboot" || m.DisplayName != "eboot" || m.Version != "1.0" {
		t.Errorf("fallback = %+v", m)
	}
}

func writeSFO(t *testing.T, entries ...sfo.Entry) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "sce_sys"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "sce_sys", "param.sfo"), sfo.Build(entries), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestBundleMetaFromSFO(t *testing.T) {
	dir := writeSFO(t,
		sfo.Entry{Key: "TITLE_ID", Str: "CUSA04529"},
		sfo.Entry{Key: "TITLE", Str: "Uncharted 4: A Thief’s End"},
		sfo.Entry{Key: "APP_VER", Str: "01.32"},
		sfo.Entry{Key: "VERSION", Str: "01.00"})
	m := bundleMetaFor(elfloader.AppRoots{Base: dir}, "eboot")
	if m.BundleID != "org.ps4recomp.CUSA04529" || m.DisplayName != "Uncharted 4: A Thief’s End" || m.Version != "01.32" {
		t.Errorf("meta = %+v", m)
	}
	if id := titleIDOf(elfloader.AppRoots{Base: dir}); id != "CUSA04529" {
		t.Errorf("titleIDOf = %q", id)
	}
}

// Real dumps carry VERSION but no APP_VER; the bundle id keeps only [A-Za-z0-9.-].
func TestBundleMetaVersionFallbackAndSanitizedID(t *testing.T) {
	dir := writeSFO(t,
		sfo.Entry{Key: "TITLE_ID", Str: "CUSA 00/557!"},
		sfo.Entry{Key: "TITLE", Str: "Game"},
		sfo.Entry{Key: "VERSION", Str: "01.05"})
	m := bundleMetaFor(elfloader.AppRoots{Base: dir}, "eboot")
	if m.BundleID != "org.ps4recomp.CUSA00557" || m.Version != "01.05" {
		t.Errorf("meta = %+v", m)
	}
}

// The runtime takes the bundle's SharedSupport as the app root (/app0) when it holds a game
// marker (ps4_vfs.c is_app_root). A bundle built without --standalone must hold none, also when
// an earlier build left game files there, so the game is read from the app dir it was built from;
// a --standalone bundle holds the whole game except its executables.
func TestPackageAppBundleGameFiles(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("app bundles are macOS only")
	}
	appDir := t.TempDir()
	for _, f := range []string{"sce_sys/param.sfo", "arc/data.psarc", "eboot.bin", "sce_module/libc.prx"} {
		p := filepath.Join(appDir, f)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := t.TempDir()
	bin := filepath.Join(out, "ps4_app")
	if err := copyFile("/usr/bin/true", bin); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(out, "guest_image.bin"), []byte("guest"), 0o644); err != nil {
		t.Fatal(err)
	}
	shared := filepath.Join(out, "eboot.app", "Contents", "SharedSupport")
	list := func() []string {
		var files []string
		err := filepath.Walk(shared, func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() {
				return err
			}
			rel, _ := filepath.Rel(shared, p)
			files = append(files, rel)
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
		return files
	}
	cfg := &Config{OutDir: out, AppDir: appDir, ElfPath: filepath.Join(appDir, "eboot.bin")}

	cfg.CopyResources = true
	if _, err := packageAppBundle(cfg, "eboot", bin); err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(list(), " "), "arc/data.psarc guest_image.bin sce_sys/param.sfo"; got != want {
		t.Errorf("--standalone SharedSupport = %q, want %q", got, want)
	}

	cfg.CopyResources = false
	if _, err := packageAppBundle(cfg, "eboot", bin); err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(list(), " "), "guest_image.bin"; got != want {
		t.Errorf("SharedSupport without --standalone = %q, want %q", got, want)
	}
}
