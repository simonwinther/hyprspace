# Empty overview refresh crash

The September 10 crash report and core dump identify a null dereference in
`COverview::selectTarget`, called by `refreshWindows` during `prepareFrame`.
The overview retained one tile with key zero while its workspace entry vector
had null storage and no entries. The fault occurred at `selectTarget + 0x7b`.

`refreshWindows` replaces the entry vector each frame. When collection returns
no workspaces, `computeLayout` previously returned without clearing the old
tiles. Restoring the previous selection then indexed the empty entry vector.
This can also leave stale keys for rendering and pointer hit testing.

The fix clears tile keys and the animation anchor before rebuilding the layout,
clears selection and hover state for an empty result or missing monitor, and
keeps the selected index valid as tiles return or disappear.

## Validation

All compositor tests used the default background runner with three private
virtual monitors. No test windows or injected input reached the desktop.

The test fixture replaces a populated overview's entries with an empty vector
and invokes the actual layout and selection methods. It then restores the
entries and verifies selection recovery on every monitor. The fixture is a
separate library; it is not part of the installed plugin.

- The source from before the fix reproduced SIGSEGV at `selectTarget + 0x7b`.
  Artifacts: `/tmp/hs-i.0mtoiias`.
- The fixed overview passed the empty-state regression and 240 Control+A and
  Super+A toggles, including reopening during closing, with animations enabled
  and disabled. Every transition asserted its open or closed state. Teardown
  restored window alpha, released input ownership and modifiers, and allowed
  ordinary application typing. Artifacts: `/tmp/hs-i.t3sx2bql`.
- Workspace transfer, disappearing clients, empty workspace drops, monitor
  removal and hotplug passed. Artifacts: `/tmp/hs-i.j1poes4y`.
- Keyboard navigation, concurrent launch destinations, repeat/release bindings,
  submaps and typing after dismissal passed. Artifacts: `/tmp/hs-i.a_9nanbp`.
- Default bindings, Alt+Tab, plugin unloading and reloading passed.
  Artifacts: `/tmp/hs-i.tqpfr5rq`.
- Host validation passed 329,103 C++ checks, 69 build/reload checks, seven launch
  helper tests and 12 runner isolation tests.

The deterministic regression recreates the state observed in the core dump;
it does not establish which compositor event emptied the list during the
reported rapid toggles.

The tested binary was installed through the atomic reload script. Its ABI
matched the running Hyprland, installed and built files matched, the running
plugin reported version 1.0.2, and configuration errors were empty.

## Safe-mode configuration error

This is a separate Hyprland 0.56.2 recovery issue. Safe mode selects
`recoverycfg.lua`, which creates the Lua configuration manager. The recovery
dialog's load action clears the safe-mode flag and calls the existing manager's
reload method without selecting a new backend. A legacy `.conf` file is then
parsed as Lua, producing the error at its `#` comment.

The installed configuration loads without errors in a normal session. No
configuration migration or compositor modification was made.

Source references for the installed compositor revision:

- [Recovery configuration path](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/config/supplementary/jeremy/Jeremy.cpp)
- [Configuration manager selection](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/config/ConfigManager.cpp)
- [Recovery dialog load action](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/Compositor.cpp)
