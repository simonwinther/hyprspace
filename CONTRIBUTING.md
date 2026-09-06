# Contributing

For a bug report, include the steps to reproduce it, what you expected, and
`hyprctl version`. Mention your monitor layout and whether a window was tiled,
floating, maximized or fullscreen. Remove private window titles from logs.

Keep changes focused. Describe the behavior a user will notice and how you
checked it. The existing [reference](docs/guide.md) covers the plugin's layout,
rendering and input handling.

## Build and check

Follow the [installation instructions](README.md#install) for build dependencies.
Build fixtures use `ripgrep` and `jq`; release tooling uses Python 3.11 or later.

```bash
make -j2
make test
make -C test asan
make release-check
python3 test/test_release.py
make check
```

Host tests and build fixtures do not interact with your desktop. `make check`
compares the compiled plugin to the running compositor without loading it.
Run `make reload` when you want to activate your changes in your own session.

For the same compiler, compositor and dependency snapshot used in CI:

```bash
docker build --file .github/ci/Dockerfile --tag hyprspace-ci .
```

CI runs host tests with GCC and Clang on Ubuntu, sanitizers with GCC, release
tooling tests, and a full build against Hyprland 0.56.2 in Arch Linux. Container
builds do not exercise rendering or input inside a live compositor.

## Check desktop behavior

`make integration-test` runs in the background by default. It requires the
`wlroots-0.20` development package in addition to the dependencies listed in the
[interaction test notes](docs/interactive.md#verification). A private
display host renders three virtual monitors without opening desktop windows or
receiving physical input. Tests run at reduced CPU priority, although they still
share CPU, GPU and memory with other applications.

Use `--visible` only when you want to watch three test windows on your desktop.
The physical suite requires its separate `--run` option and temporarily takes over
the current desktop. Arrange those checks with the person using the machine;
routine validation should use the background runner. An unavailable background
backend is a test failure, never a reason to select a disruptive mode automatically.

For rendering, focus or input changes, check
opening and closing both overlays, Escape, selection by keyboard and mouse,
fullscreen and maximized windows, and more than one workspace. Check window
move/resize and multiple monitors when the change affects them.

Add tests for layout, state handling and build failures where they can run
without a compositor. Follow the existing C++ style in `.clang-format` and
format the files you touch with `clang-format -i`.

For documentation images, follow the [screenshot notes](docs/screenshots/README.md).
For a version bump, follow [the release process](docs/releasing.md).
