#include "replication.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <iostream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mex
{
namespace
{

constexpr std::size_t kOrdersPerBatch = 2;
constexpr std::size_t kNewOrderEntryBytes = sizeof(SessionId) + wire::kFrameSize<wire::NewOrder>;

enum class Outcome
{
  Timeout = 0,
  Evicted,
  Reply,
};

struct Script
{
  Outcome outcome_ = Outcome::Timeout;
  bool valid_ = true;
};

class Client final
{
public:
  explicit Client(Script &p_script) : script_{&p_script} {}

  [[nodiscard]] std::expected<std::vector<std::byte>, vsr::io::ClientError>
  Request(std::span<const std::byte> p_body, std::chrono::milliseconds)
  {
    std::print("send");
    while (!p_body.empty())
    {
      if (p_body.size() < sizeof(SessionId))
      {
        script_->valid_ = false;
        break;
      }
      const auto frame = wire::DecodeClientFrame(p_body.subspan(sizeof(SessionId)));
      const auto *order = std::get_if<wire::NewOrder>(&frame.message_);
      if (frame.status_ != wire::DecodeStatus::Complete || order == nullptr)
      {
        script_->valid_ = false;
        break;
      }
      std::print(" {}", order->request_number_.value_);
      p_body = p_body.subspan(sizeof(SessionId) + frame.frame_size_);
    }
    return Reply();
  }

  [[nodiscard]] std::expected<std::vector<std::byte>, vsr::io::ClientError>
  Resume(std::chrono::milliseconds)
  {
    std::print("resume");
    return Reply();
  }

private:
  [[nodiscard]] std::expected<std::vector<std::byte>, vsr::io::ClientError> Reply() const
  {
    switch (script_->outcome_)
    {
    case Outcome::Timeout:
      return std::unexpected(vsr::io::ClientError::Timeout);
    case Outcome::Evicted:
      return std::unexpected(vsr::io::ClientError::Evicted);
    case Outcome::Reply:
      return std::vector<std::byte>{};
    }
    std::unreachable();
  }

  Script *script_;
};

[[nodiscard]] std::expected<void, std::string_view> Replay()
{
  Script script;
  SpscQueue<Command> inbound{8};
  SpscQueue<Report> outbound{8};
  const std::array queues{EngineQueues{inbound, outbound}};
  BasicReplicationBridge bridge{Client{script}, queues, kOrdersPerBatch * kNewOrderEntryBytes};
  std::string event;
  while (std::cin >> event)
  {
    if (event == "enqueue")
    {
      std::uint64_t id{};
      if (!(std::cin >> id) ||
          !inbound.TryPush(
              Command{.order_ = wire::NewOrder{.request_number_ = wire::RequestNumber{id}}}))
      {
        return std::unexpected("Invalid order in the replay");
      }
      std::println("enqueue | {}", bridge.AbandonedOrders());
      continue;
    }

    if (event == "send" || event == "timeout")
    {
      script.outcome_ = Outcome::Timeout;
    }
    else if (event == "evicted")
    {
      script.outcome_ = Outcome::Evicted;
    }
    else if (event == "reply")
    {
      script.outcome_ = Outcome::Reply;
    }
    else
    {
      return std::unexpected("Unknown bridge event");
    }
    if (!bridge.Step() || !script.valid_)
    {
      return std::unexpected("Bridge did not make the expected call");
    }
    std::println(" | {}", bridge.AbandonedOrders());
  }
  return {};
}

} // namespace
} // namespace mex

int main()
{
  const auto result = mex::Replay();
  if (!result)
  {
    std::println(stderr, "{}", result.error());
    return 1;
  }
  return 0;
}
