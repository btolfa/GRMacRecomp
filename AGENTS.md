# GRMacRecomp

Gravity Rush Remastered (PS4, `CUSA01113`) recompiled ahead of time into a native macOS ARM64 app.
Fork of [PS4Recomp](https://github.com/wie-project/PS4Recomp).

## Upstreamable fixes

Fix the generic lifter, disassembler, emitter and runtime so that any PS4 title gets the same
behavior; Gravity Rush specifics live only in tests and patch XML. Guest code is translated ahead of
time: the runtime runs recompiled code and never generates it (no JIT).

## Honest work

- Each implementation does what Orbis does. When it can't yet, report the gap to the user rather than
  return a placeholder value.
- Verify against the real build and the game's `eboot.bin`; a script in a scratch directory proves nothing.
- Report problems and dead ends to the user when you hit them. The user prefers bad news to a hidden
  workaround.

## Steps and done

Each step lands one meaningful, verifiable change: bigger than a microtask, much smaller than the project.
A step is done when:

- `go vet ./...` and `go test -count=1 ./...` are clean;
- tests gated on `PS4_GR_EBOOT` (path of the game's `eboot.bin`) have run, or you report them as
  skipped. A skip is not a pass;
- allocations on changed hot paths are checked with `go test -benchmem` and escape analysis
  (`go build -gcflags=-m`).

## Conventions

- Write comments, docs and commit messages in English.
- Branch work as `gr/<topic>` and merge it into `gravity_rush`, the fork's default branch. `main`
  mirrors `wie-project/PS4Recomp`.
- Using the internet is fine; ask the user before downloading anything.
