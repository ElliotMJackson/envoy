#pragma once

#include <cstdint>
#include <string>

#include "envoy/connect/status.h"

namespace Envoy {
namespace Connect {

/**
 * Connect::Status utilities.
 */
class Utility {
public:
  /**
   * Returns the Connect status code from a given HTTP response status code. Ordinarily, it is expected
   * that a 200 response is provided, but Connect defines a mapping for intermediaries that are not
   * Connect aware, see https://github.com/connect/connect/blob/master/doc/http-connect-status-mapping.md.
   * @param http_response_status HTTP status code.
   * @return Status::ConnectStatus corresponding Connect status code.
   */
  static Status::ConnectStatus httpToConnectStatus(uint64_t http_response_status);

  /**
   * @param connect_status Connect status from connect-status header.
   * @return uint64_t the canonical HTTP status code corresponding to a Connect status code.
   */
  static uint64_t connectToHttpStatus(Status::ConnectStatus connect_status);

  /**
   * @param connect_status Connect status from connect-status header.
   * @return Connect status string converted from connect-status.
   */
  static std::string connectStatusToString(Status::ConnectStatus connect_status);
};

} // namespace Connect
} // namespace Envoy
