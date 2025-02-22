// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <stdlib.h>
#include <string>

#include "yb/cdc/cdc_util.h"
#include "yb/tserver/xcluster_output_client_interface.h"
#include "yb/common/hybrid_time.h"
#include "yb/rpc/rpc.h"
#include "yb/tserver/xcluster_consumer.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/locks.h"
#include "yb/util/status_fwd.h"

#pragma once

namespace yb {

class ThreadPool;

namespace rpc {

class RpcController;

} // namespace rpc

namespace cdc {

class CDCServiceProxy;

} // namespace cdc

namespace tserver {

class XClusterConsumer;

class XClusterPoller : public std::enable_shared_from_this<XClusterPoller> {
 public:
  XClusterPoller(
      const cdc::ProducerTabletInfo& producer_tablet_info,
      const cdc::ConsumerTabletInfo& consumer_tablet_info,
      ThreadPool* thread_pool,
      rpc::Rpcs* rpcs,
      const std::shared_ptr<XClusterClient>& local_client,
      const std::shared_ptr<XClusterClient>& producer_client,
      XClusterConsumer* xcluster_consumer,
      bool use_local_tserver,
      const std::vector<TabletId>& global_transaction_status_tablets,
      bool enable_replicate_transaction_status_table,
      SchemaVersion last_compatible_consumer_schema_version);
  ~XClusterPoller();

  void Shutdown();

  // Begins poll process for a producer tablet.
  void Poll();

  bool IsPolling() { return is_polling_; }

  void SetSchemaVersion(SchemaVersion cur_version,
                        SchemaVersion last_compatible_consumer_schema_version);

  std::string LogPrefixUnlocked() const;

  HybridTime GetSafeTime() const EXCLUDES(safe_time_lock_);

  cdc::ConsumerTabletInfo GetConsumerTabletInfo() const;

 private:
  bool CheckOffline();

  void DoSetSchemaVersion(SchemaVersion cur_version,
                          SchemaVersion current_consumer_schema_version);

  void DoPoll();
  // Does the work of sending the changes to the output client.
  void HandlePoll(const Status& status, cdc::GetChangesResponsePB&& resp);
  void DoHandlePoll(Status status, std::shared_ptr<cdc::GetChangesResponsePB> resp);
  // Async handler for the response from output client.
  void HandleApplyChanges(XClusterOutputClientResponse response);
  // Does the work of polling for new changes.
  void DoHandleApplyChanges(XClusterOutputClientResponse response);
  void UpdateSafeTime(int64 new_time) EXCLUDES(safe_time_lock_);

  cdc::ProducerTabletInfo producer_tablet_info_;
  cdc::ConsumerTabletInfo consumer_tablet_info_;

  // Although this is processing serially, it might be on a different thread in the ThreadPool.
  // Using mutex to guarantee cache flush, preventing TSAN warnings.
  // Recursive, since when we abort the CDCReadRpc, that will also call the callback within the
  // same thread (HandlePoll()) which needs to Unregister poll_handle_ from rpcs_.
  std::mutex data_mutex_;

  std::atomic<bool> shutdown_ = false;

  OpIdPB op_id_ GUARDED_BY(data_mutex_);
  std::atomic<SchemaVersion> validated_schema_version_;
  std::atomic<SchemaVersion> last_compatible_consumer_schema_version_;

  Status status_ GUARDED_BY(data_mutex_);
  std::shared_ptr<cdc::GetChangesResponsePB> resp_ GUARDED_BY(data_mutex_);

  std::shared_ptr<XClusterOutputClientIf> output_client_;
  std::shared_ptr<XClusterClient> producer_client_;

  ThreadPool* thread_pool_;
  rpc::Rpcs* rpcs_;
  rpc::Rpcs::Handle poll_handle_ GUARDED_BY(data_mutex_);
  XClusterConsumer* xcluster_consumer_ GUARDED_BY(data_mutex_);

  mutable rw_spinlock safe_time_lock_;
  HybridTime producer_safe_time_ GUARDED_BY(safe_time_lock_);

  std::atomic<bool> is_polling_{true};
  int poll_failures_ GUARDED_BY(data_mutex_){0};
  int apply_failures_ GUARDED_BY(data_mutex_){0};
  int idle_polls_ GUARDED_BY(data_mutex_){0};
};

} // namespace tserver
} // namespace yb
