#pragma once

#include "config.hpp"

#include <atomic>
#include <cstdint>
#include <string>

// One replica of a replicated exchange (docs/replication.md): a libvsr
// replica with the engines as its state machine, a gateway for clients, and a
// bridge that sends the orders of the gateway to the cluster.
namespace mex
{

struct ReplicaArguments
{
  std::uint8_t replica_ = 0; // index into Config::replicas_
  std::string data_path_{};  // the storage file of this replica
};

// Runs until p_stop_requested is true or the replica halts. Returns the
// process exit status.
[[nodiscard]] int RunReplicatedExchange(const Config &p_config, const ReplicaArguments &p_arguments,
                                        const std::atomic<bool> &p_stop_requested)
    pre(p_arguments.replica_ < p_config.replicas_.size());

} // namespace mex
