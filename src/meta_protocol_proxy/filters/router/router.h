#pragma once

#include "envoy/tcp/conn_pool.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "source/common/common/logger.h"

#include "src/meta_protocol_proxy/app_exception.h"
#include "src/meta_protocol_proxy/filters/filter.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {
namespace Router {

/**
 * This interface is used by an upstream request to communicate its state.
 */
class RequestOwner : public Logger::Loggable<Logger::Id::filter> {
public:
  RequestOwner(Upstream::ClusterManager& cluster_manager) : cluster_manager_(cluster_manager) {}
  virtual ~RequestOwner() = default;

  /**
   * @return ConnectionPool::UpstreamCallbacks& the handler for upstream data.
   */
  virtual Tcp::ConnectionPool::UpstreamCallbacks& upstreamCallbacks() PURE;

  virtual void continueDecoding() PURE;
  virtual void sendLocalReply(const DirectResponse& response, bool end_stream) PURE;
  virtual CodecPtr createCodec() PURE;
  virtual void resetStream() PURE;
  virtual void setUpstreamConnection(Tcp::ConnectionPool::ConnectionDataPtr conn) PURE;
  virtual void onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host) PURE;

protected:
  struct PrepareUpstreamRequestResult {
    absl::optional<AppException> exception;
    absl::optional<Upstream::TcpPoolData> conn_pool_data;
    std::string response_code_detail;
  };

  PrepareUpstreamRequestResult prepareUpstreamRequest(const std::string& cluster_name,
                                                      uint64_t request_id,
                                                      Upstream::LoadBalancerContext* lb_context) {
    Upstream::ThreadLocalCluster* cluster = cluster_manager_.getThreadLocalCluster(cluster_name);
    if (!cluster) {
      ENVOY_LOG(warn, "meta protocol router: unknown cluster '{}'", cluster_name);
      return {AppException(
                  Error{ErrorType::ClusterNotFound,
                        fmt::format("meta protocol router: unknown cluster '{}'", cluster_name)}),
              absl::nullopt, "unknown_cluster"};
    }

    cluster_ = cluster->info();
    ENVOY_LOG(debug, "meta protocol router: cluster {} match for request '{}'", cluster_->name(),
              request_id);

    if (cluster_->maintenanceMode()) {
      ENVOY_LOG(warn, "meta protocol router: maintenance mode for cluster '{}'", cluster_name);
      return {
          AppException(Error{ErrorType::Unspecified,
                             fmt::format("meta protocol router: maintenance mode for cluster '{}'",
                                         cluster_name)}),
          absl::nullopt, "cluster_in_maintenance_mode"};
    }

    auto conn_pool_data = cluster->tcpConnPool(Upstream::ResourcePriority::Default, lb_context);
    if (!conn_pool_data) {
      ENVOY_LOG(warn, "meta protocol router: no healthy upstream for '{}'", cluster_name);
      return {AppException(Error{
                  ErrorType::NoHealthyUpstream,
                  fmt::format("meta protocol router: no healthy upstream for '{}'", cluster_name)}),
              absl::nullopt, "no_healthy_upstream"};
    }

    return {absl::nullopt, conn_pool_data, ""};
  }

  Upstream::ClusterInfoConstSharedPtr cluster_;

private:
  Upstream::ClusterManager& cluster_manager_;
};

/**
 * ShadowRouterHandle is used to write a request or release a connection early if needed.
 */
class ShadowRouterHandle {
public:
  virtual ~ShadowRouterHandle() = default;

  /**
   * @return RequestOwner& the interface associated with this ShadowRouter.
   */
  virtual RequestOwner& requestOwner() PURE;
};

/**
 * ShadowWriter is used for submitting requests and ignoring the response.
 */
class ShadowWriter {
public:
  virtual ~ShadowWriter() = default;

  /**
   * @return Upstream::ClusterManager& the cluster manager.
   */
  virtual Upstream::ClusterManager& clusterManager() PURE;

  /**
   * @return Dispatcher& the dispatcher.
   */
  virtual Event::Dispatcher& dispatcher() PURE;

  /**
   * Starts the shadow request by requesting an upstream connection.
   */
  virtual void submit(const std::string& cluster_name, MetadataSharedPtr request_metadata,
                      MutationSharedPtr mutation, CodecFactory& codec_factory) PURE;
};

} // namespace Router
} // namespace MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
