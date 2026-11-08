#include "replicated_exchange.hpp"

#include "engine.hpp"
#include "gateway.hpp"
#include "replication.hpp"
#include "spsc_queue.hpp"
#include "vsr/io/client.hpp"
#include "vsr/io/server.hpp"
#include "vsr/replica.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <print>
#include <pthread.h>
#include <thread>
#include <utility>
#include <vector>

namespace mex
{
namespace
{

constexpr int kExitFailure = 1;
constexpr auto kStopPollInterval = std::chrono::milliseconds{50};

void NameThread(const char *p_name)
{
  if (const int named = ::pthread_setname_np(::pthread_self(), p_name); named != 0)
  {
    std::println(stderr, "mex_exchange: warning: cannot name thread {}: {}", p_name,
                 std::strerror(named));
  }
}

} // namespace

int RunReplicatedExchange(const Config &p_config, const ReplicaArguments &p_arguments,
                          const std::atomic<bool> &p_stop_requested)
{
  // Before the storage opens: an operation older than this process has no
  // session here.
  const auto start_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
  Config config = p_config;
  config.gateway_.port_ = config.replicas_[p_arguments.replica_].gateway_port_;
  auto sockets = OpenGatewaySockets(config.gateway_, p_arguments.io_);
  if (!sockets.has_value())
  {
    std::println(stderr, "mex_exchange: {}", sockets.error());
    return kExitFailure;
  }
  const std::uint16_t port = sockets->port_;

  // Gateway -> bridge, and state machine -> gateway, one pair for each engine.
  std::vector<std::unique_ptr<SpscQueue<Command>>> inbound;
  std::vector<std::unique_ptr<SpscQueue<Report>>> outbound;
  std::vector<EngineQueues> queues;
  std::vector<SpscQueue<Report> *> report_queues;
  for (std::size_t index = 0; index < config.engines_.size(); ++index)
  {
    inbound.push_back(
        std::make_unique<SpscQueue<Command>>(config.gateway_.inbound_queue_capacity_));
    outbound.push_back(
        std::make_unique<SpscQueue<Report>>(config.gateway_.outbound_queue_capacity_));
    queues.push_back({*inbound.back(), *outbound.back()});
    report_queues.push_back(outbound.back().get());
  }

  ExchangeStateMachine state_machine{config, report_queues};
  const vsr::Config vsr_config =
      MakeVsrConfig(config, p_arguments.replica_, state_machine.SnapshotBytesMax());
  std::vector<std::string> addresses;
  for (const ReplicaConfig &replica : config.replicas_)
  {
    addresses.push_back(replica.replication_address_);
  }

  vsr::io::ServerOptions server_options;
  server_options.config_ = vsr_config;
  server_options.addresses_ = addresses;
  server_options.data_path_ = p_arguments.data_path_;
  // Create applies again the committed operations of the storage file.
  auto server = vsr::io::Server::Create(std::move(server_options), state_machine);
  if (!server.has_value())
  {
    std::println(stderr, "mex_exchange: replica: {}", server.error());
    return kExitFailure;
  }
  state_machine.EmitReportsAfter(server->GetReplica().Op(), start_ns);

  vsr::io::ClientOptions client_options;
  client_options.cluster_ = vsr_config.cluster_;
  client_options.addresses_ = addresses;
  client_options.body_bytes_max_ = vsr_config.body_bytes_max_;
  auto client = vsr::io::Client::Create(std::move(client_options));
  if (!client.has_value())
  {
    std::println(stderr, "mex_exchange: replication client: {}", client.error());
    return kExitFailure;
  }

  ReplicationBridge bridge{std::move(*client), queues, vsr_config.body_bytes_max_};
  Gateway gateway{config, std::move(*sockets), queues};

  std::atomic<bool> replica_ended{false};
  std::optional<vsr::HaltReason> halt_reason{}; // read after the replica thread ends
  std::jthread replica_thread{[&server, &replica_ended, &halt_reason]
                              {
                                NameThread("mex-replica");
                                if (const auto ran = server->Run(); !ran.has_value())
                                {
                                  halt_reason = ran.error();
                                }
                                replica_ended.store(true, std::memory_order_relaxed);
                              }};
  std::jthread gateway_thread{[&gateway]
                              {
                                NameThread("mex-gateway");
                                gateway.Run();
                              }};
  std::jthread bridge_thread{[&bridge]
                             {
                               NameThread("mex-bridge");
                               bridge.Run();
                             }};
  std::println("mex_exchange: listening on {}:{} as replica {} of {}",
               config.gateway_.listen_address_, port, p_arguments.replica_,
               config.replicas_.size());
  std::fflush(stdout);

  while (!p_stop_requested.load(std::memory_order_relaxed) &&
         !replica_ended.load(std::memory_order_relaxed))
  {
    std::this_thread::sleep_for(kStopPollInterval);
  }

  bridge.Stop();
  bridge_thread.join();
  // The replica before the gateway: the state machine can spin on a full
  // outbound queue, and only the running gateway drains it.
  server->Stop();
  replica_thread.join();
  gateway.Stop();
  gateway_thread.join();

  // Replicas that committed the same operations print the same digest.
  std::println("mex_exchange: state digest {:016x} at operation {}", state_machine.StateDigest(),
               server->GetReplica().CommitMin());
  std::println("mex_exchange: bridge abandoned orders: {}", bridge.AbandonedOrders());
  std::println("mex_exchange: gateway dropped reports: {}", gateway.DroppedReports());
  if (halt_reason.has_value())
  {
    std::println(stderr, "mex_exchange: the replica halted, reason {}",
                 std::to_underlying(*halt_reason));
    return kExitFailure;
  }
  return 0;
}

} // namespace mex
