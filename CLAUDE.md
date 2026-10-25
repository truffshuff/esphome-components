# esphome-components — Claude Guidelines

ESPHome external components consumed by the Halo firmware (`truffshuff/halo`, local
clone `~/Projects/halo/halo`; read its `CLAUDE.md` and `docs/` for the device
side). Requires **ESPHome 2026.10.0** (`nimble_improv` uses its `improv_base` API); the
vendored components and the parity baseline are still **2026.9.0**. ESP-IDF 5.5.x, native
`esp-idf` toolchain.

- Working branch: `nimble-bluedroid-parity`. Remotes: `origin` = truffshuff (fork),
  `upstream` = yashmulgaonkar. The original author is buglloc (`CODEOWNERS` in
  `axs15231` and `pinky_winky` still say so; leave them).
- Halo pins this repo **by commit SHA** in two files
  (`packages/system/esphome_core.yaml`, `packages/features/ble/ble_improv.yaml`). A commit
  here reaches devices only after both pins move and halo's `modular` branch is pushed.
  Comment/doc-only commits here need no repin.

## What lives here

| Component | Origin | Notes |
|---|---|---|
| `axs15231` | buglloc, trimmed | Touchscreen platform only (display is core `mipi_spi`). Single-finger filter for LilyGo's phantom touches. |
| `weather_helpers` | this fork | Header-only; `__init__.py` does nothing but make ESPHome copy the header. |
| `nimble_base`, `nimble_tracker`, `nimble_improv` | this fork | NimBLE counterparts of `esp32_ble`, `esp32_ble_tracker`, `improv_ble` (`esp32_improv` before 2026.10.0). |
| `ble_device_base`, `bluetooth_connection`, `bluetooth_proxy` | **vendored ESPHome 2026.9.0** | Override the built-ins for NimBLE builds. |
| `pinky_winky` | buglloc, ported to `ble_device_base` | Works on either tracker. |
| `nimble_proxy` | retired | Fails validation with migration instructions; keep it that way. |

## Rules for the vendored components

`components/VENDORED.md` is the contract. In short:

- Files it lists as **byte-identical to upstream** must stay byte-identical — do not
  "fix" comments or style in them; they are diffed against ESPHome on every re-sync.
- Every local C++ change is a guard marked `[nimble]`; `grep -rn "\[nimble\]"` must list
  all of them. Update VENDORED.md's table when adding or removing one.
- The three `__init__.py` files are rewritten, not copied; carry upstream schema changes
  into them by hand when re-syncing.

## Conventions in the fork's own code

- NimBLE callbacks run on the NimBLE host task. They only copy into an `EventRing`
  (`nimble_base/nimble_event_ring.h`, PSRAM via `RAMAllocator`) and all ESPHome work
  happens in `loop()`. Keep it that way.
- NimBLE host headers stay out of `.h` files (their `LOG_LEVEL_*`/`MIN`/`MAX` macros
  collide with ESPHome); `.cpp` files include them and `#undef` the log macros.
- Comments explain *why*, and cite the ESPHome/IDF behaviour being matched
  ("esp32_ble_tracker parity: ..."). Parity claims belong in `docs/NIMBLE_PARITY.md`;
  keep its hardware-results section and test plan current when behaviour is verified.
- When code changes a component's YAML surface or behaviour, update that component's
  `README.md` and, for Improv, `DEVICE_INFO.md` — both have drifted before.

## Validation

- A local ESPHome must be ≥ 2026.10.0 to validate or compile the examples.
- `examples/nimble/nimble_both.yaml` compiling cleanly is the contract check for the
  vendored hub/GATT interfaces (VENDORED.md step 4).
- Never run `esphome run` / `upload` / `logs` against a device without being asked.
- Commit messages: imperative mood, e.g. `Size the scan queue for main-loop stalls`.
