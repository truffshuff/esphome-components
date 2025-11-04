#pragma once

// Shim header to redirect ESPHome API bluetooth_proxy references to nimble_proxy
// This allows the API component to work with NimBLE instead of Bluedroid

#ifndef BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE
#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE 5
#endif

#ifndef BLUETOOTH_PROXY_MAX_CONNECTIONS
#define BLUETOOTH_PROXY_MAX_CONNECTIONS 3
#endif

// Include the actual nimble_proxy implementation
#include "esphome/components/nimble_proxy/nimble_proxy.h"

namespace esphome {

// Create bluetooth_proxy namespace as an alias to nimble_proxy
// This allows API code that expects bluetooth_proxy:: to work with nimble_proxy
namespace bluetooth_proxy {
  using esphome::nimble_proxy::NimBLEProxy;

  // Forward declare the global pointer that will be defined in nimble_proxy.cpp
  // The API expects bluetooth_proxy::global_bluetooth_proxy
  extern NimBLEProxy *global_bluetooth_proxy;
}

}  // namespace esphome
