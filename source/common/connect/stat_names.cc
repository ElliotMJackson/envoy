#include "source/common/connect/stat_names.h"

namespace Envoy {
namespace Connect {

StatNames::StatNames(Stats::SymbolTable& symbol_table)
    : pool_(symbol_table), streams_total_(pool_.add("streams_total")),
      buf_connect_client_creation_(pool_.add("buf_connect_client_creation")) {
  for (uint32_t i = 0; i <= Status::WellKnownConnectStatus::MaximumKnown; ++i) {
    std::string status_str = absl::StrCat(i);
    streams_closed_[i] = pool_.add(absl::StrCat("streams_closed_", status_str));
    status_names_[status_str] = pool_.add(status_str);
  }
}

} // namespace Connect
} // namespace Envoy
