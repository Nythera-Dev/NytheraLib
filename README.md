# NytheraLib

NytheraLib provides the shared C++23 infrastructure for Nythera Shell and Files. The applications link to the installed shared library through the `nythera-lib` pkg-config module.

## Build and install

On Arch Linux, build the package from a checkout using `makepkg -si` in `packaging/arch`. Pacman tracks the installed package and the applications’ dependency on it.

For a user-local development installation with Meson:

```sh
meson setup build --prefix="$HOME/.local/opt/nythera-lib" --libdir=lib
meson compile -C build
meson install -C build
```

Set `PKG_CONFIG_PATH` to the chosen library directory’s `pkgconfig` directory if your system does not search it by default. Meson records the discovered library directory in the applications' build-time RUNPATH. Build the library and applications with compatible C++ toolchains and standard libraries.

Meson installs these files under the specified prefix:

- `lib/libnythera_lib.so.0.1.0`, with SONAME `libnythera_lib.so.0`
- `lib/libnythera_lib.so.0` and `lib/libnythera_lib.so` symlinks
- `lib/pkgconfig/nythera-lib.pc`
- `include/nythera-lib/`, with shared C++ and Wayland protocol headers

The Shell and Files packages currently require NytheraLib 0.1.x. Keep coordinated development on `dev`; stable applications use a compatible packaged release from `main`. An incompatible binary interface requires a new SONAME.

The applications supply their own asset bundles and configure their own asset lookup paths at startup.
