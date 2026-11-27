#!/usr/bin/env bash
# Run as root inside a disposable archlinux:base-devel container.
set -euo pipefail

pacman -Syu --needed --noconfirm git sudo
useradd --create-home builder
printf 'builder ALL=(root) NOPASSWD: /usr/bin/pacman\n' > /etc/sudoers.d/aur-build
chmod 0440 /etc/sudoers.d/aur-build

for project in lib files; do
  git init --bare "/work/$project.git"
  git -C "/work/$project.git" symbolic-ref HEAD refs/heads/testing
  git -C "/work/$project.git" fetch "/inputs/$project/source.bundle" HEAD:refs/heads/testing
  install -d -o builder "/work/$project"
  cp "/inputs/$project/"{PKGBUILD,.SRCINFO} "/work/$project/"
  chown builder "/work/$project/"{PKGBUILD,.SRCINFO}
done

# Exercise the actual makepkg Git source and branch handling using the two CI checkouts.
git config --file /etc/makepkg.d/gitconfig url.file:///work/lib.git.insteadOf https://github.com/Nythera-Dev/NytheraLib.git
git config --file /etc/makepkg.d/gitconfig url.file:///work/files.git.insteadOf https://github.com/Nythera-Dev/Nythera-Files.git

build_package() (
  cd "/work/$1"
  runuser -u builder -- makepkg --printsrcinfo | diff -u .SRCINFO -
  runuser -u builder -- makepkg --syncdeps --noconfirm
  pacman -U --needed --noconfirm ./*.pkg.tar.zst
)

build_package lib
build_package files

# Confirm pacman records the exact SONAME constraint derived from the linked binaries.
soname_dependency=$(bsdtar -xOf /work/files/*.pkg.tar.zst .PKGINFO |
  sed -n 's/^depend = \(libnythera_lib\.so=[^ ]*\)$/\1/p')
test -n "$soname_dependency"
pacman -T "$soname_dependency"
pkgconf --exists nythera-lib
ldd -r /usr/bin/nythera-file-manager 2>&1 | tee /tmp/files-linkage
if grep -E 'not found|undefined symbol' /tmp/files-linkage; then
  exit 1
fi
grep -q 'libarchive\.so' /tmp/files-linkage
grep -q 'libgdk_pixbuf-2\.0\.so' /tmp/files-linkage

test -r /usr/share/nythera-lib/assets/fonts/NytheraDisplay-Regular.ttf
test -r /usr/share/nythera-lib/assets/fonts/tabler.json
test -r /usr/share/nythera-lib/assets/translations/en.json
test -r /usr/share/nythera/files-assets/LICENSE
test -r /usr/share/pixmaps/dev.nythera.FileManager.png
desktop-file-validate /usr/share/applications/dev.nythera.FileManager.desktop
for entry in admin-session:nythera-admin-helper modify-properties:nythera-properties-helper; do
  helper="/usr/lib/${entry#*:}"
  policy="/usr/share/polkit-1/actions/dev.nythera.Files.${entry%:*}.policy"
  test "$(stat -c '%u:%a' "$helper")" = 0:755
  test "$(stat -c '%u:%a' "$policy")" = 0:644
  grep -Fq ">$helper</annotate>" "$policy"
done
