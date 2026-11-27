#pragma once

#include "gateway.hpp"
#include "protocol.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mex
{

constexpr std::size_t kMarketDepth = 20;

struct DepthLevel
{
  Price price_{};
  std::uint64_t quantity_ = 0;

  bool operator==(const DepthLevel &) const = default;
};

struct MarketDepth
{
  std::array<DepthLevel, kMarketDepth> bids_{};
  std::array<DepthLevel, kMarketDepth> asks_{};
  std::size_t bid_count_ = 0;
  std::size_t ask_count_ = 0;
};

// The writer and reader each own one slot. They exchange it with the third slot
// using one atomic index. The writer marks that slot ready. The reader clears
// the mark when it takes ownership. Storage stays fixed during bursts.
class MarketBook final
{
public:
  void Publish(const Book &p_book);
  [[nodiscard]] std::optional<MarketDepth> Take();

private:
  static constexpr unsigned kReady = 4;
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  std::array<MarketDepth, 3> slots_{};
  alignas(64) unsigned write_slot_ = 0;
  alignas(64) unsigned read_slot_ = 1;
  alignas(64) std::atomic<unsigned> exchange_slot_{2};
};

class MarketDataPublisher final
{
public:
  [[nodiscard]] static std::expected<MarketDataPublisher, std::string>
  Create(const Config &p_config, std::uint16_t p_port);

  // The registry is fixed at startup. Each symbol has exactly one writer.
  [[nodiscard]] MarketBook &ForSymbol(wire::SymbolId p_symbol) const pre(books_.contains(p_symbol));
  // Call once. Keep this publisher alive until the returned thread joins.
  [[nodiscard]] std::jthread Start() &;

private:
  struct Symbol
  {
    std::unique_ptr<MarketBook> book_;
    MarketDepth depth_{};
    std::uint64_t sequence_ = 0;
  };

  explicit MarketDataPublisher(TcpListener p_listener, const Config &p_config);
  [[nodiscard]] std::string Snapshot() const;
  [[nodiscard]] std::string CollectUpdates();
  void Run(std::stop_token p_stop);

  TcpListener listener_;
  std::string address_;
  std::vector<wire::SymbolId> symbols_;
  std::unordered_map<wire::SymbolId, Symbol> books_;
  std::string hello_;
};

} // namespace mex
