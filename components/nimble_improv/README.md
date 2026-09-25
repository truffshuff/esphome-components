# nimble_improv

[Improv WiFi](https://www.improv-wifi.com/) provisioning over NimBLE: the
NimBLE counterpart of ESPHome's `esp32_improv`. Its behaviour follows
`esp32_improv` in ESPHome 2026.9.0.

## Configuration

```yaml
nimble_base:

nimble_improv:
  # All optional; defaults shown.
  authorizer: none            # a binary_sensor; while it is on, settings are accepted
  authorized_duration: 1min
  status_indicator: my_led    # a binary output; enables the Identify capability
  identify_duration: 10s
  wifi_timeout: 90s
  next_url: "https://example.com/?device={{device_name}}"
  on_start: ...
  on_provisioning: ...
  on_provisioned: ...
  on_state: ...               # state, error
  on_stop: ...
```

## When it runs

The service runs only while WiFi needs provisioning:

- at boot when there is no stored network, or
- after WiFi has been disconnected for `wifi_timeout`.

Once WiFi connects it reports PROVISIONED and stops advertising 10 seconds
later, so clients can read the final state. `esp32_improv` is started and
stopped by the WiFi component itself. That hook is not available to other
components, so `nimble_improv` watches WiFi state and applies the same
triggers.

ESPHome's WiFi schema only accepts a `wifi:` block with no network for
`esp32_improv` and `improv_serial`. For a device provisioned purely over
`nimble_improv`, declare a fallback `ap:` (see
`examples/nimble/nimble_improv_only.yaml`).

With no `authorizer`, provisioning is accepted as soon as a client connects,
which is also what `esp32_improv` does with `authorizer: none`. It is only
reachable while the device needs provisioning.

## Protocol

- Improv service `00467768-6228-2272-4663-277478268000` with Status, Error,
  RPC Command, RPC Result and Capabilities characteristics.
- Advertising: flags, the Improv service UUID, and the Improv service data
  (state and capabilities). The device name is advertised for one second every
  minute; the full name is always in the scan response.
- Commands: `WIFI_SETTINGS` (checksummed framing, 30 s connect timeout) and
  `IDENTIFY`. Anything else is answered with `ERROR_UNKNOWN_RPC`, as in
  `esp32_improv`.
- On success the credentials are saved by the WiFi component
  (`save_wifi_sta`), and the RPC result carries `next_url`, the Home Assistant
  link and, with `web_server`, the device URL.

A Device Information Service (manufacturer, model, serial, hardware, firmware
and software) is exposed alongside it; see `DEVICE_INFO.md`.

## Upgrading from the previous nimble_improv

- The service no longer runs permanently.
- `authorizer` is now a binary_sensor. The old output-based option could never
  authorize anything.
- Credentials the old component stored in its own preference slot are moved
  into the WiFi component once, on first boot.
- `GET_DEVICE_INFO` and `GET_WIFI_NETWORKS` are no longer answered (they are
  Improv-serial commands; `esp32_improv` does not answer them either).
- Advertising is handled here now, not by `nimble_proxy`.
