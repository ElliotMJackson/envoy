#include "source/common/connect/common.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/base64.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/fmt.h"
#include "source/common/common/macros.h"
#include "source/common/common/safe_memcpy.h"
#include "source/common/common/utility.h"
#include "source/common/http/header_utility.h"
#include "source/common/http/headers.h"
#include "source/common/http/message_impl.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"

#include "absl/container/fixed_array.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Connect {

bool Common::hasConnectContentType(const Http::RequestOrResponseHeaderMap& headers) {
  const absl::string_view content_type = headers.getContentTypeValue();
  // Content type is Connect if it is exactly "application/connect" or starts with
  // "application/connect+". Specifically, something like application/connect-web is not Connect.
  return absl::StartsWith(content_type, Http::Headers::get().ContentTypeValues.Connect) &&
         (content_type.size() == Http::Headers::get().ContentTypeValues.Connect.size() ||
          content_type[Http::Headers::get().ContentTypeValues.Connect.size()] == '+');
}

bool Common::hasProtobufContentType(const Http::RequestOrResponseHeaderMap& headers) {
  return headers.getContentTypeValue() == Http::Headers::get().ContentTypeValues.Protobuf;
}

bool Common::isConnectRequestHeaders(const Http::RequestHeaderMap& headers) {
  if (!headers.Path()) {
    return false;
  }
  return hasConnectContentType(headers);
}

bool Common::isProtobufRequestHeaders(const Http::RequestHeaderMap& headers) {
  if (!headers.Path()) {
    return false;
  }
  return hasProtobufContentType(headers);
}

bool Common::isConnectResponseHeaders(const Http::ResponseHeaderMap& headers, bool end_stream) {
  if (end_stream) {
    // Trailers-only response, only connect-status is required.
    return headers.ConnectStatus() != nullptr;
  }
  if (Http::Utility::getResponseStatus(headers) != enumToInt(Http::Code::OK)) {
    return false;
  }
  return hasConnectContentType(headers);
}

absl::optional<Status::ConnectStatus>
Common::getConnectStatus(const Http::ResponseHeaderOrTrailerMap& trailers, bool allow_user_defined) {
  const absl::string_view connect_status_header = trailers.getConnectStatusValue();
  uint64_t connect_status_code;

  if (connect_status_header.empty()) {
    return absl::nullopt;
  }
  if (!absl::SimpleAtoi(connect_status_header, &connect_status_code) ||
      (connect_status_code > Status::WellKnownConnectStatus::MaximumKnown && !allow_user_defined)) {
    return {Status::WellKnownConnectStatus::InvalidCode};
  }
  return {static_cast<Status::ConnectStatus>(connect_status_code)};
}

absl::optional<Status::ConnectStatus> Common::getConnectStatus(const Http::ResponseTrailerMap& trailers,
                                                         const Http::ResponseHeaderMap& headers,
                                                         const StreamInfo::StreamInfo& info,
                                                         bool allow_user_defined) {
  // The Connect specification does not guarantee a Connect status code will be returned from a Connect
  // request. When it is returned, it will be in the response trailers. With that said, Envoy will
  // treat a trailers-only response as a headers-only response, so we have to check the following
  // in order:
  //   1. trailers Connect status, if it exists.
  //   2. headers Connect status, if it exists.
  //   3. Inferred from info HTTP status, if it exists.
  const std::array<absl::optional<Connect::Status::ConnectStatus>, 3> optional_statuses = {{
      {Connect::Common::getConnectStatus(trailers, allow_user_defined)},
      {Connect::Common::getConnectStatus(headers, allow_user_defined)},
      {info.responseCode() ? absl::optional<Connect::Status::ConnectStatus>(
                                 Connect::Utility::httpToConnectStatus(info.responseCode().value()))
                           : absl::nullopt},
  }};

  for (const auto& optional_status : optional_statuses) {
    if (optional_status.has_value()) {
      return optional_status;
    }
  }

  return absl::nullopt;
}

std::string Common::getConnectMessage(const Http::ResponseHeaderOrTrailerMap& trailers) {
  const auto entry = trailers.ConnectMessage();
  return entry ? std::string(entry->value().getStringView()) : EMPTY_STRING;
}

absl::optional<buf::rpc::Status>
Common::getConnectStatusDetailsBin(const Http::HeaderMap& trailers) {
  const auto details_header = trailers.get(Http::Headers::get().ConnectStatusDetailsBin);
  if (details_header.empty()) {
    return absl::nullopt;
  }

  // Some implementations use non-padded base64 encoding for connect-status-details-bin.
  // This is effectively a trusted header so using the first value is fine.
  auto decoded_value = Base64::decodeWithoutPadding(details_header[0]->value().getStringView());
  if (decoded_value.empty()) {
    return absl::nullopt;
  }

  buf::rpc::Status status;
  if (!status.ParseFromString(decoded_value)) {
    return absl::nullopt;
  }

  return {std::move(status)};
}

Buffer::InstancePtr Common::serializeToConnectFrame(const Protobuf::Message& message) {
  // http://www.connect.io/docs/guides/wire.html
  // Reserve enough space for the entire message and the 5 byte header.
  // NB: we do not use prependConnectFrameHeader because that would add another BufferFragment and this
  // (using a single BufferFragment) is more efficient.
  Buffer::InstancePtr body(new Buffer::OwnedImpl());
  const uint32_t size = message.ByteSize();
  const uint32_t alloc_size = size + 5;
  auto reservation = body->reserveSingleSlice(alloc_size);
  ASSERT(reservation.slice().len_ >= alloc_size);
  uint8_t* current = reinterpret_cast<uint8_t*>(reservation.slice().mem_);
  *current++ = 0; // flags
  const uint32_t nsize = htonl(size);
  safeMemcpyUnsafeDst(current, &nsize);
  current += sizeof(uint32_t);
  Protobuf::io::ArrayOutputStream stream(current, size, -1);
  Protobuf::io::CodedOutputStream codec_stream(&stream);
  message.SerializeWithCachedSizes(&codec_stream);
  reservation.commit(alloc_size);
  return body;
}

Buffer::InstancePtr Common::serializeMessage(const Protobuf::Message& message) {
  auto body = std::make_unique<Buffer::OwnedImpl>();
  const uint32_t size = message.ByteSize();
  auto reservation = body->reserveSingleSlice(size);
  ASSERT(reservation.slice().len_ >= size);
  uint8_t* current = reinterpret_cast<uint8_t*>(reservation.slice().mem_);
  Protobuf::io::ArrayOutputStream stream(current, size, -1);
  Protobuf::io::CodedOutputStream codec_stream(&stream);
  message.SerializeWithCachedSizes(&codec_stream);
  reservation.commit(size);
  return body;
}

absl::optional<std::chrono::milliseconds>
Common::getConnectTimeout(const Http::RequestHeaderMap& request_headers) {
  const Http::HeaderEntry* header_connect_timeout_entry = request_headers.ConnectTimeout();
  std::chrono::milliseconds timeout;
  if (header_connect_timeout_entry) {
    int64_t connect_timeout;
    absl::string_view timeout_entry = header_connect_timeout_entry->value().getStringView();
    if (timeout_entry.empty()) {
      // Must be of the form TimeoutValue TimeoutUnit. See
      // https://github.com/connect/connect/blob/master/doc/PROTOCOL-HTTP2.md#requests.
      return absl::nullopt;
    }
    // TimeoutValue must be a positive integer of at most 8 digits.
    if (absl::SimpleAtoi(timeout_entry.substr(0, timeout_entry.size() - 1), &connect_timeout) &&
        connect_timeout >= 0 && static_cast<uint64_t>(connect_timeout) <= MAX_CONNECT_TIMEOUT_VALUE) {
      const char unit = timeout_entry[timeout_entry.size() - 1];
      switch (unit) {
      case 'H':
        return std::chrono::hours(connect_timeout);
      case 'M':
        return std::chrono::minutes(connect_timeout);
      case 'S':
        return std::chrono::seconds(connect_timeout);
      case 'm':
        return std::chrono::milliseconds(connect_timeout);
        break;
      case 'u':
        timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::microseconds(connect_timeout));
        if (timeout < std::chrono::microseconds(connect_timeout)) {
          timeout++;
        }
        return timeout;
      case 'n':
        timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(connect_timeout));
        if (timeout < std::chrono::nanoseconds(connect_timeout)) {
          timeout++;
        }
        return timeout;
      }
    }
  }
  return absl::nullopt;
}

void Common::toConnectTimeout(const std::chrono::milliseconds& timeout,
                           Http::RequestHeaderMap& headers) {
  uint64_t time = timeout.count();
  static const char units[] = "mSMH";
  const char* unit = units; // start with milliseconds
  if (time > MAX_CONNECT_TIMEOUT_VALUE) {
    time /= 1000; // Convert from milliseconds to seconds
    unit++;
  }
  while (time > MAX_CONNECT_TIMEOUT_VALUE) {
    if (*unit == 'H') {
      time = MAX_CONNECT_TIMEOUT_VALUE; // No bigger unit available, clip to max 8 digit hours.
    } else {
      time /= 60; // Convert from seconds to minutes to hours
      unit++;
    }
  }
  headers.setConnectTimeout(absl::StrCat(time, absl::string_view(unit, 1)));
}

Http::RequestMessagePtr
Common::prepareHeaders(const std::string& host_name, const std::string& service_full_name,
                       const std::string& method_name,
                       const absl::optional<std::chrono::milliseconds>& timeout) {
  Http::RequestMessagePtr message(new Http::RequestMessageImpl());
  message->headers().setReferenceMethod(Http::Headers::get().MethodValues.Post);
  message->headers().setPath(absl::StrCat("/", service_full_name, "/", method_name));
  message->headers().setHost(host_name);
  // According to https://github.com/connect/connect/blob/master/doc/PROTOCOL-HTTP2.md TE should appear
  // before Timeout and ContentType.
  message->headers().setReferenceTE(Http::Headers::get().TEValues.Trailers);
  if (timeout) {
    toConnectTimeout(timeout.value(), message->headers());
  }
  message->headers().setReferenceContentType(Http::Headers::get().ContentTypeValues.Connect);

  return message;
}

void Common::checkForHeaderOnlyError(Http::ResponseMessage& http_response) {
  // First check for connect-status in headers. If it is here, we have an error.
  absl::optional<Status::ConnectStatus> connect_status_code =
      Common::getConnectStatus(http_response.headers());
  if (!connect_status_code) {
    return;
  }

  if (connect_status_code.value() == Status::WellKnownConnectStatus::InvalidCode) {
    throw Exception(absl::optional<uint64_t>(), "bad connect-status header");
  }

  throw Exception(connect_status_code.value(), Common::getConnectMessage(http_response.headers()));
}

void Common::validateResponse(Http::ResponseMessage& http_response) {
  if (Http::Utility::getResponseStatus(http_response.headers()) != enumToInt(Http::Code::OK)) {
    throw Exception(absl::optional<uint64_t>(), "non-200 response code");
  }

  checkForHeaderOnlyError(http_response);

  // Check for existence of trailers.
  if (!http_response.trailers()) {
    throw Exception(absl::optional<uint64_t>(), "no response trailers");
  }

  absl::optional<Status::ConnectStatus> connect_status_code =
      Common::getConnectStatus(*http_response.trailers());
  if (!connect_status_code || connect_status_code.value() < 0) {
    throw Exception(absl::optional<uint64_t>(), "bad connect-status trailer");
  }

  if (connect_status_code.value() != 0) {
    throw Exception(connect_status_code.value(), Common::getConnectMessage(*http_response.trailers()));
  }
}

const std::string& Common::typeUrlPrefix() {
  CONSTRUCT_ON_FIRST_USE(std::string, "type.bufapis.com");
}

std::string Common::typeUrl(const std::string& qualified_name) {
  return typeUrlPrefix() + "/" + qualified_name;
}

void Common::prependConnectFrameHeader(Buffer::Instance& buffer) {
  std::array<char, 5> header;
  header[0] = 0; // flags
  const uint32_t nsize = htonl(buffer.length());
  safeMemcpyUnsafeDst(&header[1], &nsize);
  buffer.prepend(absl::string_view(&header[0], 5));
}

bool Common::parseBufferInstance(Buffer::InstancePtr&& buffer, Protobuf::Message& proto) {
  Buffer::ZeroCopyInputStreamImpl stream(std::move(buffer));
  return proto.ParseFromZeroCopyStream(&stream);
}

absl::optional<Common::RequestNames>
Common::resolveServiceAndMethod(const Http::HeaderEntry* path) {
  absl::optional<RequestNames> request_names;
  if (path == nullptr) {
    return request_names;
  }
  absl::string_view str = path->value().getStringView();
  str = str.substr(0, str.find('?'));
  const auto parts = StringUtil::splitToken(str, "/");
  if (parts.size() != 2) {
    return request_names;
  }
  request_names = RequestNames{parts[0], parts[1]};
  return request_names;
}

} // namespace Connect
} // namespace Envoy
