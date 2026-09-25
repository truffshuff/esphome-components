# nimble_proxy (retired)

`nimble_proxy` has been replaced. The NimBLE Bluetooth proxy now runs
ESPHome 2026.9.0's own `bluetooth_proxy` and `bluetooth_connection` code
(vendored in this repository, see `../VENDORED.md`) on top of:

- `nimble_tracker`: scanning, the `BLEHub` implementation
- a NimBLE GATT backend (`bluetooth_connection/bluetooth_connection_nimble.*`)

Home Assistant therefore sees the same protocol behaviour as from a Bluedroid
proxy. The old implementation differed in ways that broke active connections
and notifications; `../../docs/NIMBLE_PARITY.md` lists the parity status.

A config that still uses `nimble_proxy:` fails validation with the replacement
config. The option mapping is in `../nimble_tracker/README.md`.
