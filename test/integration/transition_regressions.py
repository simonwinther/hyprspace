"""Destination zoom, native handoff and preview stacking across outputs."""

import time

from PIL import Image
from resize import area
from zoom import center, inspection, tile


def tap(s, code):
    s.key(code, 1)
    s.key(code, 0)


def focus_source(s):
    s.ctl('dispatch', 'focusmonitor', s.names[0])
    s.ctl('dispatch', 'workspace', '10')
    s.ctl('dispatch', 'focuswindow', 'address:' + s.windows()['hs-A']['address'])
    s.move(s.point(s.windows()['hs-A'], .5, .5))
    assert s.data('activeworkspace')['id'] == 10
    assert next(m for m in s.data('monitors') if m['focused'])['name'] == s.names[0]


def prepared(s, wait_for):
    s.env['HS_CONTENT_MARKER'] = 'hs-B'
    s.setup('dwindle', 0, 1)
    s.ctl('keyword', 'workspace', f'10,monitor:{s.names[0]},persistent:true,layout:dwindle')
    for workspace in (1, 2):
        s.ctl('keyword', 'workspace', f'{workspace},monitor:{s.names[1]},persistent:true,layout:dwindle')
    s.ctl('dispatch', 'movetoworkspacesilent', f'2,address:{s.windows()["hs-B"]["address"]}')
    s.ctl('dispatch', 'moveworkspacetomonitor', f'2 {s.names[1]}')
    s.ctl('dispatch', 'focusmonitor', s.names[1])
    s.ctl('dispatch', 'workspace', '1')
    s.ctl('dispatch', 'moveworkspacetomonitor', f'1 {s.names[1]}')
    s.ctl('dispatch', 'focusmonitor', s.names[1])
    s.ctl('dispatch', 'workspace', '1')
    s.ctl('dispatch', 'movetoworkspacesilent', f'10,address:{s.windows()["hs-A"]["address"]}')
    s.ctl('dispatch', 'moveworkspacetomonitor', f'10 {s.names[0]}')
    focus_source(s)
    monitors = {m['name']: m for m in s.data('monitors')}
    assert monitors[s.names[1]]['activeWorkspace']['id'] == 1
    assert s.windows()['hs-B']['monitor'] == monitors[s.names[1]]['id']
    s.ctl('keyword', 'animations:enabled', 'true')
    s.ctl('keyword', 'animation', 'windowsMove,1,12,default')
    s.ctl('keyword', 'animation', 'workspaces,1,12,default,slide')
    s.ctl('dispatch', 'hyprspace:overview', 'on')
    wait_for(lambda: len(s.status()['views']) == 3)
    time.sleep(1.4)
    s.move(center(tile(s, 10)))
    assert s.status()['target']['workspace'] == 10


def toggle(s):
    s.key(125, 1)
    tap(s, 30)  # Super+A uses the native binding matcher.
    s.key(125, 0)


def toggle_destinations(s, wait_for):
    s.ctl('keyword', 'workspace', f'21,monitor:{s.names[0]},persistent:true')
    for follow, destination, magnify in ((True, 21, False), (True, 2, True), (False, 21, False)):
        s.ctl('keyword', 'animations:enabled', 'false')
        focus_source(s)
        s.ctl('keyword', 'plugin:hyprspace:follow_mouse', 'true')
        s.ctl('keyword', 'animations:enabled', 'true')
        toggle(s)
        wait_for(lambda: s.status()['live'])
        time.sleep(1.4)
        old_workspace = 1 if destination == 2 else 10
        normal = tile(s, destination)
        old = tile(s, old_workspace)
        if follow:
            s.move(center(normal))
        else:
            s.move(center(tile(s, 10)))
            s.ctl('keyword', 'plugin:hyprspace:follow_mouse', 'false')
            tap(s, 107)  # End selects workspace 21 on the source output.
            s.move(center(tile(s, 2)))
        assert s.status()['target']['workspace'] == destination
        if magnify:
            s.key(44, 1)
            inspection(s, wait_for, 1)
            tap(s, 13)
            inspection(s, wait_for, 1.15)
            s.move(center(tile(s, destination)))
            s.key(57, 1)
            wait_for(lambda: s.status()['zoom']['panning'])
            s.motion(8, 4)
            old = tile(s, old_workspace)
        toggle(s)
        assert not s.status()['live']
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '.25')
        try:
            assert tile(s, destination)['w'] > normal['w'] * 1.2
            assert tile(s, old_workspace)['w'] <= old['w'] + 1, ('toggle zoomed into the old workspace', old, tile(s, old_workspace))
            assert s.data('activeworkspace')['id'] == destination
        finally:
            s.ctl('dispatch', 'hyprspace-test:transition-frame', '0')
            s.key(57, 0)
            s.key(44, 0)
        wait_for(lambda: not s.status()['views'])
        s.check(f'Super+A commits workspace {destination} directly with follow_mouse={follow} and inspection={magnify}')

    s.ctl('keyword', 'animations:enabled', 'false')
    s.ctl('keyword', 'plugin:hyprspace:follow_mouse', 'true')
    for cancel in ('off', 'close', 'escape'):
        s.ctl('dispatch', 'focusmonitor', s.names[1])
        s.ctl('dispatch', 'workspace', '1')
        focus_source(s)
        before = {m['name']: m['activeWorkspace']['id'] for m in s.data('monitors')}
        toggle(s)
        wait_for(lambda: s.status()['live'])
        s.move(center(tile(s, 2)))
        assert s.status()['target']['workspace'] == 2
        if cancel == 'escape':
            tap(s, 1)
        else:
            s.ctl('dispatch', 'hyprspace:overview' if cancel == 'off' else 'hyprspace:close', *(['off'] if cancel == 'off' else []))
        wait_for(lambda: not s.status()['views'])
        assert {m['name']: m['activeWorkspace']['id'] for m in s.data('monitors')} == before
    s.check('explicit off, close and Escape still dismiss without committing the hovered destination')

    focus_source(s)
    toggle(s)
    wait_for(lambda: s.status()['live'])
    s.move(s.preview_point('hs-A'))
    s.key(125, 1)
    s.button(1)
    assert s.status()['dragging']
    tap(s, 30)
    s.button(0)
    s.key(125, 0)
    wait_for(lambda: not s.status()['views'])
    assert s.windows()['hs-A']['workspace']['id'] == 10
    s.check('Super+A during a provisional drag dismisses safely and consumes the late button release')


def run(s, wait_for):
    fixture = s.artifact('test-overview.so')
    s.ctl('plugin', 'load', str(fixture))
    time.sleep(.15)
    frozen = False
    try:
        prepared(s, wait_for)
        before = tile(s, 2)
        old = tile(s, 1)
        tap(s, 3)  # Digit 2 from workspace 10 on the other output.
        assert not s.status()['live'], 'numeric selection left another output interactive'
        growing = wait_for(lambda: (value if (value := tile(s, 2))['w'] > before['w'] * 1.2 else None))
        assert tile(s, 1)['w'] <= old['w'] + 1, 'destination output zoomed into its old workspace'
        assert growing['w'] > before['w']
        s.check('digit 2 from another output zooms into workspace 2 and closes every overview')

        # Freeze an actual rendered close frame so screenshot timing cannot
        # miss previews incorrectly drawn above the expanding destination.
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '.25')
        frozen = True
        time.sleep(.08)
        destination, other = s.preview('hs-B'), s.preview('hs-C')
        left = max(destination['x'], other['x']) + 8
        top = max(destination['y'], other['y']) + 8
        right = min(destination['x'] + destination['w'], other['x'] + other['w']) - 8
        bottom = min(destination['y'] + destination['h'], other['y'] + other['h']) - 8
        assert right > left and bottom > top, (destination, other)
        monitor = next(m for m in s.data('monitors') if m['name'] == s.names[1])
        path = s.root / 'destination-above-previews.png'
        s.run('grim', '-s', '1', '-o', s.names[1], str(path))
        with Image.open(path) as picture:
            pixel = picture.convert('RGB').getpixel((round((left + right) / 2 - monitor['x']), round((top + bottom) / 2 - monitor['y'])))
        assert all(abs(value - expected) <= 8 for value, expected in zip(pixel, (36, 229, 87))), pixel
        s.check('expanding opaque destination covers other workspace previews in the rendered close frame')

        # Check the endpoint before removing the overlay, then the desktop
        # after handoff, including native workspace offsets and visibility.
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '.011')
        near = s.preview('hs-B')
        native = s.windows()['hs-B']
        assert all(abs(near[part] - value) <= 12 for part, value in zip(('x', 'y', 'w', 'h'), native['at'] + native['size'])), (near, native)
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '0')
        frozen = False
        wait_for(lambda: not s.status()['views'])
        assert s.data('activewindow')['address'] == s.windows()['hs-B']['address']
        assert s.data('activeworkspace')['id'] == 2
        assert all(window['alpha'] == 1 for window in s.status()['windows'])
        path = s.root / 'destination-desktop-handoff.png'
        s.run('grim', '-s', '1', '-o', s.names[1], str(path))
        with Image.open(path) as picture:
            picture = picture.convert('RGB')
            native = s.windows()['hs-B']
            for fraction in (.1, .5, .9):
                point = (round(native['at'][0] + native['size'][0] * fraction - monitor['x']),
                         round(native['at'][1] + native['size'][1] * fraction - monitor['y']))
                pixel = picture.getpixel(point)
                assert all(abs(value - expected) <= 8 for value, expected in zip(pixel, (36, 229, 87))), (point, pixel)
        work = area(monitor)
        assert abs(s.windows()['hs-B']['at'][0] - work['x']) < 30
        s.check('cross-output zoom hands off to the selected desktop window with restored visibility')
        toggle_destinations(s, wait_for)

        # Persistent empty remote destinations have a tile too and must focus
        # their output, rather than retain focus on the source client.
        s.ctl('keyword', 'animations:enabled', 'false')
        focus_source(s)
        s.ctl('dispatch', 'hyprspace:overview', 'on')
        wait_for(lambda: s.status()['live'])
        s.move(center(tile(s, 10)))
        tap(s, 2)  # Digit 1: persistent empty workspace on output 1.
        wait_for(lambda: not s.status()['views'])
        assert s.data('activeworkspace')['id'] == 1
        assert not s.data('activewindow').get('address')
        assert next(m for m in s.data('monitors') if m['focused'])['name'] == s.names[1]
        s.check('numeric selection of an empty remote tile focuses its output and clears old window focus')

        # A workspace rule can bind a not-yet-created number to another output.
        # There is no destination tile to find when this key is pressed.
        s.ctl('keyword', 'workspace', f'9,monitor:{s.names[1]},persistent:false')
        assert not any(ws['id'] == 9 for ws in s.data('workspaces'))
        focus_source(s)
        s.ctl('keyword', 'animations:enabled', 'true')
        s.ctl('dispatch', 'hyprspace:overview', 'on')
        time.sleep(1.4)
        s.move(center(tile(s, 10)))
        previous = tile(s, 1)
        tap(s, 10)  # Digit 9: fresh workspace bound to the remote output.
        assert not s.status()['live']
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '.25')
        frozen = True
        assert tile(s, 1)['w'] <= previous['w'] + 1, 'empty remote destination zoomed into its old workspace'
        assert s.data('activeworkspace')['id'] == 9
        assert next(m for m in s.data('monitors') if m['focused'])['name'] == s.names[1]
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '0')
        frozen = False
        wait_for(lambda: not s.status()['views'])
        s.check('a fresh workspace bound to another output fades that output into its empty desktop')

        s.ctl('keyword', 'animations:enabled', 'false')
        s.ctl('keyword', 'plugin:hyprspace:overview:all_monitors', 'false')
        focus_source(s)
        s.ctl('keyword', 'animations:enabled', 'true')
        s.ctl('dispatch', 'hyprspace:overview', 'on')
        time.sleep(1.4)
        assert len(s.status()['views']) == 1
        s.move(center(tile(s, 10)))
        source = tile(s, 10)
        tap(s, 3)
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '.25')
        frozen = True
        assert tile(s, 10)['w'] > source['w'] * 1.2, 'uncovered destination collapsed the source grid'
        assert s.data('activeworkspace')['id'] == 2
        s.ctl('dispatch', 'hyprspace-test:transition-frame', '0')
        frozen = False
        wait_for(lambda: not s.status()['views'])
        s.check('single-output overview restores its source desktop when selecting an uncovered remote workspace')

        s.ctl('keyword', 'animations:enabled', 'false')
        s.ctl('keyword', 'plugin:hyprspace:overview:all_monitors', 'true')
        s.ctl('keyword', 'plugin:hyprspace:follow_mouse', 'true')
        s.ctl('keyword', 'workspace', '21,persistent:false')
        s.ctl('keyword', 'binds:workspace_back_and_forth', 'true')
        # Keep native focus on workspace 2 while pointer selection names 10,
        # so a string resolver would reinterpret this as back-and-forth.
        s.ctl('dispatch', 'focusmonitor', s.names[1])
        s.ctl('dispatch', 'workspace', '2')
        s.ctl('dispatch', 'focuswindow', 'address:' + s.windows()['hs-B']['address'])
        s.ctl('dispatch', 'hyprspace:overview', 'on')
        wait_for(lambda: s.status()['live'])
        s.move(center(tile(s, 10)))
        tap(s, 3)  # Workspace 2 is already active on its output.
        wait_for(lambda: not s.status()['views'])
        assert s.data('activeworkspace')['id'] == 2
        assert s.data('activewindow')['address'] == s.windows()['hs-B']['address']
        s.check('explicit numeric selection stays on an already-active remote workspace with back-and-forth enabled')

        s.ctl('keyword', 'binds:workspace_back_and_forth', 'false')
        s.ctl('keyword', 'plugin:hyprspace:overview:include_special', 'true')
        s.ctl('dispatch', 'focusmonitor', s.names[1])
        s.ctl('dispatch', 'togglespecialworkspace', 'transition-empty')
        special = next(ws['id'] for ws in s.data('workspaces') if ws['name'] == 'special:transition-empty')
        focus_source(s)
        s.ctl('dispatch', 'hyprspace:overview', 'on')
        wait_for(lambda: s.status()['live'])
        s.move(center(tile(s, special)))
        tap(s, 28)  # Enter into the empty special tile on the remote output.
        wait_for(lambda: not s.status()['views'])
        monitors = {m['name']: m for m in s.data('monitors')}
        assert monitors[s.names[1]]['specialWorkspace']['id'] == special
        assert monitors[s.names[0]]['specialWorkspace']['id'] == 0
        assert monitors[s.names[1]]['focused']
        assert not s.data('activewindow').get('address')
        s.ctl('dispatch', 'togglespecialworkspace', 'transition-empty')
        s.check('empty special tile selection focuses its owning output without moving the special workspace')
    finally:
        if frozen:
            s.ctl('dispatch', 'hyprspace-test:transition-frame', '0')
        s.ctl('keyword', 'animations:enabled', 'false')
        s.close()
        s.ctl('keyword', 'plugin:hyprspace:overview:all_monitors', 'true')
        s.ctl('keyword', 'binds:workspace_back_and_forth', 'false')
        s.ctl('keyword', 'plugin:hyprspace:follow_mouse', 'true')
        s.ctl('keyword', 'workspace', '21,persistent:false')
        s.env.pop('HS_CONTENT_MARKER', None)
        s.ctl('plugin', 'unload', str(fixture))
