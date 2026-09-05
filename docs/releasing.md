# Releasing hyprspace

Releases contain source code and SHA-256 checksums. Users build against their
own Hyprland installation. The release workflow creates a draft after all CI
jobs pass; publishing that draft is a separate maintainer action.

## Prepare a version

1. Set the same `X.Y.Z` version in `hyprpm.toml` and `src/Version.hpp`.
2. Add notes under `## X.Y.Z` in `CHANGELOG.md`, including the tested Hyprland
   version and commit. Check that the README reports the same compatibility.
3. Run the checks in [CONTRIBUTING.md](../CONTRIBUTING.md) and check the live
   overlays. Refresh the screenshots if the interface changed.
4. Commit the release files, push the branch and wait for Checks to pass.

The initial version is `1.0.0`. For that version:

```bash
git tag -a v1.0.0 -m "hyprspace 1.0.0"
make dist TAG=v1.0.0
tar -tzf dist/hyprspace-1.0.0.tar.gz
(cd dist && sha256sum --check SHA256SUMS)
git push origin v1.0.0
```

`make dist` requires a clean checkout at the matching version tag. It archives
committed files only, including the license, examples and screenshots. It leaves
the source archive and checksums in `dist/`. It never packages your local build.

Pushing the tag starts **Draft release**. Review its source archive, checksums
and notes in GitHub Releases, then publish the draft. Tags should keep pointing
at the tested commit; use a new patch version for a correction.

To retry manually, select the existing tag in the workflow's **Use workflow
from** selector and enter that same tag as the input. The packaging step rejects
a tag that points anywhere other than the commit checked by that workflow run.
An existing release causes creation to fail rather than overwriting its assets.

## Hyprland compatibility

The CI image pins an Arch snapshot and checks its Hyprland version before
building. Update `.github/ci/Dockerfile` when moving to another supported
compositor, then run the container build and live checks again.

After creating the release commit, add its full hash to `repository.commit_pins`
in `hyprpm.toml`, paired with the supported Hyprland commit. Commit that mapping
on the default branch so hyprpm can select the compatible source revision.
Keep earlier mappings when supporting additional Hyprland releases. The
[upstream guidelines](https://wiki.hypr.land/Plugins/Development/Plugin-Guidelines/)
describe the format. A pin must refer to a real, tested plugin commit, so it
cannot point at the commit that introduces the pin itself.

## First public release

Before changing repository visibility, review the committed files and history
for material you do not want to make public. In GitHub's repository settings,
set the description to "Live workspace overview and Alt+Tab switcher for
Hyprland". Useful topics are `hyprland`, `hyprland-plugin`, `wayland` and `omarchy`.

Make the repository public when you are ready for others to clone it, then
publish the reviewed release draft. The README's public clone and hyprpm URLs
work without authentication once the repository is public.
