# NytheraLib Agent Instructions

Repository-specific guidance supplementing the global Codex `AGENTS.md`. Follow global instructions for general research, initiative, safety, efficient tool use and reporting; do not treat this file as replacing them.

## Project context and invariants

- **NytheraLib** provides the shared C++23 infrastructure (`libnythera_lib.so.0.1.0`, `SONAME: libnythera_lib.so.0`, pkg-config module `nythera-lib`) for **Nythera Shell** and **Nythera Files**, built with **Meson/Ninja** and orchestrated by `just`.
- Development builds install under `$HOME/.local/opt/nythera-lib` with `libdir=lib` (`lib/pkgconfig/nythera-lib.pc`, `include/nythera-lib/**`). Both downstream applications require `nythera-lib` at `>=0.1.0, <0.2.0`.
- Keep `nythera-lib` application-agnostic: do not add build-time or runtime dependencies on `nythera-shell` or `nythera-files`, and do not assume a single application's private asset bundle or configuration ownership.
- Preserve configurable styling primitives, Wayland protocol wrappers (`protocols/`), and development build optimisations (`debugoptimized` default, LLD, split/compressed DWARF, and stdlib assertions in non-release builds).
- Follow `README.md` for installation and coordinated `0.1.x` rebuild rules. Surface conflicts with documented technical invariants rather than silently bypassing them.

## Repository workflow

1. **Orient and inspect.** Check `git status --short` before edits. Identify the owning subsystem using `rg --files`, focused `rg -n` searches and bounded `sed -n` reads. Exclude `third_party/`, build trees, generated output and caches unless relevant.
2. **Implement coherently.** Keep related edits together and avoid touching unrelated code, vendored `third_party/` code unless explicitly tasked, or mass reformatting. For research-only requests, inspect and explain without editing.
3. **Validate proportionately.** Use the change-type matrix below. Remember that compiling `nythera-lib` alone does not update `$HOME/.local/opt/nythera-lib`: run `just install debug` before validating downstream consumers, and rebuild consumers whenever public headers or ABI-affecting build options change.
4. **Review before finishing.** Use `git diff --check`, review the changed-file list and focused diff, and check downstream impact on `nythera-shell` and `nythera-files`. Recheck after subsequent edits when necessary.

## NytheraLib-specific tool practices

- Prefer repository source to external browsing; consult external docs for unresolved API, compatibility or upstream-change questions.
- Prefer direct Git, `rg`/`sed` and focused patches for ordinary operations. Use Python or shell scripts for structured processing or reproducible diagnostics—not to wrap a single trivial command.
- Keep compiler/test output in local logs and inspect error excerpts rather than dumping full logs. Await process completion without repeated polling; check exit codes.
- Do not use subagents for routine work; delegate only independent bounded work where coordination costs are justified.
- Do not modify live shell configuration, install system packages, clean builds, write to system prefixes (`/usr`, `/usr/local`), or restart running consumer applications without explicit authorisation.

## Cross-repository dependencies and rebuild discipline

- **Downstream consumers:** `nythera-shell` and `nythera-files` dynamically link `libnythera_lib.so.0` and include installed headers from `include/nythera-lib/`.
- **Build and install order:** Build and install `nythera-lib` (`just build` then `just install debug`) before building or testing `nythera-shell` or `nythera-files` against library changes.
- **Implementation-only (`.cpp`) changes** that preserve binary compatibility only require rebuilding and reinstalling `nythera-lib`; running consumer processes still keep the old library mapped until restarted (restart only when explicitly authorised).
- **Public header (`.h`), layout, vtable, inline/template, or ABI-affecting option changes** (such as `debug`/`debugoptimized` vs `release`, sanitizers, or C++ toolchain/stdlib upgrades) require rebuilding and reinstalling `nythera-lib` and then rebuilding both `nythera-shell` and `nythera-files`. See `README.md` for full details.
- **Unit tests:** `nythera-lib` does not currently host a standalone test suite (its `justfile` intentionally has no `test` recipe); validate library behaviour through affected Meson tests in `nythera-shell` and/or `nythera-files` after `just install debug`.

## Choose validation by change type

| Change                                                                           | Default validation                                                                                                                                                              |
| -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Documentation or comments only                                                   | `git diff --check`. No C++ build.                                                                                                                                               |
| Implementation-only (`.cpp`) changes preserving binary compatibility             | Incremental `just build`; if runtime behaviour requires test verification, `just install debug` and run directly affected tests in `nythera-shell` and/or `nythera-files`.        |
| Public headers (`.h`), inline/template code, `meson.build`, or ABI-affecting opts | Incremental `just build` and `just install debug`, then rebuild affected consumers (`nythera-shell` and/or `nythera-files`) and run affected consumer tests.                    |

- Use the existing `build-debug/` directory (`debugoptimized` default) and incremental builds when available and correctly configured. `just build` configures if necessary; do not run `just configure` before every build.
- Do not run `just rebuild`, `just clean`, ASan, release builds, or full `just lint` by default.
- Prefer focused clang-tidy for touched `.cpp` files if lint is warranted: `clang-tidy -p build-debug --warnings-as-errors='*' <touched-files>`. Whole-tree `just lint` is an explicit-request/CI operation.
- Prefer formatting **only touched C++ files** under `src/` with the project's configured clang-format version (22+). Do not format `third_party/` or generated protocol files, and do not run repo-wide `just format-code` for narrow changes.

## Working with project documentation

- `README.md`: development build/install workflow (`justfile`), user-local prefix (`$HOME/.local/opt/nythera-lib`), and shared-library rebuild discipline (`0.1.x`).
- `meson.build` and `meson_options.txt`: installed public headers, `nythera-lib.pc` generation, `native_optimizations`, and `system_libraries` options.

## Git safety

- Protect uncommitted work: do not reset, overwrite, stash, rebase, force-push or merge into `main` without explicit approval and an accounted-for state.
- Use `dev` for ordinary development. Create a temporary task branch (or worktree) only for substantial or risky changes.
- Do not commit or push until explicitly authorised.

### Committing and pushing session work

- When asked to commit or push "your changes", identify the complete relevant change set from the current task or session across implementation, packaging, docs, additions and deletions.
- Review the files to be staged and commit the full relevant change set; do not blindly stage the entire working tree. If ownership or inclusion is genuinely uncertain, ask before including those files.
- After pushing, verify the remote branch contains the intended commit and report what was committed, what was pushed and any relevant changes left uncommitted.
