# NytheraLib Agent Instructions

Repository-specific guidance supplementing global agent instructions. Follow global rules alongside this document; do not treat this file as replacing them.

## Engineering Philosophy

**Practical engineering and acceptable risk.**

Nythera is designed to be lightweight, responsive, reliable and maintainable. It is not an enterprise-grade security product.

- Prefer established, proven approaches over unnecessarily complex custom solutions.
- Evaluate safeguards against realistic risks, not every theoretically possible edge case.
- Balance safety against performance, compatibility, complexity and maintenance costs.
- Prefer stronger guarantees where readily available, with practical fallbacks where appropriate.
- Do not sacrifice everyday functionality to eliminate exceedingly rare risks without clear justification.
- Treat explicitly accepted risks as settled engineering decisions, not outstanding defects.
- Benchmark potentially expensive safeguards before recommending or implementing them.
- Do not expand agreed safety requirements without approval.

The objective is reliable software for everyday use, not absolute guarantees at any cost.

## Common development rules

### Workflow, scope and tool efficiency

- **Orient and inspect:** Check `git status --short` before edits. Identify the owning subsystem with `rg --files`, focused `rg -n` searches, and bounded `sed -n` reads. Exclude `third_party/`, build trees, generated output, caches, and unrelated large assets unless relevant.
- **Implement coherently:** Keep related edits together. Avoid touching unrelated code, speculative abstractions, unnecessary refactoring, or mass reformatting. For research-only requests, inspect and explain without editing.
- **Preserve behaviour and invariants:** Preserve dark sci-fi/fantasy aesthetics, configurable styling, Umbriel compatibility, and approved architectural decisions. Surface conflicts with documented technical invariants rather than silently bypassing them.
- **Tool efficiency:** Prefer repository source over external browsing; prefer direct Git, `rg`/`sed`, and focused patches over heavy scripts (use Python or shell only for structured processing or reproducible diagnostics). Keep compiler/test output in local logs and inspect excerpts rather than dumping full logs. Await completion without repeated polling; check exit codes. Do not use subagents for routine work; delegate only independent bounded work where coordination costs are justified. Reuse existing visual captures for unchanged visual states.

### Build, formatting and validation safeguards

- Use the existing `build-debug/` directory (`debugoptimized` default) and incremental `just build` (which configures if necessary; do not run `just configure` before every build).
- Do not run `just rebuild`, `just clean`, ASan, release builds, full `just lint`, or full `just test` by default.
- Format **only touched C++ files** under `src/` and `tests/` with the project's configured `clang-format` version (22+). Do not run repo-wide `just format-code` for narrow changes.
- Prefer focused `clang-tidy -p build-debug --warnings-as-errors='*' <touched-files>` when lint is warranted; whole-tree `just lint` is an explicit-request/CI operation.
- Before a targeted Meson test, use `meson test -C build-debug --list` once if the exact test name is unknown, then run `meson test -C build-debug <test-name> --print-errorlogs`. Add a focused isolated regression test when protecting a deterministic contract not already covered; do not invent arbitrary tests merely to tick a box.
- **Review before finishing:** Run `git diff --check`, review the changed-file list and focused diff, check affected architectural invariants, and report actual checks run, scope, and unresolved items. Never cite unverified claims or inferred test passes.

- **Runtime compatibility during coordinated changes:** Before changing shared assets, resource layouts, public library interfaces or other files consumed at runtime, account for currently running applications and any subprocesses/helpers they may launch. Preserve compatibility with the active runtime until the coordinated build, installation and authorised restart are complete. If compatibility cannot be maintained, stop before the incompatible change and agree a safe transition and recovery plan with the maintainer. Validate affected runtime paths, including security-critical helpers where relevant, before removing temporary compatibility measures. Never weaken authentication or other safety checks to conceal a deployment mismatch.

- **Shell restart safety (NoNewPrivs):** Before any authorised live Nythera Shell restart, including coordinated downstream restarts, check `NoNewPrivs` in `/proc/<pid>/status` for the actual launching process/environment. Never launch or restart live Shell from an environment with `NoNewPrivs: 1`; this restriction is inherited and cannot be cleared by exec or disown. If the launch environment is restricted or cannot be verified, leave the restart to the maintainer from an unrestricted terminal or use an explicitly authorised, verified unrestricted launch mechanism. After restarting, verify `NoNewPrivs: 0` in `/proc/<nythera-pid>/status` for the actual running Shell before declaring deployment successful or asking the maintainer to test locking. On 26 November 2026, an agent-launched Shell with `NoNewPrivs: 1` failed lock-screen PAM authentication with “Authentication service cannot access authentication info”; restarting from the maintainer's unrestricted terminal restored `NoNewPrivs: 0` and successful lock/unlock. Do not change PAM or authentication permissions to work around this deployment problem.

### Authorisation and Git safety

- Do not modify live user configuration (`~/.config/nythera`, `~/.local/state/nythera`), install host packages, modify system configuration, write to system prefixes (`/usr`, `/usr/local`), restart running applications/services, commit, or push without explicit authorisation.
- Protect uncommitted work: do not reset, overwrite, stash, rebase, force-push, or merge into `main` without explicit approval and an accounted-for state.
- Use `dev` for ordinary development; create a temporary task branch or worktree only for substantial or risky changes.
- When authorised to commit or push "your changes", identify and stage the complete relevant session change set across implementation, tests, assets, and docs (never blindly staging the entire working tree; ask if ownership is uncertain). Verify the remote branch after pushing and report what was committed, what was pushed, and any relevant uncommitted files.

### Baseline test isolation and data safety

- All tests involving filesystem mutations or user/system services must operate exclusively on isolated, disposable fixtures—never touching live user config, home directory contents, or the live session bus.
- Any test constructing `ConfigService` or service/filesystem helpers must isolate `HOME`, `NYTHERA_{CONFIG,STATE,DATA}_HOME`, `XDG_{CONFIG,STATE,DATA,CACHE}_HOME`, set `GIO_USE_VFS=local`, and disable or privately isolate `DBUS_SESSION_BUS_ADDRESS` before instantiation; never fall back silently to the live session bus.
- Create fixture roots exclusively (`mkdtemp` or exclusive `create_directory`) and clean up by exact owned path only—never via prefix or wildcard sweeps.
- Drain asynchronous workers and helper sessions cleanly before teardown; avoid `std::exit()` or unguarded paths that skip fixture disposal. Immediately stop and report any unintended user-data modification or isolation gap.

---

## NytheraLib context and invariants

- **NytheraLib** provides the shared C++23 infrastructure (`libnythera_lib.so.0.1.0`, `SONAME: libnythera_lib.so.0`, pkg-config module `nythera-lib`) for **Nythera Shell** and **Nythera Files**, built with **Meson/Ninja** and orchestrated by `just`.
- Development builds install under `$HOME/.local/opt/nythera-lib` with `libdir=lib` (`lib/pkgconfig/nythera-lib.pc`, `include/nythera-lib/**`, `share/nythera-lib/assets/**`). Both downstream applications require `nythera-lib` at `>=0.1.0, <0.2.0`.
- Keep `nythera-lib` application-agnostic: do not add build-time or runtime dependencies on `nythera-shell` or `nythera-files`, hardcode application IDs in shared components, or assume a single application's private asset bundle or configuration ownership.
- **Shared assets and translations:** NytheraLib is the single source of truth for shared fonts (`assets/fonts/NytheraDisplay-Regular.ttf`, `assets/fonts/nythera-tabler.ttf`), Tabler glyph metadata (`assets/fonts/tabler.json`), and translation keys belonging to shared library components (`assets/translations/*.json`), resolved via `paths::sharedAssetsRoot()` / `paths::sharedAssetPath()`. Do not edit or regenerate non-English translations unless explicitly tasked.
- **Unit tests:** NytheraLib owns its self-contained unit test suite under `tests/`, runnable independently via `just test` without requiring Nythera Shell or Nythera Files.
- Preserve configurable styling primitives, Wayland protocol wrappers (`protocols/`), and development build optimisations (`debugoptimized` default, LLD, split/compressed DWARF, and stdlib assertions in non-release builds). Do not edit or format vendored `third_party/` code or generated protocol headers unless explicitly tasked.

## Cross-repository coordination (`0.1.x`)

- **Downstream consumers:** `nythera-shell` and `nythera-files` dynamically link `libnythera_lib.so.0` and include installed headers from `include/nythera-lib/`.
- **Build and install order:** Always build and install `nythera-lib` (`just build` then `just install debug` when authorised to update the dev prefix) before building or testing `nythera-shell` or `nythera-files` against library changes.
- **Implementation-only (`.cpp`) changes** preserving binary compatibility only require rebuilding and reinstalling `nythera-lib`; running consumer processes must be restarted (only when explicitly authorised) to map the updated shared library.
- **Public header (`.h`), layout, vtable, inline/template, or ABI-affecting option changes** (such as `debug`/`debugoptimized` vs `release`, sanitizers, or C++ toolchain/stdlib upgrades) require rebuilding and reinstalling `nythera-lib` and then rebuilding **both** `nythera-shell` and `nythera-files`. See `README.md` for full details.

## Choose validation by change type

| Change                                                                           | Default validation                                                                                                                                         |
| -------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Documentation or comments only                                                   | `git diff --check`. No C++ build/tests.                                                                                                                    |
| Implementation-only (`.cpp`) changes preserving binary compatibility             | Incremental `just build`; run directly affected library tests with `meson test -C build-debug <test-name> --print-errorlogs`.                                |
| Unit tests, shared assets, or translation key changes                            | For translation edits, run `python3 tools/i18n-check.py`; run targeted `meson test -C build-debug <test-name> --print-errorlogs` (never edit non-English translations unless tasked). |
| Public headers (`.h`), inline/template code, `meson.build`, or ABI-affecting opts | Incremental `just build`, directly affected library tests, and (when authorised to install) `just install debug` followed by rebuilding/testing affected consumers. |

## Working with project documentation

- `README.md`: development build/install/test workflow (`justfile`), user-local prefix (`$HOME/.local/opt/nythera-lib`), shared assets, and coordinated `0.1.x` rebuild discipline.
- `meson.build` and `meson_options.txt`: installed public headers, shared `assets/` install rules, `nythera-lib.pc` generation, `native_optimizations`, `system_libraries`, and `tests` options.
- `tools/i18n-check.py`: shared library translation key validation against `assets/translations/en.json`.
