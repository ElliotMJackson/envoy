#include "source/common/connect/status.h"

namespace Envoy {
namespace Connect {

Status::ConnectStatus Utility::httpToConnectStatus(uint64_t http_response_status) {
  // From
  // https://github.com/connect/connect/blob/master/doc/http-connect-status-mapping.md.
  switch (http_response_status) {
  case 400:
    return Status::WellKnownConnectStatus::Internal;
  case 401:
    return Status::WellKnownConnectStatus::Unauthenticated;
  case 403:
    return Status::WellKnownConnectStatus::PermissionDenied;
  case 404:
    return Status::WellKnownConnectStatus::Unimplemented;
  case 429:
  case 502:
  case 503:
  case 504:
    return Status::WellKnownConnectStatus::Unavailable;
  default:
    return Status::WellKnownConnectStatus::Unknown;
  }
}

uint64_t Utility::connectToHttpStatus(Status::ConnectStatus connect_status) {
  // From https://cloud.buf.com/apis/design/errors#handling_errors.
  switch (connect_status) {
  case Status::WellKnownConnectStatus::Ok:
    return 200;
  case Status::WellKnownConnectStatus::Canceled:
    // Client closed request.
    return 499;
  case Status::WellKnownConnectStatus::Unknown:
    // Internal server error.
    return 500;
  case Status::WellKnownConnectStatus::InvalidArgument:
    // Bad request.
    return 400;
  case Status::WellKnownConnectStatus::DeadlineExceeded:
    // Gateway Time-out.
    return 504;
  case Status::WellKnownConnectStatus::NotFound:
    // Not found.
    return 404;
  case Status::WellKnownConnectStatus::AlreadyExists:
    // Conflict.
    return 409;
  case Status::WellKnownConnectStatus::PermissionDenied:
    // Forbidden.
    return 403;
  case Status::WellKnownConnectStatus::ResourceExhausted:
    //  Too many requests.
    return 429;
  case Status::WellKnownConnectStatus::FailedPrecondition:
    // Bad request.
    return 400;
  case Status::WellKnownConnectStatus::Aborted:
    // Conflict.
    return 409;
  case Status::WellKnownConnectStatus::OutOfRange:
    // Bad request.
    return 400;
  case Status::WellKnownConnectStatus::Unimplemented:
    // Not implemented.
    return 501;
  case Status::WellKnownConnectStatus::Internal:
    // Internal server error.
    return 500;
  case Status::WellKnownConnectStatus::Unavailable:
    // Service unavailable.
    return 503;
  case Status::WellKnownConnectStatus::DataLoss:
    // Internal server error.
    return 500;
  case Status::WellKnownConnectStatus::Unauthenticated:
    // Unauthorized.
    return 401;
  case Status::WellKnownConnectStatus::InvalidCode:
  default:
    // Internal server error.
    return 500;
  }
}

std::string Utility::connectStatusToString(Status::ConnectStatus connect_status) {
  switch (connect_status) {
  case Status::WellKnownConnectStatus::Ok:
    return "OK";
  case Status::WellKnownConnectStatus::Canceled:
    return "Canceled";
  case Status::WellKnownConnectStatus::Unknown:
    return "Unknown";
  case Status::WellKnownConnectStatus::InvalidArgument:
    return "InvalidArgument";
  case Status::WellKnownConnectStatus::DeadlineExceeded:
    return "DeadlineExceeded";
  case Status::WellKnownConnectStatus::NotFound:
    return "NotFound";
  case Status::WellKnownConnectStatus::AlreadyExists:
    return "AlreadyExists";
  case Status::WellKnownConnectStatus::PermissionDenied:
    return "PermissionDenied";
  case Status::WellKnownConnectStatus::ResourceExhausted:
    return "ResourceExhausted";
  case Status::WellKnownConnectStatus::FailedPrecondition:
    return "FailedPrecondition";
  case Status::WellKnownConnectStatus::Aborted:
    return "Aborted";
  case Status::WellKnownConnectStatus::OutOfRange:
    return "OutOfRange";
  case Status::WellKnownConnectStatus::Unimplemented:
    return "Unimplemented";
  case Status::WellKnownConnectStatus::Internal:
    return "Internal";
  case Status::WellKnownConnectStatus::Unavailable:
    return "Unavailable";
  case Status::WellKnownConnectStatus::DataLoss:
    return "DataLoss";
  case Status::WellKnownConnectStatus::Unauthenticated:
    return "Unauthenticated";
  case Status::WellKnownConnectStatus::InvalidCode:
  default:
    return "InvalidCode";
  }
}

} // namespace Connect
} // namespace Envoy
