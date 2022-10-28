#pragma once

#include <cstdint>
#include <string>

#include "envoy/api/api.h"
#include "envoy/buffer/buffer.h"
#include "envoy/common/platform.h"
#include "envoy/config/core/v3/connect_service.pb.h"

#include "connectpp/connectpp.h"

namespace Envoy {
namespace Connect {

class BufConnectUtils {
public:
  /**
   * Build connect::ByteBuffer which aliases the data in a Buffer::InstancePtr.
   * @param buffer source data container.
   * @return byteBuffer target container aliased to the data in Buffer::Instance and owning the
   * Buffer::Instance.
   */
  static connect::ByteBuffer makeByteBuffer(Buffer::InstancePtr&& buffer);

  /**
   * Build Buffer::Instance which aliases the data in a connect::ByteBuffer.
   * @param buffer source data container.
   * @return a Buffer::InstancePtr aliased to the data in the provided connect::ByteBuffer and
   * owning the corresponding connect::Slice(s) or nullptr if the connect::ByteBuffer is bad.
   */
  static Buffer::InstancePtr makeBufferInstance(const connect::ByteBuffer& buffer);

  /**
   * Build connect::ChannelArguments from Connect service config.
   * @param config Buf Connect config.
   * @return connect::ChannelArguments corresponding to config.
   */
  static connect::ChannelArguments
  channelArgsFromConfig(const envoy::config::core::v3::ConnectService& config);

  /**
   * Build Connect channel based on the given ConnectService configuration.
   * @param config Buf Connect config.
   * @param api reference to the Api object
   * @return static std::shared_ptr<connect::Channel> a Connect channel.
   */
  static std::shared_ptr<connect::Channel>
  createChannel(const envoy::config::core::v3::ConnectService& config, Api::Api& api);
};

} // namespace Connect
} // namespace Envoy
