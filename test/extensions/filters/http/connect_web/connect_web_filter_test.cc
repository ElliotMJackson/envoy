#include "envoy/http/filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/base64.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/utility.h"
#include "source/common/connect/common.h"
#include "source/common/http/codes.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/headers.h"
#include "source/common/stats/symbol_table.h"
#include "source/extensions/filters/http/connect_web/connect_web_filter.h"

#include "test/mocks/http/mocks.h"
#include "test/test_common/global.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Combine;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::Values;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ConnectWeb {
namespace {

const char MESSAGE[] = "\x00\x00\x00\x00\x11connect-web-bin-data";
const size_t MESSAGE_SIZE = sizeof(MESSAGE) - 1;
const char TEXT_MESSAGE[] = "\x00\x00\x00\x00\x12connect-web-text-data";
const size_t TEXT_MESSAGE_SIZE = sizeof(TEXT_MESSAGE) - 1;
const char B64_MESSAGE[] = "AAAAABJncnBjLXdlYi10ZXh0LWRhdGE=";
const size_t B64_MESSAGE_SIZE = sizeof(B64_MESSAGE) - 1;
const char B64_MESSAGE_NO_PADDING[] = "AAAAABJncnBjLXdlYi10ZXh0LWRhdGE";
const size_t B64_MESSAGE_NO_PADDING_SIZE = sizeof(B64_MESSAGE_NO_PADDING) - 1;
const char INVALID_B64_MESSAGE[] = "****";
const size_t INVALID_B64_MESSAGE_SIZE = sizeof(INVALID_B64_MESSAGE) - 1;
const char TRAILERS[] = "\x80\x00\x00\x00\x20connect-status:0\r\nconnect-message:ok\r\n";
const size_t TRAILERS_SIZE = sizeof(TRAILERS) - 1;
constexpr uint64_t MAX_BUFFERED_PLAINTEXT_LENGTH = 16384;

} // namespace

class ConnectWebFilterTest : public testing::TestWithParam<std::tuple<std::string, std::string>> {
public:
  ConnectWebFilterTest() : connect_context_(*symbol_table_), filter_(connect_context_) {
    filter_.setDecoderFilterCallbacks(decoder_callbacks_);
    filter_.setEncoderFilterCallbacks(encoder_callbacks_);
  }

  ~ConnectWebFilterTest() override { filter_.onDestroy(); }

  const std::string& request_content_type() const { return std::get<0>(GetParam()); }

  const std::string& request_accept() const { return std::get<1>(GetParam()); }

  bool isTextRequest() const {
    return request_content_type() == Http::Headers::get().ContentTypeValues.ConnectWebText ||
           request_content_type() == Http::Headers::get().ContentTypeValues.ConnectWebTextProto;
  }

  bool isBinaryRequest() const {
    return request_content_type() == Http::Headers::get().ContentTypeValues.ConnectWeb ||
           request_content_type() == Http::Headers::get().ContentTypeValues.ConnectWebProto;
  }

  bool accept_text_response() const {
    return request_accept() == Http::Headers::get().ContentTypeValues.ConnectWebText ||
           request_accept() == Http::Headers::get().ContentTypeValues.ConnectWebTextProto;
  }

  bool accept_binary_response() const {
    return request_accept() == Http::Headers::get().ContentTypeValues.ConnectWeb ||
           request_accept() == Http::Headers::get().ContentTypeValues.ConnectWebProto;
  }

  bool doStatTracking() const { return filter_.doStatTracking(); }

  void expectErrorResponse(const Http::Code& expected_code, const std::string& expected_message) {
    EXPECT_CALL(decoder_callbacks_, encodeHeaders_(_, _))
        .WillOnce(Invoke([=](Http::ResponseHeaderMap& headers, bool) {
          uint64_t code;
          ASSERT_TRUE(absl::SimpleAtoi(headers.getStatusValue(), &code));
          EXPECT_EQ(static_cast<uint64_t>(expected_code), code);
        }));
    EXPECT_CALL(decoder_callbacks_, encodeData(_, _))
        .WillOnce(Invoke(
            [=](Buffer::Instance& data, bool) { EXPECT_EQ(expected_message, data.toString()); }));
  }

  void expectRequiredConnectUpstreamHeaders(const Http::TestRequestHeaderMapImpl& request_headers) {
    EXPECT_EQ(Http::Headers::get().ContentTypeValues.Connect, request_headers.getContentTypeValue());
    // Ensure we never send content-length upstream
    EXPECT_EQ(nullptr, request_headers.ContentLength());
    EXPECT_EQ(Http::Headers::get().TEValues.Trailers, request_headers.getTEValue());
    EXPECT_EQ(Http::CustomHeaders::get().ConnectAcceptEncodingValues.Default,
              request_headers.get_(Http::CustomHeaders::get().ConnectAcceptEncoding));
  }

  bool isProtoEncodedConnectWebContentType(const std::string& content_type) {
    Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
    if (!content_type.empty()) {
      request_headers.addCopy(Http::Headers::get().ContentType, content_type);
    }
    return filter_.hasProtoEncodedConnectWebContentType(request_headers);
  }

  bool isProtoEncodedConnectWebResponseHeaders(const Http::ResponseHeaderMap& headers) {
    return filter_.isProtoEncodedConnectWebResponseHeaders(headers);
  }

  void expectMergedAndLimitedResponseData(Buffer::Instance* encoded_buffer,
                                          Buffer::Instance* last_data,
                                          uint64_t expected_merged_length) {
    if (encoded_buffer != nullptr) {
      auto on_modify_encoding_buffer = [encoded_buffer](std::function<void(Buffer::Instance&)> cb) {
        cb(*encoded_buffer);
      };
      if (last_data != nullptr) {
        EXPECT_CALL(encoder_callbacks_, addEncodedData(_, false))
            .WillOnce(Invoke([&](Buffer::Instance& data, bool) { encoded_buffer->move(data); }));
      }
      EXPECT_CALL(encoder_callbacks_, encodingBuffer).WillRepeatedly(Return(encoded_buffer));
      EXPECT_CALL(encoder_callbacks_, modifyEncodingBuffer)
          .WillRepeatedly(Invoke(on_modify_encoding_buffer));
    }
    Buffer::OwnedImpl output;
    filter_.mergeAndLimitNonProtoEncodedResponseData(output, last_data);
    EXPECT_EQ(expected_merged_length, output.length());
    if (encoded_buffer != nullptr) {
      EXPECT_EQ(0U, encoded_buffer->length());
    }
    if (last_data != nullptr) {
      EXPECT_EQ(0U, last_data->length());
    }
  }

  Stats::TestUtil::TestSymbolTable symbol_table_;
  Connect::ContextImpl connect_context_;
  ConnectWebFilter filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  Http::TestRequestTrailerMapImpl request_trailers_;
  Http::TestResponseHeaderMapImpl response_headers_;
  Http::TestResponseTrailerMapImpl response_trailers_;
  Http::TestRequestHeaderMapImpl request_headers_{{":path", "/"}};
};

TEST_F(ConnectWebFilterTest, SupportedContentTypes) {
  const std::string supported_content_types[] = {
      Http::Headers::get().ContentTypeValues.ConnectWeb,
      Http::Headers::get().ContentTypeValues.ConnectWebProto,
      Http::Headers::get().ContentTypeValues.ConnectWebText,
      Http::Headers::get().ContentTypeValues.ConnectWebTextProto};
  for (auto& content_type : supported_content_types) {
    Http::TestRequestHeaderMapImpl request_headers{{":path", "/"}};
    request_headers.addCopy(Http::Headers::get().ContentType, content_type);
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
    Http::MetadataMap metadata_map{{"metadata", "metadata"}};
    EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_.decodeMetadata(metadata_map));
    EXPECT_EQ(Http::Headers::get().ContentTypeValues.Connect, request_headers.getContentTypeValue());
  }
}

TEST_F(ConnectWebFilterTest, ExpectedConnectWebProtoContentType) {
  EXPECT_TRUE(isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWeb));
  EXPECT_TRUE(
      isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWebProto));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWeb +
                                               "; version=1; action=urn:CreateCredential"));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWeb +
                                               "    ; version=1; action=urn:CreateCredential"));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWebProto +
                                               "; version=1"));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType("Application/Connect-Web"));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType("Application/Connect-Web+Proto"));
  EXPECT_TRUE(isProtoEncodedConnectWebContentType("APPLICATION/CONNECT-WEB+PROTO; ok=1; great=1"));
}

TEST_F(ConnectWebFilterTest, UnexpectedConnectWebProtoContentType) {
  EXPECT_FALSE(isProtoEncodedConnectWebContentType(EMPTY_STRING));
  EXPECT_FALSE(isProtoEncodedConnectWebContentType("Invalid; ok=1"));
  EXPECT_FALSE(isProtoEncodedConnectWebContentType("Invalid; ok=1; nok=2"));
  EXPECT_FALSE(
      isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWeb + "+thrift"));
  EXPECT_FALSE(
      isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWeb + "+json"));
  EXPECT_FALSE(
      isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWebText));
  EXPECT_FALSE(
      isProtoEncodedConnectWebContentType(Http::Headers::get().ContentTypeValues.ConnectWebTextProto));
}

TEST_F(ConnectWebFilterTest, ExpectedConnectWebProtoResponseHeaders) {
  EXPECT_TRUE(isProtoEncodedConnectWebResponseHeaders(Http::TestResponseHeaderMapImpl{
      {":status", "200"}, {"content-type", "application/connect-web"}}));
  EXPECT_TRUE(isProtoEncodedConnectWebResponseHeaders(Http::TestResponseHeaderMapImpl{
      {":status", "200"}, {"content-type", "application/connect-web+proto"}}));
}

TEST_F(ConnectWebFilterTest, UnexpectedConnectWebProtoResponseHeaders) {
  EXPECT_FALSE(isProtoEncodedConnectWebResponseHeaders(Http::TestResponseHeaderMapImpl{
      {":status", "500"}, {"content-type", "application/connect-web+proto"}}));
  EXPECT_FALSE(isProtoEncodedConnectWebResponseHeaders(Http::TestResponseHeaderMapImpl{
      {":status", "200"}, {"content-type", "application/connect-web+json"}}));
}

TEST_F(ConnectWebFilterTest, MergeAndLimitNonProtoEncodedResponseData) {
  Buffer::OwnedImpl encoded_buffer(std::string(100, 'a'));
  Buffer::OwnedImpl last_data(std::string(100, 'a'));
  expectMergedAndLimitedResponseData(&encoded_buffer, &last_data,
                                     /*expected_merged_length=*/encoded_buffer.length() +
                                         last_data.length());
}

TEST_F(ConnectWebFilterTest, MergeAndLimitNonProtoEncodedResponseDataWithLargeEncodingBuffer) {
  Buffer::OwnedImpl encoded_buffer(std::string(2 * MAX_BUFFERED_PLAINTEXT_LENGTH, 'a'));
  Buffer::OwnedImpl last_data(std::string(2 * MAX_BUFFERED_PLAINTEXT_LENGTH, 'a'));
  // Since the buffered data in encoding buffer is larger than MAX_BUFFERED_PLAINTEXT_LENGTH, the
  // output length is limited to MAX_BUFFERED_PLAINTEXT_LENGTH.
  expectMergedAndLimitedResponseData(&encoded_buffer, &last_data,
                                     /*expected_merged_length=*/MAX_BUFFERED_PLAINTEXT_LENGTH);
}

TEST_F(ConnectWebFilterTest, MergeAndLimitNonProtoEncodedResponseDataWithNullEncodingBuffer) {
  Buffer::OwnedImpl last_data(std::string(2 * MAX_BUFFERED_PLAINTEXT_LENGTH, 'a'));
  // If we don't have buffered data in encoding buffer, the merged data will be the same as last
  // data.
  expectMergedAndLimitedResponseData(nullptr, &last_data,
                                     /*expected_merged_length=*/last_data.length());
}

TEST_F(ConnectWebFilterTest, MergeAndLimitNonProtoEncodedResponseDataWithNoEncodingBufferAndLastData) {
  // If we don't have both buffered data in encoding buffer and last data, the output length is
  // zero.
  expectMergedAndLimitedResponseData(nullptr, nullptr, /*expected_merged_length=*/0U);
}

TEST_F(ConnectWebFilterTest, UnsupportedContentType) {
  Buffer::OwnedImpl data;
  request_headers_.addCopy(Http::Headers::get().ContentType, "unsupported");
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(request_trailers_));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers_));
}

TEST_F(ConnectWebFilterTest, NoContentType) {
  Buffer::OwnedImpl data;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(request_trailers_));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers_));
}

TEST_F(ConnectWebFilterTest, NoPath) {
  Http::TestRequestHeaderMapImpl request_headers{};
  Buffer::OwnedImpl data;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(request_trailers_));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers_));
}

TEST_F(ConnectWebFilterTest, InvalidBase64) {
  request_headers_.addCopy(Http::Headers::get().ContentType,
                           Http::Headers::get().ContentTypeValues.ConnectWebText);
  expectErrorResponse(Http::Code::BadRequest, "Bad Connect-web request, invalid base64 data.");
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  expectRequiredConnectUpstreamHeaders(request_headers_);

  Buffer::OwnedImpl request_buffer;
  Buffer::OwnedImpl decoded_buffer;
  request_buffer.add(&INVALID_B64_MESSAGE, INVALID_B64_MESSAGE_SIZE);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
            filter_.decodeData(request_buffer, true));
  EXPECT_EQ(decoder_callbacks_.details(), "connect_base_64_decode_failed");
}

TEST_F(ConnectWebFilterTest, Base64NoPadding) {
  request_headers_.addCopy(Http::Headers::get().ContentType,
                           Http::Headers::get().ContentTypeValues.ConnectWebText);
  expectErrorResponse(Http::Code::BadRequest, "Bad Connect-web request, invalid base64 data.");
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  expectRequiredConnectUpstreamHeaders(request_headers_);

  Buffer::OwnedImpl request_buffer;
  Buffer::OwnedImpl decoded_buffer;
  request_buffer.add(&B64_MESSAGE_NO_PADDING, B64_MESSAGE_NO_PADDING_SIZE);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
            filter_.decodeData(request_buffer, true));
  EXPECT_EQ(decoder_callbacks_.details(), "connect_base_64_decode_failed_bad_size");
}

TEST_F(ConnectWebFilterTest, InvalidUpstreamResponseForText) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebText}, {":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data("hello");
  Buffer::InstancePtr buffer(new Buffer::OwnedImpl("hello"));
  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).WillRepeatedly(Return(buffer.get()));
  auto on_modify_encoding_buffer = [encoded_buffer =
                                        buffer.get()](std::function<void(Buffer::Instance&)> cb) {
    cb(*encoded_buffer);
  };
  EXPECT_CALL(encoder_callbacks_, modifyEncodingBuffer)
      .WillRepeatedly(Invoke(on_modify_encoding_buffer));

  EXPECT_CALL(encoder_callbacks_, addEncodedData(_, false))
      .WillOnce(Invoke([&](Buffer::Instance& data, bool) { buffer->move(data); }));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_.encodeData(data, false));

  buffer->add(data);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_.encodeData(data, false));

  TestUtility::feedBufferWithRandomCharacters(data, MAX_BUFFERED_PLAINTEXT_LENGTH);
  const std::string expected_connect_message =
      absl::StrCat("hellohello", data.toString()).substr(0, MAX_BUFFERED_PLAINTEXT_LENGTH);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, true));
  EXPECT_EQ(MAX_BUFFERED_PLAINTEXT_LENGTH,
            response_headers.get_(Http::Headers::get().ConnectMessage).length());
  EXPECT_EQ(expected_connect_message, response_headers.get_(Http::Headers::get().ConnectMessage));
}

TEST_F(ConnectWebFilterTest, InvalidUpstreamResponseForTextWithLargeEncodingBuffer) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebText}, {":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl encoded_buffer;
  encoded_buffer.add(std::string(2 * MAX_BUFFERED_PLAINTEXT_LENGTH, 'a'));
  // The encoding buffer is filled with data more than MAX_BUFFERED_PLAINTEXT_LENGTH.
  auto on_modify_encoding_buffer = [&encoded_buffer](std::function<void(Buffer::Instance&)> cb) {
    cb(encoded_buffer);
  };
  EXPECT_CALL(encoder_callbacks_, encodingBuffer).WillRepeatedly(Return(&encoded_buffer));
  EXPECT_CALL(encoder_callbacks_, modifyEncodingBuffer)
      .WillRepeatedly(Invoke(on_modify_encoding_buffer));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
            filter_.encodeData(encoded_buffer, false));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
            filter_.encodeData(encoded_buffer, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(encoded_buffer, true));
  EXPECT_EQ(MAX_BUFFERED_PLAINTEXT_LENGTH,
            response_headers.get_(Http::Headers::get().ConnectMessage).length());
}

TEST_F(ConnectWebFilterTest, InvalidUpstreamResponseForTextWithLargeLastData) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebText}, {":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data;
  // The last data length is set to be bigger than "MAX_BUFFERED_PLAINTEXT_LENGTH".
  const std::string expected_connect_message = std::string(MAX_BUFFERED_PLAINTEXT_LENGTH + 1, 'a');
  data.add(expected_connect_message);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, true));
  // The connect-message value length is the same as the last sent buffer.
  EXPECT_EQ(MAX_BUFFERED_PLAINTEXT_LENGTH + 1,
            response_headers.get_(Http::Headers::get().ConnectMessage).length());
  EXPECT_EQ(expected_connect_message, response_headers.get_(Http::Headers::get().ConnectMessage));
}

TEST_F(ConnectWebFilterTest, InvalidUpstreamResponseForTextWithTrailers) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebText}, {":path", "/"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "400"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data("hello");
  Buffer::InstancePtr buffer(new Buffer::OwnedImpl("hello"));
  EXPECT_CALL(encoder_callbacks_, encodingBuffer()).WillRepeatedly(Return(buffer.get()));
  auto on_modify_encoding_buffer = [encoded_buffer =
                                        buffer.get()](std::function<void(Buffer::Instance&)> cb) {
    cb(*encoded_buffer);
  };
  EXPECT_CALL(encoder_callbacks_, modifyEncodingBuffer)
      .WillRepeatedly(Invoke(on_modify_encoding_buffer));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_.encodeData(data, false));

  buffer->add(data);
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, filter_.encodeData(data, false));

  Http::TestResponseTrailerMapImpl response_trailers{{"connect-status", "0"}};
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));

  EXPECT_EQ("hellohello", response_headers.get_(Http::Headers::get().ConnectMessage));
}

TEST_P(ConnectWebFilterTest, StatsNoCluster) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", request_content_type()},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};
  EXPECT_CALL(decoder_callbacks_, clusterInfo()).WillOnce(Return(nullptr));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  EXPECT_FALSE(doStatTracking());
}

TEST_P(ConnectWebFilterTest, StatsNormalResponse) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", request_content_type()},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));

  Http::TestResponseHeaderMapImpl continue_headers{{":status", "100"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encode1xxHeaders(continue_headers));

  Http::MetadataMap metadata_map{{"metadata", "metadata"}};
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_.encodeMetadata(metadata_map));

  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "200"}, {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebProto}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
  Http::TestResponseTrailerMapImpl response_trailers{{"connect-status", "0"}};
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));
  EXPECT_EQ(1UL,
            decoder_callbacks_.clusterInfo()
                ->statsScope()
                .counterFromString("connect-web.lyft.users.BadCompanions.GetBadCompanions.success")
                .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counterFromString("connect-web.lyft.users.BadCompanions.GetBadCompanions.total")
                     .value());
}

TEST_P(ConnectWebFilterTest, StatsErrorResponse) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", request_content_type()},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "200"}, {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebProto}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
  Http::TestResponseTrailerMapImpl response_trailers{{"connect-status", "1"}};
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));
  EXPECT_EQ(1UL,
            decoder_callbacks_.clusterInfo()
                ->statsScope()
                .counterFromString("connect-web.lyft.users.BadCompanions.GetBadCompanions.failure")
                .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counterFromString("connect-web.lyft.users.BadCompanions.GetBadCompanions.total")
                     .value());
}

TEST_P(ConnectWebFilterTest, ExternallyProvidedEncodingHeader) {
  Http::TestRequestHeaderMapImpl request_headers{
      {"connect-accept-encoding", "foo"}, {":path", "/"}, {"content-type", request_accept()}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  EXPECT_EQ("foo", request_headers.get_(Http::CustomHeaders::get().ConnectAcceptEncoding));
}

TEST_P(ConnectWebFilterTest, MediaTypeWithParameter) {
  Http::TestRequestHeaderMapImpl request_headers{{"content-type", request_content_type()},
                                                 {":path", "/test.MediaTypes/GetParameter"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers, false));
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "200"},
      // Set a valid media-type with a specified parameter value.
      {"content-type", Http::Headers::get().ContentTypeValues.ConnectWebProto + "; version=1"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers, false));
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(data, false));
}

TEST_P(ConnectWebFilterTest, Unary) {
  // Tests request headers.
  request_headers_.addCopy(Http::Headers::get().ContentType, request_content_type());
  request_headers_.addCopy(Http::CustomHeaders::get().Accept, request_accept());
  request_headers_.addCopy(Http::Headers::get().ContentLength, uint64_t(8));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.decodeHeaders(request_headers_, false));
  expectRequiredConnectUpstreamHeaders(request_headers_);

  // Tests request data.
  if (isBinaryRequest()) {
    Buffer::OwnedImpl request_buffer;
    Buffer::OwnedImpl decoded_buffer;
    for (size_t i = 0; i < MESSAGE_SIZE; i++) {
      request_buffer.add(&MESSAGE[i], 1);
      EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(request_buffer, true));
      decoded_buffer.move(request_buffer);
    }
    EXPECT_EQ(std::string(MESSAGE, MESSAGE_SIZE), decoded_buffer.toString());
  } else if (isTextRequest()) {
    Buffer::OwnedImpl request_buffer;
    Buffer::OwnedImpl decoded_buffer;
    for (size_t i = 0; i < B64_MESSAGE_SIZE; i++) {
      request_buffer.add(&B64_MESSAGE[i], 1);
      if (i == B64_MESSAGE_SIZE - 1) {
        EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(request_buffer, true));
        decoded_buffer.move(request_buffer);
        break;
      }
      if (i % 4 == 3) {
        EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.decodeData(request_buffer, false));
      } else {
        EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
                  filter_.decodeData(request_buffer, false));
      }
      decoded_buffer.move(request_buffer);
    }
    EXPECT_EQ(std::string(TEXT_MESSAGE, TEXT_MESSAGE_SIZE), decoded_buffer.toString());
  } else {
    FAIL() << "Unsupported Connect-Web request content-type: " << request_content_type();
  }

  // Tests request trailers, they are passed through.
  Http::TestRequestTrailerMapImpl request_trailers;
  request_trailers.addCopy(Http::Headers::get().ConnectStatus, "0");
  request_trailers.addCopy(Http::Headers::get().ConnectMessage, "ok");
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.decodeTrailers(request_trailers_));
  EXPECT_EQ("0", request_trailers.get_("connect-status"));
  EXPECT_EQ("ok", request_trailers.get_("connect-message"));

  // Tests response headers.
  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.addCopy(Http::Headers::get().Status, "200");
  response_headers.addCopy(Http::Headers::get().ContentType,
                           Http::Headers::get().ContentTypeValues.ConnectWebProto);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers, false));
  EXPECT_EQ("200", response_headers.get_(Http::Headers::get().Status.get()));
  if (accept_binary_response()) {
    EXPECT_EQ(Http::Headers::get().ContentTypeValues.ConnectWebProto,
              response_headers.getContentTypeValue());
  } else if (accept_text_response()) {
    EXPECT_EQ(Http::Headers::get().ContentTypeValues.ConnectWebTextProto,
              response_headers.getContentTypeValue());
  } else {
    FAIL() << "Unsupported Connect-Web request accept: " << request_accept();
  }

  // Tests response data.
  if (accept_binary_response()) {
    Buffer::OwnedImpl response_buffer;
    Buffer::OwnedImpl encoded_buffer;
    for (size_t i = 0; i < MESSAGE_SIZE; i++) {
      response_buffer.add(&MESSAGE[i], 1);
      EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(response_buffer, false));
      encoded_buffer.move(response_buffer);
    }
    EXPECT_EQ(std::string(MESSAGE, MESSAGE_SIZE), encoded_buffer.toString());
  } else if (accept_text_response()) {
    Buffer::OwnedImpl response_buffer;
    Buffer::OwnedImpl encoded_buffer;
    for (size_t i = 0; i < TEXT_MESSAGE_SIZE; i++) {
      response_buffer.add(&TEXT_MESSAGE[i], 1);
      if (i < TEXT_MESSAGE_SIZE - 1) {
        EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer,
                  filter_.encodeData(response_buffer, false));
      } else {
        EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(response_buffer, false));
      }
      encoded_buffer.move(response_buffer);
    }
    EXPECT_EQ(std::string(B64_MESSAGE, B64_MESSAGE_SIZE), encoded_buffer.toString());
  } else {
    FAIL() << "Unsupported Connect-Web response content-type: "
           << response_headers.getContentTypeValue();
  }

  // Tests response trailers.
  Buffer::OwnedImpl trailers_buffer;
  EXPECT_CALL(encoder_callbacks_, addEncodedData(_, true))
      .WillOnce(Invoke([&](Buffer::Instance& data, bool) { trailers_buffer.move(data); }));
  Http::TestResponseTrailerMapImpl response_trailers;
  response_trailers.addCopy(Http::Headers::get().ConnectStatus, "0");
  response_trailers.addCopy(Http::Headers::get().ConnectMessage, "ok");
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));
  if (accept_binary_response()) {
    EXPECT_EQ(std::string(TRAILERS, TRAILERS_SIZE), trailers_buffer.toString());
  } else if (accept_text_response()) {
    EXPECT_EQ(std::string(TRAILERS, TRAILERS_SIZE), Base64::decode(trailers_buffer.toString()));
  } else {
    FAIL() << "Unsupported Connect-Web response content-type: "
           << response_headers.getContentTypeValue();
  }
  EXPECT_EQ(0, response_trailers.size());
}

INSTANTIATE_TEST_SUITE_P(Unary, ConnectWebFilterTest,
                         Combine(Values(Http::Headers::get().ContentTypeValues.ConnectWeb,
                                        Http::Headers::get().ContentTypeValues.ConnectWebProto,
                                        Http::Headers::get().ContentTypeValues.ConnectWebText,
                                        Http::Headers::get().ContentTypeValues.ConnectWebTextProto),
                                 Values(Http::Headers::get().ContentTypeValues.ConnectWeb,
                                        Http::Headers::get().ContentTypeValues.ConnectWebProto,
                                        Http::Headers::get().ContentTypeValues.ConnectWebText,
                                        Http::Headers::get().ContentTypeValues.ConnectWebTextProto)));

} // namespace ConnectWeb
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
