# GRMacRecomp

Ahead-of-time static recompiler that turns Gravity Rush Remastered (PS4, `CUSA01113`) into a native macOS ARM64 app.
Fork of [PS4Recomp](https://github.com/wie-project/PS4Recomp); the `main` branch mirrors upstream.

## Scope

- Gravity Rush Remastered is the target. Other titles are upstream's concern.
- Fix behavior in the generic recompiler and runtime, not for one executable. No title-ID checks or
  Gravity Rush special cases in `pkg/lifter`, `pkg/disasm`, `pkg/emitter` or `pkg/runtime`. Code
  specific to Gravity Rush belongs in tests and patch XML only. This keeps fixes upstreamable.
- The project must not turn into a JIT emulator. Guest code is translated ahead of time.

## Honesty

- No cheating: no tests that pass without checking anything, no faked results, no Python runs in an
  isolated directory standing in for the real build.
- No temporary solutions and no stubs that return 0 in place of a real implementation.
- A skipped test is not a pass. Tests that need the game skip unless `PS4_GR_EBOOT` points to the
  Gravity Rush Remastered `eboot.bin`; report them as skipped.
- Tell the user about problems instead of hiding them. Being deceived upsets the user more than bad news.

## Work size

- Never try to do the whole project in one step, and do not split work into microtasks that are not
  worth a separate step.

## Code

- Comments, README and documentation in English only.
- Run `go vet ./...` and `go test -count=1 ./...` before calling a change done.
- Keep the code clean and allocations minimal; check with the Go analysis tools.

## Branches

- Feature work goes on `gr/<topic>` branches and is merged into `gravity_rush`, the default branch of the fork.
- `main` follows `wie-project/PS4Recomp`; do not commit fork-only work there.

## Network

- Internet use is allowed. Ask the user before downloading anything; they usually agree.
