# Run configurations in PyCharm

## Server

Open the repository root in PyCharm. The `Server` group contains:

| Configuration | Action |
| --- | --- |
| Server | Run `server/main.py`, loading environment variables from `server/.env` |
| Server Tests | Run `server/tests/` with PyCharm's pytest runner, without loading `.env` |

Both configurations use `server/.venv/bin/python` and `server/` as the working
directory. Create the environment first with `cd server && uv sync --locked`.
If needed, select this existing interpreter in Run → Edit Configurations.
Both **Run** and **Debug** are supported; Server Tests displays results in the
IDE test runner. Server requires the settings described in
[server/docs/server.md](../server/docs/server.md).

## Firmware

Open the repository root in PyCharm. The shared Run configurations appear in
the `Firmware` group:

| Configuration | PlatformIO command |
| --- | --- |
| Build reterminal_e1005 | `pio run -e reterminal_e1005` |
| Upload and Monitor reterminal_e1005 | `pio run -e reterminal_e1005 -t upload -t monitor` |
| Monitor reterminal_e1005 | `pio device monitor -e reterminal_e1005` |
| Native Tests | `pio test -e native` |

All configurations run the `platformio` Python module from `firmware/` using
`$USER_HOME$/.platformio/penv/bin/python`. If PlatformIO is installed elsewhere,
change the Python interpreter in Run → Edit Configurations. No PlatformIO IDE
plugin is required. Use **Run**, rather than Python Debug, for these commands.

Upload builds the firmware first and opens the monitor only after a successful
upload. Both monitor configurations emulate a terminal and use the environment's
serial settings from `firmware/platformio.ini` (115200 baud, RTS/DTR disabled).
PlatformIO detects the serial port automatically. If multiple devices are
connected, append `--port /dev/cu.usbmodem…` to the configuration's parameters;
for Upload and Monitor this sets both the upload and monitor ports.

Stop an existing monitor before uploading so it releases the serial port.
In the monitor console, Ctrl+C exits and Ctrl+T opens the PlatformIO menu.

Firmware builds require `firmware/src/secrets.h`; for a new checkout, create it
from `firmware/src/secrets.example.h` and fill in local settings. Native Tests
builds and runs all host test suites without a connected device or credentials;
it requires a host C++ compiler.
