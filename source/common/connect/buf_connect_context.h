#pragma once

#include "source/common/common/thread.h"

namespace Envoy {
namespace Connect {

// Captures global connect initialization and shutdown. Note that connect
// initialization starts several threads, so it is a little annoying to run them
// alongside unrelated tests, particularly if they are trying to track memory
// usage, or you are exploiting otherwise consistent run-to-run pointer values
// during debug.
//
// Instantiating this class makes it easy to ensure classes that depend on connect
// libraries get them initialized.
class BufConnectContext {
public:
  BufConnectContext();
  ~BufConnectContext();

private:
  struct InstanceTracker {
    Thread::MutexBasicLockable mutex_;
    uint64_t live_instances_ ABSL_GUARDED_BY(mutex_) = 0;
  };

  static InstanceTracker& instanceTracker();

  InstanceTracker& instance_tracker_;
};

} // namespace Connect
} // namespace Envoy
