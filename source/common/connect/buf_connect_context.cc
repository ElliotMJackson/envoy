#include "source/common/connect/buf_connect_context.h"

#include <atomic>

#include "source/common/common/assert.h"
#include "source/common/common/lock_guard.h"
#include "source/common/common/macros.h"
#include "source/common/common/thread.h"

#ifdef ENVOY_BUF_CONNECT
#include "connectpp/connectpp.h"
#endif

namespace Envoy {
namespace Connect {

BufConnectContext::BufConnectContext() : instance_tracker_(instanceTracker()) {
#ifdef ENVOY_BUF_CONNECT
  Thread::LockGuard lock(instance_tracker_.mutex_);
  if (++instance_tracker_.live_instances_ == 1) {
    connect_init();
  }
#endif
}

BufConnectContext::~BufConnectContext() {
#ifdef ENVOY_BUF_CONNECT
  // Per https://github.com/connect/connect/issues/20303 it is OK to call
  // connect_shutdown_blocking() as long as no one can concurrently call
  // connect_init(). We use check_format.py to ensure that this file contains the
  // only callers to connect_init(), and the mutex to then make that guarantee
  // across users of this class.
  Thread::LockGuard lock(instance_tracker_.mutex_);
  ASSERT(instance_tracker_.live_instances_ > 0);
  if (--instance_tracker_.live_instances_ == 0) {
    connect_shutdown_blocking(); // Waiting for quiescence avoids non-determinism in tests.
  }
#endif
}

BufConnectContext::InstanceTracker& BufConnectContext::instanceTracker() {
  MUTABLE_CONSTRUCT_ON_FIRST_USE(InstanceTracker);
}

} // namespace Connect
} // namespace Envoy
