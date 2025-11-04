#pragma once

// Shim header to satisfy ESPHome API includes when the native bluetooth_proxy
// component isn't available in a pure NimBLE setup. We only need compile-time
// constants for advertisement batching and max connections.

#ifndef BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE
#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE 5
#endif

#ifndef BLUETOOTH_PROXY_MAX_CONNECTIONS
#define BLUETOOTH_PROXY_MAX_CONNECTIONS 3
#endif

namespace esphome {
namespace bluetooth_proxy {
// Empty placeholder namespace; real component not present in this build.
}
}  // namespace esphome
