#include "market_data.hpp"

#include "book.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <format>
#include <iterator>
#include <print>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <utility>

namespace mex
{
namespace
{

constexpr std::size_t kMarketSymbolsMax = 256;
constexpr std::size_t kSubscribersMax = 32;
constexpr std::size_t kPendingBytesMax = 1024 * 1024;
constexpr auto kPublishInterval = std::chrono::milliseconds{20};
constexpr auto kHeartbeatInterval = std::chrono::seconds{1};
constexpr auto kSubscriberTimeout = std::chrono::seconds{5};
using Clock = std::chrono::steady_clock;

void Capture(const PriceLadder &p_ladder, std::span<DepthLevel> p_levels)
{
  // Both ladders keep their best price at the end.
  for (std::size_t index = 0; index < p_levels.size(); ++index)
  {
    const auto &level = p_ladder[p_ladder.size() - 1 - index];
    p_levels[index] = {level.price_, level.total_qty_};
  }
}

void AppendLevel(std::string &p_json, Price p_price, std::uint64_t p_quantity)
{
  if (p_json.back() != '[')
  {
    p_json += ',';
  }
  std::format_to(std::back_inserter(p_json), R"(["{}","{}"])", p_price.ticks_, p_quantity);
}

void AppendChanges(std::string &p_json, std::span<const DepthLevel> p_now,
                   std::span<const DepthLevel> p_before, Side p_side)
{
  p_json += '[';
  std::size_t now = 0;
  std::size_t before = 0;
  const auto better = [p_side](Price p_lhs, Price p_rhs)
  { return p_side == Side::Buy ? p_lhs > p_rhs : p_lhs < p_rhs; };

  while (now < p_now.size() || before < p_before.size())
  {
    if (before == p_before.size() ||
        (now < p_now.size() && better(p_now[now].price_, p_before[before].price_)))
    {
      AppendLevel(p_json, p_now[now].price_, p_now[now].quantity_);
      ++now;
    }
    else if (now == p_now.size() || better(p_before[before].price_, p_now[now].price_))
    {
      AppendLevel(p_json, p_before[before].price_, 0);
      ++before;
    }
    else
    {
      if (p_now[now].quantity_ != p_before[before].quantity_)
      {
        AppendLevel(p_json, p_now[now].price_, p_now[now].quantity_);
      }
      ++now;
      ++before;
    }
  }
  p_json += ']';
}

void AppendBook(std::string &p_json, wire::SymbolId p_symbol, std::uint64_t p_sequence,
                const MarketDepth &p_now, const MarketDepth &p_before, std::string_view p_type)
{
  std::format_to(std::back_inserter(p_json), R"({{"type":"{}","symbol":{},"seq":"{}","bids":)",
                 p_type, p_symbol, p_sequence);
  AppendChanges(p_json, {p_now.bids_.data(), p_now.bid_count_},
                {p_before.bids_.data(), p_before.bid_count_}, Side::Buy);
  p_json += R"(,"asks":)";
  AppendChanges(p_json, {p_now.asks_.data(), p_now.ask_count_},
                {p_before.asks_.data(), p_before.ask_count_}, Side::Sell);
  p_json += "}\n";
}

[[nodiscard]] bool SameDepth(const MarketDepth &p_now, const MarketDepth &p_before)
{
  return std::ranges::equal(std::span{p_now.bids_}.first(p_now.bid_count_),
                            std::span{p_before.bids_}.first(p_before.bid_count_)) &&
         std::ranges::equal(std::span{p_now.asks_}.first(p_now.ask_count_),
                            std::span{p_before.asks_}.first(p_before.ask_count_));
}

struct Subscriber
{
  UniqueFd socket_;
  std::string pending_{};
  std::size_t sent_ = 0;
  Clock::time_point progress_ = Clock::now();
};

// Partial writes retain their place in the stream. A slow socket can consume
// at most one fixed budget. Close it before dropping any part of the stream.
[[nodiscard]] bool Send(Subscriber &p_subscriber, std::string_view p_update,
                        Clock::time_point p_now)
{
  auto &pending = p_subscriber.pending_;
  auto &sent = p_subscriber.sent_;
  if (!p_update.empty())
  {
    pending.erase(0, sent);
    sent = 0;
    if (pending.size() + p_update.size() > kPendingBytesMax)
    {
      return false;
    }
    if (pending.empty())
    {
      p_subscriber.progress_ = p_now;
    }
    pending += p_update;
  }
  if (sent == pending.size())
  {
    return true;
  }
  const ssize_t count = ::send(p_subscriber.socket_.Get(), pending.data() + sent,
                               pending.size() - sent, MSG_NOSIGNAL);
  if (count > 0)
  {
    sent += static_cast<std::size_t>(count);
    p_subscriber.progress_ = p_now;
    return true;
  }
  return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
         p_now - p_subscriber.progress_ < kSubscriberTimeout;
}

} // namespace

void MarketBook::Publish(const Book &p_book)
{
  auto &slot = slots_[write_slot_];
  const auto &bids = p_book.Bids();
  const auto &asks = p_book.Asks();
  slot.bid_count_ = std::min(kMarketDepth, bids.size());
  slot.ask_count_ = std::min(kMarketDepth, asks.size());
  Capture(bids, std::span{slot.bids_}.first(slot.bid_count_));
  Capture(asks, std::span{slot.asks_}.first(slot.ask_count_));
  write_slot_ = exchange_slot_.exchange(write_slot_ | kReady, std::memory_order_acq_rel) & ~kReady;
}

std::optional<MarketDepth> MarketBook::Take()
{
  if ((exchange_slot_.load(std::memory_order_relaxed) & kReady) == 0)
  {
    return std::nullopt;
  }
  // Only this reader can clear the ready bit. The exchange acquires the slot
  // contents and releases the previously read slot to the writer.
  read_slot_ = exchange_slot_.exchange(read_slot_, std::memory_order_acq_rel) & ~kReady;
  return slots_[read_slot_];
}

MarketDataPublisher::MarketDataPublisher(TcpListener p_listener, const Config &p_config)
    : listener_{std::move(p_listener)}, address_{p_config.gateway_.listen_address_},
      hello_{std::format(R"({{"type":"hello","version":1,"depth":{},"symbols":[)", kMarketDepth)}
{
  for (const auto &symbol : p_config.symbols_)
  {
    if (!symbols_.empty())
    {
      hello_ += ',';
    }
    symbols_.push_back(symbol.symbol_id_);
    books_.try_emplace(symbol.symbol_id_, Symbol{std::make_unique<MarketBook>()});
    std::format_to(std::back_inserter(hello_), "{}", symbol.symbol_id_);
  }
  hello_ += "]}\n";
}

std::expected<MarketDataPublisher, std::string> MarketDataPublisher::Create(const Config &p_config,
                                                                            std::uint16_t p_port)
{
  if (p_config.symbols_.size() > kMarketSymbolsMax)
  {
    return std::unexpected(std::format("market data supports up to {} symbols", kMarketSymbolsMax));
  }
  auto listener = OpenTcpListener(p_config.gateway_.listen_address_, p_port);
  if (!listener)
  {
    return std::unexpected(std::move(listener.error()));
  }
  return MarketDataPublisher{std::move(*listener), p_config};
}

MarketBook &MarketDataPublisher::ForSymbol(wire::SymbolId p_symbol) const
{
  return *books_.find(p_symbol)->second.book_;
}

std::jthread MarketDataPublisher::Start() &
{
  std::println("mex_exchange: market data on {}:{}", address_, listener_.port_);
  return std::jthread{[this](std::stop_token p_stop) { Run(p_stop); }};
}

std::string MarketDataPublisher::Snapshot() const
{
  static constexpr MarketDepth kEmpty{};
  std::string snapshot = hello_;
  for (const wire::SymbolId symbol : symbols_)
  {
    const auto &book = books_.find(symbol)->second;
    AppendBook(snapshot, symbol, book.sequence_, book.depth_, kEmpty, "snapshot");
  }
  return snapshot;
}

std::string MarketDataPublisher::CollectUpdates()
{
  std::string updates;
  for (const wire::SymbolId symbol : symbols_)
  {
    auto &book = books_.find(symbol)->second;
    const auto current = book.book_->Take();
    if (current && !SameDepth(*current, book.depth_))
    {
      ++book.sequence_;
      AppendBook(updates, symbol, book.sequence_, *current, book.depth_, "update");
      book.depth_ = *current;
    }
  }
  return updates;
}

void MarketDataPublisher::Run(std::stop_token p_stop)
{
  std::vector<Subscriber> subscribers;
  subscribers.reserve(kSubscribersMax);
  auto next_tick = Clock::now();
  auto next_heartbeat = next_tick + kHeartbeatInterval;
  while (!p_stop.stop_requested())
  {
    const auto now = Clock::now();
    auto updates = CollectUpdates();
    if (now >= next_heartbeat)
    {
      updates += "{\"type\":\"heartbeat\"}\n";
      next_heartbeat = now + kHeartbeatInterval;
    }
    std::erase_if(subscribers, [&](Subscriber &p_client) { return !Send(p_client, updates, now); });

    std::optional<std::string> snapshot;
    for (std::size_t count = 0; count < kSubscribersMax; ++count)
    {
      UniqueFd socket{
          ::accept4(listener_.fd_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
      if (!socket.IsOpen())
      {
        break;
      }
      if (subscribers.size() == kSubscribersMax)
      {
        break;
      }

      if (!snapshot)
      {
        snapshot = Snapshot();
      }
      Subscriber client{.socket_ = std::move(socket)};
      if (Send(client, *snapshot, now))
      {
        subscribers.push_back(std::move(client));
      }
    }
    next_tick = std::max(next_tick + kPublishInterval, Clock::now());
    std::this_thread::sleep_until(next_tick);
  }
}

} // namespace mex
