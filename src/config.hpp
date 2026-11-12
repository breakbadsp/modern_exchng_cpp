#pragma once

#include "engine.hpp"
#include "protocol.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Startup configuration (prep/io-design-spec.md, step 5). Read once before any
// thread starts; nothing reads the file after that. The syntax is in the spec
// and in config/example.cfg.
namespace mex
{

struct EngineConfig
{
  std::uint32_t core_ = 0;
  std::vector<BookConfig> books_{}; // books_[i] serves symbol_slot_ i
};

// One per symbol line, in file order. This is the symbol_id -> (engine, slot)
// routing table and the price band the gateway checks.
struct SymbolConfig
{
  wire::SymbolId symbol_id_ = 0;
  std::uint32_t engine_ = 0;
  std::uint32_t slot_ = 0; // index into engines_[engine_].books_
  Price min_price_{};
  Price max_price_{};
};

// The index in Config::sessions_ is the SessionId.
struct SessionConfig
{
  wire::Name session_name_{};
  wire::Name token_{};
};

struct GatewayConfig
{
  std::string listen_address_{}; // IPv4, dotted decimal
  std::uint16_t port_ = 0;
  std::uint32_t max_connections_ = 0;
  std::uint32_t inbound_queue_capacity_ = 0;  // power of two
  std::uint32_t outbound_queue_capacity_ = 0; // power of two
};

// One copy of the exchange in a replicated cluster (docs/replication.md).
// The index in Config::replicas_ is the replica number.
struct ReplicaConfig
{
  std::string replication_address_{}; // "a.b.c.d:port" for the other replicas
  std::uint16_t gateway_port_ = 0;    // the client port of this replica
};

// libvsr supports clusters up to this size.
constexpr std::size_t kMaxReplicas = 7;

struct Config
{
  std::vector<EngineConfig> engines_{};
  std::vector<SymbolConfig> symbols_{};
  std::vector<SessionConfig> sessions_{};
  GatewayConfig gateway_{};
  std::vector<ReplicaConfig> replicas_{}; // empty: one exchange, no replication
};

struct ConfigError
{
  std::size_t line_ = 0; // 1-based; 0 when the error is not on one line
  std::string reason_{};
};

[[nodiscard]] std::expected<Config, ConfigError> ParseConfig(std::string_view p_text);

[[nodiscard]] std::expected<Config, ConfigError> LoadConfig(const std::filesystem::path &p_path);

} // namespace mex
