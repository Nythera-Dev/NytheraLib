# Nythera AUR testing packages

`nythera-lib-git` and `nythera-files-git` build from their respective GitHub `testing` branches. The recipes are adapted from the checkout builds under `packaging/arch`. The AUR packages target x86_64 Arch Linux and CachyOS.

## Install, update and remove

After the packages have been registered, install both with an AUR helper. For example:

```sh
sudo pacman -Syu --needed base-devel git
yay -S nythera-lib-git nythera-files-git
```

Files pulls in the library if you install Files alone. Use `yay -S nythera-lib-git` for a library-only installation. Paru can be used in place of Yay. If your helper did not track the packages when you originally installed them, run `yay -Y --gendb` or `paru --gendb` once. Use `yay -Syu --devel` or `paru -Syu --devel` to include Git branch updates in a system upgrade. Exit and reopen running Nythera apps after updating their library.

Files includes its own artwork, desktop entry, archive backend, image preview support and Polkit helpers. Install the optional packages listed by pacman for features such as network browsing, video/PDF previews and content search. Administrator Mode requires a running Polkit authentication agent in the desktop session. On a previously hand-installed system, follow the [Files installation conventions][files-install] before migrating. The development and packaged helpers use different binary paths and share the same policy filenames.

Remove Files with `sudo pacman -Rns nythera-files-git`. To remove both packages together, run `sudo pacman -Rns nythera-files-git nythera-lib-git`. Keep the library installed while any other packaged consumer requires it.

## One-time maintainer setup

These steps require the maintainer's AUR and GitHub accounts.

1. Create or sign in to an [AUR account][aur-account]. Create a dedicated, passphrase-free SSH key for the unattended runner and add its **public** key in the AUR account settings.

   ```sh
   ssh-keygen -t ed25519 -N '' -C nythera-aur-ci -f ~/.ssh/nythera-aur
   ssh-keyscan -t ed25519 aur.archlinux.org > ~/.ssh/nythera-aur-known-hosts
   ssh-keygen -lf ~/.ssh/nythera-aur-known-hosts
   ```

   Compare the fingerprint with the Ed25519 fingerprint on the [AUR homepage][aur] before trusting or uploading the host key file.

2. After approving the initial snapshots, merge the reviewed `dev` into the local `testing` in both repositories and push each `testing` ref to GitHub. If setting up from a clone without a local `testing`, create it from the approved `dev` first. Once both remote branches exist, inspect the **Build and install both packages on Arch Linux** step in Actions before registering the packages. Initial runs can fail if the counterpart branch is still missing or publishing is attempted before AUR registration and secret setup.

3. Recheck name availability in the AUR. For each package, register it by cloning its empty AUR Git repository over SSH, copying the matching `PKGBUILD` and `.SRCINFO` from the approved `testing` checkout, committing both files and pushing to AUR `master`. Register NytheraLib first. The initial push claims the package name. See the [AUR submission guidelines][aur-submission].

   Example for NytheraLib, from its approved checkout:

   ```sh
   env GIT_SSH_COMMAND="ssh -i $HOME/.ssh/nythera-aur -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$HOME/.ssh/nythera-aur-known-hosts" git -c init.defaultBranch=master clone ssh://aur@aur.archlinux.org/nythera-lib-git.git
   cp packaging/aur/PKGBUILD packaging/aur/.SRCINFO nythera-lib-git/
   git -C nythera-lib-git add -- PKGBUILD .SRCINFO
   git -C nythera-lib-git commit -m 'Register nythera-lib-git'
   env GIT_SSH_COMMAND="ssh -i $HOME/.ssh/nythera-aur -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$HOME/.ssh/nythera-aur-known-hosts" git -C nythera-lib-git push origin master
   ```

   Repeat from the Files checkout with `nythera-files-git` in place of `nythera-lib-git`.

4. In **Settings → Secrets and variables → Actions** in both GitHub repositories, set `AUR_SSH_PRIVATE_KEY` to the full contents of `~/.ssh/nythera-aur` and `AUR_SSH_KNOWN_HOSTS` to the verified contents of `~/.ssh/nythera-aur-known-hosts`. The AUR account for that key must have write access to both packages. Re-run the latest `testing` workflows if necessary to verify the setup. With identical recipes, publication reports that the metadata is already current.

## Ongoing distribution and rebuilds

Develop on `dev`. Merge approved commits into `testing` manually. Keep `testing` history moving forward so the Git revision count in the package version stays monotonic. Reserve `main` for future stable releases.

A push to either `testing` branch builds that snapshot against the other repository's current `testing` in a disposable Arch container. The job installs the library package before building Files, then installs Files and checks its linkage, assets and Polkit installation. The publishing job mirrors the triggering repository's `PKGBUILD` and `.SRCINFO` to AUR `master` when either file changes. For packaging changes, edit the recipe in `packaging/aur`, bump `pkgrel` when users need a rebuild and regenerate `.SRCINFO` using `makepkg --printsrcinfo > .SRCINFO` in that directory. Routine source updates use `pkgver()` at build time, so there is no need to refresh the declared seed version on each merge.

- For a binary-compatible library change, promote NytheraLib normally. Users rebuild the library through their helper's development package updates.
- For changes that require recompiling Files, prevalidate the two proposed snapshots together. Publish compatible library extensions first when possible, then promote Files. Include a `pkgrel` bump in the Files snapshot if its source has no changes or the unchanged SONAME would otherwise leave users on an old build. Users should rebuild NytheraLib first and then Files. If a helper misses the Files rebuild, explicitly target Files for a fresh build using `yay -S --rebuild nythera-files-git` or `paru -S --rebuild nythera-files-git`.
- For a breaking ABI change, bump NytheraLib's Meson `soversion` and update the package/Meson version constraints if needed. Arch's versioned shared-library dependency stops pacman from replacing the old SONAME while an installed Files package still needs it. If the helper cannot stage the pair, build the library first, build Files in an isolated Arch build environment with the new library package installed, then install both resulting package files together with `sudo pacman -U path/to/nythera-lib-git-….pkg.tar.zst path/to/nythera-files-git-….pkg.tar.zst`. Include any other affected consumers in the transition.

The two `testing` branches update independently. Git-based packages see their branch as soon as a push lands, and Actions validates afterward. Plan cross-repository changes so intermediate snapshots remain compatible where possible. A successful build checks the submitted pair and linkage. It cannot detect every ABI break under an unchanged SONAME or rebuild already installed packages.

## Diagnose failed Actions runs

- **Checkout:** Confirm both public GitHub `testing` branches exist and contain their recipes. Files calls the reusable workflow from NytheraLib's `testing` branch.
- **Metadata:** Regenerate `.SRCINFO` with `makepkg --printsrcinfo` after editing a recipe. The build log shows any difference.
- **Package build or install:** Check the first failed command. A dependency or link error can come from incompatible `testing` snapshots. Correct the affected code or packaging on `dev`, validate it, then merge the fix into `testing`.
- **Publishing:** Confirm the packages have been registered on AUR `master`, the key's account can write to them, and both secrets are set. For a host-key failure, verify the new fingerprint against the AUR homepage before replacing the known-hosts secret. After fixing transient or credential errors, re-run the latest workflow. Superseded runs skip publishing.

[aur]: https://aur.archlinux.org/

[aur-account]: https://aur.archlinux.org/register

[aur-submission]: https://wiki.archlinux.org/title/AUR_submission_guidelines

[files-install]: https://github.com/Nythera-Dev/Nythera-Files/blob/testing/README.md#installation-conventions-development-packaged-and-unprivileged
