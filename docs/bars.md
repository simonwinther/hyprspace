# Space-aware workspace bars

A workspace bar must resolve the active space before displaying workspace
numbers or occupancy. Compositor-native IDs are global implementation identities;
workspace 1 in Hobby and workspace 1 in Work have different native IDs. Filtering
only by monitor does not provide space isolation.

Hyprspace exposes a lightweight interface for bars. It does not build overview
geometry or read capture/render diagnostics.

## Snapshot

Connect to `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprspace.sock`
with an `AF_UNIX`, `SOCK_SEQPACKET` socket. Send the packet `spaces` and read one
JSON reply. Each request uses its own connection.

```json
{
  "schema": 1,
  "type": "spaces",
  "enabled": true,
  "active": "active-space-uuid",
  "spaces": [
    {"id": "active-space-uuid", "name": "Hobby", "active": true,
     "windows": 3, "occupied_workspaces": 2}
  ],
  "workspaces": [
    {"id": "workspace-uuid", "number": 3, "name": "3", "native_id": 10003,
     "monitor": "eDP-1", "preferred_monitor": "eDP-1",
     "active": true, "visible": true, "focused": true,
     "occupied": true, "windows": 2, "special": false}
  ],
  "monitors": [
    {"name": "eDP-1", "workspace_uuid": "workspace-uuid",
     "number": 3, "focused": true}
  ]
}
```

`spaces` contains space summaries; `workspaces` contains **only the active
space's workspaces**. Display `number` as the local numeric label, and use `id`
for stable identity. A local number of zero denotes a named or special
workspace. `occupied` describes application occupancy, rather than the existence
of a saved empty workspace. `monitors` identifies each connected output's
selected local workspace; the same number identifies one workspace within the
space across all outputs.

A bar may show empty local slots as creation targets. Those slots must not be
marked occupied from another space's native workspace. Monitor colors and
visibility indicators must also come from the active-space snapshot.

## Clicks and scrolling

Send a packet with a `workspace ` prefix and JSON body:

```text
workspace {"monitor":"eDP-1","workspace":"3","space":"active-space-uuid"}
```

`workspace` accepts a local numeric selector or the native relative grammar,
including `e+1` and `e-1`. The compositor resolves it in the active space. The
`space` UUID must match the currently active space; a stale queued click is
rejected before changing focus or allocating a workspace. Clicking an already
selected slot leaves it selected even when workspace back-and-forth is enabled.
The reply is `{"success":true}` or `{"success":false,"error":"…"}`.

## Event-driven updates

Keep a connection to Hyprland's normal `.socket2.sock` event stream. Refresh
metadata after `custom>>hyprspace:spacechanged,<uuid>` and
`custom>>hyprspace:ready`. Clear displayed occupancy, active selection and stale
click targets after `custom>>hyprspace:unavailable`. Native output, workspace,
focus and window events can also trigger a refresh. Perform socket I/O outside
pointer and rendering callbacks, coalesce bursts, and redraw only changed state.
No periodic workspace polling is needed.

Reconnect after event-stream EOF. Query a new snapshot when reconnecting so
changes during the interruption are recovered. If the private socket is
unavailable, metadata is malformed or the schema is unsupported, clear the
space-dependent controls rather than falling back to global IDs. Native
single-space behavior is appropriate only after an explicit `enabled:false`
reply.

## Existing workspace slider

The installed `workspaces-slider` is a separate layer-shell surface centered
over Waybar; it is not Waybar's `hyprland/workspaces` module. Its maintained Rust
source is under `$XDG_CONFIG_HOME/waybar/workspaces-slider-rs` (normally
`~/.config/waybar/workspaces-slider-rs`). It retains the existing ten numbered
slots, theme, output colors and animated selection while using this interface.
The slots are local to the selected space. Named workspaces and numbers above
ten remain available through the overview and compositor bindings.

The slider's `--snapshot OUTPUT` diagnostic returns the exact local state used
by its live surface, without opening a window. Its systemd supervisor and
`workspaces-slider-launch` continue to manage one child per eligible output;
Waybar's hardware, audio, clock and tray modules need no replacement.

## Private regression

With a compiled slider, run the integration against a disposable three-output
compositor. It leaves the installed bar and physical desktop untouched:

```sh
python3 test/integration/space_bar.py --slider "$HOME/.cache/workspaces-slider-rs-target/release/workspaces-slider"
```

The check launches a real slider surface and exercises clicks and wheel input,
three-output active-space occupancy, independent reused local numbers,
restoration, and plugin unload/reload. Each artifact includes the tested plugin
and external slider binary hashes. The slider's Rust namespace/render checks run
with `cargo test --offline --locked --manifest-path "$HOME/.config/waybar/workspaces-slider-rs/Cargo.toml"`;
set `CARGO_TARGET_DIR` outside the dotfiles checkout.
