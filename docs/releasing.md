# Releasing hyprspace

Published GitHub releases are the stable-version list. Each contains source and
SHA-256 checksums. Release Please opens a version PR and creates a draft and tag
after it is merged. The artifact workflow checks the exact tagged commit before
attaching files. A maintainer reviews and publishes the draft separately.

## Repository setup

In **Settings > Actions > General > Workflow permissions**, enable
**Allow GitHub Actions to create and approve pull requests**. The workflows use
the repository token with explicit permissions; no personal token is needed.
Enable a tag ruleset for `v*` that blocks updates and deletions while allowing
initial creation. Release tooling never moves an existing tag.

The repository is public. Stable installation still requires a maintainer to
review and publish a release after its checks and compatibility review pass.
No workflow publishes releases.

## Review the version PR

The single package uses the existing `type(scope): subject` commit convention.
Release Please starts at `1.0.0`; later feature and fix commits produce ordinary
semantic version bumps. Breaking changes use `!` or a `BREAKING CHANGE` footer.

The initial manifest value `0.0.0` means that no version has been released.
The plugin and `version.txt` already declare the planned `1.0.0`. Release Please
synchronizes the manifest, `version.txt`, `hyprpm.toml` and `src/Version.hpp` in
the first release PR, then keeps all four synchronized for later releases.
Do not keep a permanent `release-as` override.

For the first PR, fold the consolidated `Unreleased` notes into its generated
`1.0.0` entry, remove the empty `Unreleased` heading and the preparation sentence,
and remove duplicate bullets. The metadata check deliberately fails while
unreviewed notes remain. Keep only one entry per version. Subsequent entries
may use Release Please's dated plain or linked headings, including H3 patch
release headings. Include the supported compositor commit and validation evidence.

Update the README's selected installation tag only to the version being reviewed;
remove its first-release availability notice when preparing the initial public
release. Verify that any proposed compatibility change has the evidence below.
Review all changes and wait for Checks before merging.

The [Release Please configuration](https://github.com/googleapis/release-please/blob/main/docs/manifest-releaser.md)
sets `draft` and `force-tag-creation`. The latter creates the tag immediately,
so drafts do not leave the next release without a version anchor.

## Checks, artifacts and retries

Token-created PRs and tags do not automatically trigger ordinary PR or push
workflows. Release Please explicitly dispatches Checks on each release PR branch.
After creating a draft, it calls Draft release directly with the returned tag
and commit. This follows [GitHub's workflow triggering rules](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).

Draft release resolves the existing tag once and passes that exact commit to
every check and the packaging job. Any failed check blocks uploads. Packaging
requires a clean checkout at the matching tag and compares the remote tag to
the checked commit again before uploading. It archives committed source only;
local builds never enter the archive.

For a failed or incomplete draft, run **Draft release** manually from the current
main branch and enter the existing tag. The workflow checks out and tests that
tag, regardless of the branch selected in the workflow UI. Matching draft assets
are retained, missing assets are uploaded, and conflicting assets cause failure.
Published releases cannot be replaced. Correct a published release with a new
patch version. Avoid publishing a draft while its artifact job is running.

Retries help when a runner or upload failed. If the tagged source itself fails
validation, fix it on the main branch and prepare a new patch release. Keep the
failed tag unchanged and its draft unpublished. In particular, the initial
`v1.0.0` draft still contains unreviewed changelog notes in its tagged commit;
the corrected notes on the main branch belong to the next release candidate.

Review the draft's source archive, checksums, notes and compatibility evidence
before publishing it. Publishing and visibility changes remain maintainer actions.

## Compatibility and installation gate

The interactive overview remains unreleased until the final plugin and pinned
companions pass every automated suite and the physical/application checks in
[the interaction guide](interactive.md#verification). Host test counts or metadata
checks alone do not satisfy this gate. Attach the nested JSON results, companion
compatibility record and physical-session observations to the release review.
Routine testing must use the background runner. Physical tests require separate
explicit authorization for the session in which they run.

Also require the tagged candidate's hyprpm activation, startup loading, removal
and explicit version selection, plus the Nix build, package-content checks and
private compositor loading with the matching Nix compositor. Record evidence in
[the installation verification record](verification/stable-install.md).

The Arch CI image pins its dependency snapshot. The flake pins the supported
compositor commit and its dependencies. Update those pins only with successful
compatibility testing, including library ABI agreement. Explicit hyprpm release
tags take precedence over repository commit pins, so stable installation does
not require a later commit-pin maintenance commit.
