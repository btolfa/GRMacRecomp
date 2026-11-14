package elfloader

import (
	"os"
	"path/filepath"
	"strings"
)

// AppRoots is the host view of the guest's /app0. On Orbis an installed update (patch package) is
// layered over the base application per file: a path present in the patch is read from the
// patch, every other path from the base. Patch is empty when no update is installed.
type AppRoots struct {
	Base  string
	Patch string
}

// Path returns the host path of rel (a path relative to /app0): the patch's copy when the patch
// has one, the base's otherwise. It returns "" when there is no base.
func (r AppRoots) Path(rel string) string {
	if r.Base == "" {
		return ""
	}
	if r.Patch != "" {
		if p := filepath.Join(r.Patch, rel); exists(p) {
			return p
		}
	}
	return filepath.Join(r.Base, rel)
}

// Overlay maps a host path inside the base onto the patch's copy of the same file when the patch
// has one; any other path is returned unchanged.
func (r AppRoots) Overlay(path string) string {
	if r.Patch == "" || r.Base == "" {
		return path
	}
	rel, err := filepath.Rel(r.Base, path)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return path
	}
	if p := filepath.Join(r.Patch, rel); exists(p) {
		return p
	}
	return path
}

// dirs lists the roots, patch first.
func (r AppRoots) dirs() []string {
	var out []string
	for _, d := range []string{r.Patch, r.Base} {
		if d != "" {
			out = append(out, d)
		}
	}
	return out
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
