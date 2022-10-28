#pragma once

#include "envoy/connect/status.h"

#include "source/common/stats/symbol_table.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Connect {

/**
 * Captures symbolized representation for tokens used in connect stats. These are
 * broken out so they can be allocated early and used across all Connect-related
 * filters.
 */
struct StatNames {
  explicit StatNames(Stats::SymbolTable& symbol_table);

  Stats::StatNamePool pool_;
  Stats::StatName streams_total_;
  std::array<Stats::StatName, Status::WellKnownConnectStatus::MaximumKnown + 1> streams_closed_;
  absl::flat_hash_map<std::string, Stats::StatName> status_names_;
  // Stat name tracking the creation of the Buf connect client.
  Stats::StatName buf_connect_client_creation_;
};

} // namespace Connect
} // namespace Envoy
