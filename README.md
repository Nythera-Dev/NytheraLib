# NytheraLib

NytheraLib provides the shared C++23 infrastructure for Nythera Shell and Files. The applications link to the installed shared library through the `nythera-lib` pkg-config module.

## Build and install

On Arch Linux, build the package from a checkout using `makepkg -si` in `packaging/arch`. Pacman tracks the installed package and the applications’ dependency on it.

For coordinated development, use the repository `justfile`, which defaults to `debugoptimized` in `build-debug/` and installs into `$HOME/.local/opt/nythera-lib` (`libdir=lib`):

```sh
just configure
just build
just install debug
```

Other modes (`release`, `asan`) and maintenance recipes (`uninstall`, `clean`, `rebuild`, `format-code-check`, `lint`) follow the same mode conventions as Nythera Shell and Nythera Files.

Equivalent manual Meson commands for the user-local prefix:

```sh
meson setup build-debug --buildtype=debugoptimized --prefix="$HOME/.local/opt/nythera-lib" --libdir=lib
meson compile -C build-debug
meson install --no-rebuild -C build-debug
```

Set `PKG_CONFIG_PATH` to the chosen library directory’s `pkgconfig` directory when invoking Meson or `pkg-config` directly if your system does not search it by default (`just` in Nythera Shell and Nythera Files discovers `$HOME/.local/opt/nythera-lib/lib/pkgconfig` automatically when `nythera-lib` is not found elsewhere). Meson records the discovered library directory in the applications' build-time RUNPATH.

Meson installs these files under the specified prefix:

- `lib/libnythera_lib.so.0.1.0`, with SONAME `libnythera_lib.so.0`
- `lib/libnythera_lib.so.0` and `lib/libnythera_lib.so` symlinks
- `lib/pkgconfig/nythera-lib.pc`
- `include/nythera-lib/`, with shared C++ and Wayland protocol headers

## Coordinated development and rebuild discipline (`0.1.x`)

The Shell and Files packages currently require NytheraLib `0.1.x`. Keep coordinated development on `dev`; stable applications use a compatible packaged release from `main`. An incompatible binary interface outside coordinated `dev` iteration requires bumping `VERSION` (e.g., `0.2.0`) and `soversion`.

Because NytheraLib exports a wide C++23 header surface under a fixed `soversion: '0'` during `0.1.x` development, follow these rules across repositories:

1. **Build and install order:** Always build and install `nythera-lib` (`just build` then `just install debug`) before configuring, building, or running tests in `nythera-shell` or `nythera-files` against library changes.
2. **Implementation-only (`.cpp`) changes:** Changes confined to `.cpp` files that preserve exported signatures, class layouts, inline definitions, and binary compatibility only require rebuilding and reinstalling `nythera-lib`. Consumers do not need relinking; restart running `nythera` or `nythera-file-manager` processes to load the updated `libnythera_lib.so.0`.
3. **Header, layout, inline, or template changes:** Any change to installed `.h` files—including struct/class member layout, virtual table order, enums, function signatures, `inline` functions, or templates—requires rebuilding and reinstalling `nythera-lib` and then rebuilding **both** `nythera-shell` and `nythera-files` (or at least every consumer whose build includes the modified interfaces).
4. **Compiler, standard library, and ABI-affecting configuration changes:** Non-release builds (`debug` and `debugoptimized`) enable standard-library assertions (`-D_GLIBCXX_ASSERTIONS` or `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE`), while `release` disables them. Switching between `release` and non-release modes, enabling sanitizers (`asan`), or upgrading the C++ compiler/standard library can alter ABI boundaries; rebuild `nythera-lib`, `nythera-shell`, and `nythera-files` consistently together.
5. **When running applications must restart:** Already-running processes keep the previously loaded `libnythera_lib.so.0` mapped in memory. Restart `nythera` or `nythera-file-manager` after reinstalling `nythera-lib` (and after rebuilding the consumer when headers/ABI changed) to exercise updated code.
6. **Risks of stale libraries or headers:** Forgetting `just install debug` after editing `nythera-lib` causes consumers to compile against stale installed headers and test/run against the old shared library. Conversely, reinstalling `nythera-lib` after a header layout change without rebuilding Shell and Files risks ODR violations, symbol errors, or memory corruption/crashes at runtime.

The applications supply their own asset bundles and configure their own asset lookup paths at startup.
