# Background integration testing, September 6, 2026

The integration runner now defaults to an invisible display host. No visible
runner mode, physical desktop suite or live plugin reload was used for this change.
At the end of that verification, the built and installed plugin were byte-identical:
`a4397140a611ae41db0ac435504c874d3252b6720b1bad0364506b584acd36d0`.

The final run used Hyprland 0.56.2, Aquamarine 0.14, wlroots 0.20.2 and the patched
Walker 2.17.0 / Elephant 2.22.0 companions. The private display host rendered on
the NVIDIA GTX 1080 Ti through its render node, with no physical output or input
backend. Hyprland's log confirmed a sessionless backend and refusal of the
private, unavailable seatd socket.

| Check | Result |
| --- | --- |
| Complete background suite with companions and Firefox | 110 passed in one run |
| Layout and output pairs within that suite | All 27 passed with identical starting layouts |
| Host assertions | 327,265 passed |
| Build and reload fixtures | 69 passed, using simulated compositor calls |
| Launch helper tests | 7 passed |
| Runner isolation and process cleanup tests | 10 passed |
| SIGINT and SIGTERM during background interaction | Both stopped the compositor, display host and clients |
| Physical runner without its opt-in flag | Refused before opening a desktop connection |
| Formatting and diff checks | Passed |

An initial background run stopped after 54 checks because dwindle's starting tree
depended on window map order and the previous cursor position. The runner now
maps fixture windows in order, seeds cursor position, and compares the starting
geometry before testing native and overview gestures. The complete 110-check run
passed after that correction in `/tmp/hs-i.d21kkwii`.

Logs, source hashes, the final result and cancellation records are retained under
`build/verification/background-tests-2026-09-06/`. A final process scan found no
remaining compositor, display host or client processes from these test sessions.

This run does not repeat Discord or physical hardware checks. Hardware cursor
planes, physical hotplug and installed desktop services still require separately
scheduled physical tests. Background tests share CPU, GPU and memory resources
with the desktop; this run does not certify game performance while testing.
