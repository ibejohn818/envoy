#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>

#include "envoy/common/exception.h"
#include "envoy/extensions/wasm/v3/wasm.pb.validate.h"
#include "envoy/http/filter.h"
#include "envoy/server/lifecycle_notifier.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "common/config/datasource.h"
#include "common/stats/symbol_table_impl.h"
#include "common/version/version.h"

#include "extensions/common/wasm/context.h"
#include "extensions/common/wasm/wasm_extension.h"
#include "extensions/common/wasm/wasm_vm.h"
#include "extensions/common/wasm/well_known_names.h"

#include "include/proxy-wasm/exports.h"
#include "include/proxy-wasm/wasm.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Wasm {

#define ALL_WASM_STATS(COUNTER, GAUGE)                                                             \
  COUNTER(created)                                                                                 \
  GAUGE(active, NeverImport)

class WasmHandle;

struct WasmStats {
  ALL_WASM_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

// Wasm execution instance. Manages the Envoy side of the Wasm interface.
class Wasm : public WasmBase, Logger::Loggable<Logger::Id::wasm> {
public:
  Wasm(absl::string_view runtime, absl::string_view vm_id, absl::string_view vm_configuration,
       absl::string_view vm_key, const Stats::ScopeSharedPtr& scope,
       Upstream::ClusterManager& cluster_manager, Event::Dispatcher& dispatcher);
  Wasm(std::shared_ptr<WasmHandle> other, Event::Dispatcher& dispatcher);
  ~Wasm() override;

  Upstream::ClusterManager& clusterManager() const { return cluster_manager_; }
  Event::Dispatcher& dispatcher() { return dispatcher_; }
  Context* getRootContext(const std::shared_ptr<PluginBase>& plugin, bool allow_closed) {
    return static_cast<Context*>(WasmBase::getRootContext(plugin, allow_closed));
  }
  void setTimerPeriod(uint32_t root_context_id, std::chrono::milliseconds period) override;
  virtual void tickHandler(uint32_t root_context_id);
  std::shared_ptr<Wasm> sharedThis() { return std::static_pointer_cast<Wasm>(shared_from_this()); }
  Network::DnsResolverSharedPtr& dnsResolver() { return dns_resolver_; }

  // WasmBase
  void error(absl::string_view message) override;
  proxy_wasm::CallOnThreadFunction callOnThreadFunction() override;
  ContextBase* createContext(const std::shared_ptr<PluginBase>& plugin) override;
  ContextBase* createRootContext(const std::shared_ptr<PluginBase>& plugin) override;
  ContextBase* createVmContext() override;
  void registerCallbacks() override;
  void getFunctions() override;

  // AccessLog::Instance
  void log(const PluginSharedPtr& plugin, const Http::RequestHeaderMap* request_headers,
           const Http::ResponseHeaderMap* response_headers,
           const Http::ResponseTrailerMap* response_trailers,
           const StreamInfo::StreamInfo& stream_info);

  void onStatsUpdate(const PluginSharedPtr& plugin, Envoy::Stats::MetricSnapshot& snapshot);

  virtual std::string buildVersion() { return BUILD_VERSION_NUMBER; }

  void initializeLifecycle(Server::ServerLifecycleNotifier& lifecycle_notifier);
  uint32_t nextDnsToken() {
    do {
      dns_token_++;
    } while (!dns_token_);
    return dns_token_;
  }

  void setCreateContextForTesting(CreateContextFn create_context,
                                  CreateContextFn create_root_context) {
    create_context_for_testing_ = create_context;
    create_root_context_for_testing_ = create_root_context;
  }
  void setFailStateForTesting(proxy_wasm::FailState fail_state) { failed_ = fail_state; }

protected:
  friend class Context;

  void initializeStats();
  // Calls into the VM.
  proxy_wasm::WasmCallVoid<3> on_resolve_dns_;
  proxy_wasm::WasmCallVoid<2> on_stats_update_;

  Stats::ScopeSharedPtr scope_;
  Upstream::ClusterManager& cluster_manager_;
  Event::Dispatcher& dispatcher_;
  Event::PostCb server_shutdown_post_cb_;
  absl::flat_hash_map<uint32_t, Event::TimerPtr> timer_; // per root_id.
  TimeSource& time_source_;

  // Host Stats/Metrics
  WasmStats wasm_stats_;

  // Plugin Stats/Metrics
  absl::flat_hash_map<uint32_t, Stats::Counter*> counters_;
  absl::flat_hash_map<uint32_t, Stats::Gauge*> gauges_;
  absl::flat_hash_map<uint32_t, Stats::Histogram*> histograms_;

  CreateContextFn create_context_for_testing_;
  CreateContextFn create_root_context_for_testing_;
  Network::DnsResolverSharedPtr dns_resolver_;
  uint32_t dns_token_ = 1;
};
using WasmSharedPtr = std::shared_ptr<Wasm>;

class WasmHandle : public WasmHandleBase, public ThreadLocal::ThreadLocalObject {
public:
  explicit WasmHandle(const WasmSharedPtr& wasm)
      : WasmHandleBase(std::static_pointer_cast<WasmBase>(wasm)), wasm_(wasm) {}

  WasmSharedPtr& wasm() { return wasm_; }

private:
  WasmSharedPtr wasm_;
};

using WasmHandleSharedPtr = std::shared_ptr<WasmHandle>;

class PluginHandle : public PluginHandleBase, public ThreadLocal::ThreadLocalObject {
public:
  explicit PluginHandle(const WasmHandleSharedPtr& wasm_handle, absl::string_view plugin_key)
      : PluginHandleBase(std::static_pointer_cast<WasmHandleBase>(wasm_handle), plugin_key),
        wasm_handle_(wasm_handle) {}

  WasmSharedPtr& wasm() { return wasm_handle_->wasm(); }
  WasmHandleSharedPtr& wasmHandleForTest() { return wasm_handle_; }

private:
  WasmHandleSharedPtr wasm_handle_;
};

using PluginHandleSharedPtr = std::shared_ptr<PluginHandle>;

using CreateWasmCallback = std::function<void(WasmHandleSharedPtr)>;

// Returns false if createWasm failed synchronously. This is necessary because xDS *MUST* report
// all failures synchronously as it has no facility to report configuration update failures
// asynchronously. Callers should throw an exception if they are part of a synchronous xDS update
// because that is the mechanism for reporting configuration errors.
bool createWasm(const VmConfig& vm_config, const PluginSharedPtr& plugin,
                const Stats::ScopeSharedPtr& scope, Upstream::ClusterManager& cluster_manager,
                Init::Manager& init_manager, Event::Dispatcher& dispatcher, Api::Api& api,
                Envoy::Server::ServerLifecycleNotifier& lifecycle_notifier,
                Config::DataSource::RemoteAsyncDataProviderPtr& remote_data_provider,
                CreateWasmCallback&& callback,
                CreateContextFn create_root_context_for_testing = nullptr);

PluginHandleSharedPtr
getOrCreateThreadLocalPlugin(const WasmHandleSharedPtr& base_wasm, const PluginSharedPtr& plugin,
                             Event::Dispatcher& dispatcher,
                             CreateContextFn create_root_context_for_testing = nullptr);

void clearCodeCacheForTesting();
std::string anyToBytes(const ProtobufWkt::Any& any);
void setTimeOffsetForCodeCacheForTesting(MonotonicTime::duration d);
EnvoyWasm::WasmEvent toWasmEvent(const std::shared_ptr<WasmHandleBase>& wasm);

} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy
