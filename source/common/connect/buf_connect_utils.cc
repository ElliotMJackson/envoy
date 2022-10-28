#include "source/common/connect/buf_connect_utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "envoy/connect/buf_connect_creds.h"
#include "envoy/registry/registry.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/fmt.h"
#include "source/common/common/macros.h"
#include "source/common/common/utility.h"

#include "absl/container/fixed_array.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Connect {

namespace {

std::shared_ptr<connect::ChannelCredentials>
getBufConnectChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service,
                                Api::Api& api) {
  BufConnectCredentialsFactory* credentials_factory = nullptr;
  const std::string& buf_connect_credentials_factory_name =
      connect_service.buf_connect().credentials_factory_name();
  if (buf_connect_credentials_factory_name.empty()) {
    credentials_factory = Registry::FactoryRegistry<BufConnectCredentialsFactory>::getFactory(
        "envoy.connect_credentials.default");
  } else {
    credentials_factory = Registry::FactoryRegistry<BufConnectCredentialsFactory>::getFactory(
        buf_connect_credentials_factory_name);
  }
  if (credentials_factory == nullptr) {
    throw EnvoyException(absl::StrCat("Unknown buf connect credentials factory: ",
                                      buf_connect_credentials_factory_name));
  }
  return credentials_factory->getChannelCredentials(connect_service, api);
}

} // namespace

struct BufferInstanceContainer {
  BufferInstanceContainer(int ref_count, Buffer::InstancePtr&& buffer)
      : ref_count_(ref_count), buffer_(std::move(buffer)) {}
  std::atomic<uint32_t> ref_count_; // In case gPRC dereferences in a different threads.
  Buffer::InstancePtr buffer_;

  static void derefBufferInstanceContainer(void* container_ptr) {
    auto container = static_cast<BufferInstanceContainer*>(container_ptr);
    container->ref_count_--;
    // This is safe because the ref_count_ is never incremented.
    if (container->ref_count_ <= 0) {
      delete container;
    }
  }
};

connect::ByteBuffer BufConnectUtils::makeByteBuffer(Buffer::InstancePtr&& buffer_instance) {
  if (!buffer_instance) {
    return {};
  }
  Buffer::RawSliceVector raw_slices = buffer_instance->getRawSlices();
  if (raw_slices.empty()) {
    return {};
  }

  auto* container =
      new BufferInstanceContainer{static_cast<int>(raw_slices.size()), std::move(buffer_instance)};
  std::vector<connect::Slice> slices;
  slices.reserve(raw_slices.size());
  for (Buffer::RawSlice& raw_slice : raw_slices) {
    slices.emplace_back(raw_slice.mem_, raw_slice.len_,
                        &BufferInstanceContainer::derefBufferInstanceContainer, container);
  }
  return {&slices[0], slices.size()};
}

class ConnectSliceBufferFragmentImpl : public Buffer::BufferFragment {
public:
  explicit ConnectSliceBufferFragmentImpl(connect::Slice&& slice) : slice_(std::move(slice)) {}

  // Buffer::BufferFragment
  const void* data() const override { return slice_.begin(); }
  size_t size() const override { return slice_.size(); }
  void done() override { delete this; }

private:
  const connect::Slice slice_;
};

Buffer::InstancePtr BufConnectUtils::makeBufferInstance(const connect::ByteBuffer& byte_buffer) {
  auto buffer = std::make_unique<Buffer::OwnedImpl>();
  if (byte_buffer.Length() == 0) {
    return buffer;
  }
  // NB: ByteBuffer::Dump moves the data out of the ByteBuffer so we need to ensure that the
  // lifetime of the Slice(s) exceeds our Buffer::Instance.
  std::vector<connect::Slice> slices;
  if (!byte_buffer.Dump(&slices).ok()) {
    return nullptr;
  }

  for (auto& slice : slices) {
    buffer->addBufferFragment(*new ConnectSliceBufferFragmentImpl(std::move(slice)));
  }
  return buffer;
}

connect::ChannelArguments
BufConnectUtils::channelArgsFromConfig(const envoy::config::core::v3::ConnectService& config) {
  connect::ChannelArguments args;
  for (const auto& channel_arg : config.buf_connect().channel_args().args()) {
    switch (channel_arg.second.value_specifier_case()) {
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelArgs::Value::kStringValue:
      args.SetString(channel_arg.first, channel_arg.second.string_value());
      break;
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelArgs::Value::kIntValue:
      args.SetInt(channel_arg.first, channel_arg.second.int_value());
      break;
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelArgs::Value::
        VALUE_SPECIFIER_NOT_SET:
      PANIC_DUE_TO_PROTO_UNSET;
    }
  }
  return args;
}

std::shared_ptr<connect::Channel>
BufConnectUtils::createChannel(const envoy::config::core::v3::ConnectService& config, Api::Api& api) {
  std::shared_ptr<connect::ChannelCredentials> creds = getBufConnectChannelCredentials(config, api);
  const connect::ChannelArguments args = channelArgsFromConfig(config);
  return CreateCustomChannel(config.buf_connect().target_uri(), creds, args);
}

} // namespace Connect
} // namespace Envoy
