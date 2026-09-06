# Desktop testing

Use the default background integration runner for routine validation. It keeps
rendering and injected input inside a private display server.

Do not use `--visible` or the physical desktop suite unless the user explicitly
requests disruptive desktop testing for the current work. A request to check or
fix the plugin does not by itself request those modes.

If background startup fails, diagnose it in isolation. Do not fall back to opening
test windows or injecting input into the user's desktop.
