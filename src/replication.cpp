#include "replication.hpp"

#include "order_check.hpp"

#include <algorithm>
#include <concepts>
#include <cstring>

namespace mex
{
namespace
{

// The cluster id tells the replicas of one exchange from another cluster
// that reaches them by mistake.
constexpr std::uint64_t kClusterId = 0x6D65'7801;

// An unused inbound queue: the state machine calls Engine::Apply directly.
constexpr std::size_t kUnusedQueueCapacity = 2;

template <typename F> void PutField(std::span<std::byte> p_out, std::size_t &p_offset, F p_field)
{
  std::memcpy(p_out.subspan(p_offset, sizeof(F)).data(), &p_field, sizeof(F));
  p_offset += sizeof(F);
}

template <typename F>
[[nodiscard]] bool GetField(std::span<const std::byte> p_in, std::size_t &p_offset, F &p_field)
{
  if (p_in.size() - p_offset < sizeof(F))
  {
    return false;
  }
  std::memcpy(&p_field, p_in.subspan(p_offset, sizeof(F)).data(), sizeof(F));
  p_offset += sizeof(F);
  return true;
}

} // namespace

OrderBatch::OrderBatch(std::size_t p_bytes_max) : bytes_(p_bytes_max) {}

bool OrderBatch::TryAppend(SessionId p_session, const ClientOrder &p_order)
{
  if (entry_count_ == kBatchEntriesMax)
  {
    return false;
  }
  return std::visit(
      [this, p_session]<typename M>(const M &p_message)
      {
        if (bytes_.size() - used_bytes_ < sizeof(SessionId) + wire::kFrameSize<M>)
        {
          return false;
        }
        PutField(bytes_, used_bytes_, p_session);
        used_bytes_ += wire::Encode(p_message, std::span{bytes_}.subspan(used_bytes_));
        ++entry_count_;
        return true;
      },
      p_order);
}

std::span<const std::byte> OrderBatch::Bytes() const
{
  return std::span{bytes_}.first(used_bytes_);
}

std::uint32_t OrderBatch::EntryCount() const { return entry_count_; }

void OrderBatch::Clear()
{
  used_bytes_ = 0;
  entry_count_ = 0;
}

struct ExchangeStateMachine::EngineSlot
{
  EngineSlot(const EngineConfig &p_config, std::size_t p_session_count,
             SpscQueue<Report> &p_outbound)
      : engine_{p_config.books_, p_session_count, unused_inbound_, p_outbound}
  {
  }

  SpscQueue<Command> unused_inbound_{kUnusedQueueCapacity};
  Engine engine_;
};

ExchangeStateMachine::ExchangeStateMachine(const Config &p_config,
                                           std::span<SpscQueue<Report> *const> p_outbound)
    : routes_(p_config.symbols_), session_count_(p_config.sessions_.size())
{
  std::ranges::sort(routes_, {}, &SymbolConfig::symbol_id_);
  for (std::size_t index = 0; index < p_config.engines_.size(); ++index)
  {
    engines_.push_back(
        std::make_unique<EngineSlot>(p_config.engines_[index], session_count_, *p_outbound[index]));
  }
}

ExchangeStateMachine::~ExchangeStateMachine() = default;

void ExchangeStateMachine::EmitReportsAfter(std::uint64_t p_op, std::uint64_t p_realtime_ns)
{
  emit_after_op_ = p_op;
  emit_from_ns_ = p_realtime_ns;
}

std::size_t ExchangeStateMachine::SnapshotBytesMax() const
{
  std::size_t bytes = 0;
  for (const std::unique_ptr<EngineSlot> &slot : engines_)
  {
    bytes += sizeof(std::uint64_t) + slot->engine_.SnapshotBytesMax();
  }
  return bytes;
}

std::uint64_t ExchangeStateMachine::StateDigest() const
{
  // FNV-1a over the engine digests, in engine order.
  constexpr std::uint64_t kOffsetBasis = 0xCBF2'9CE4'8422'2325;
  constexpr std::uint64_t kPrime = 0x0000'0100'0000'01B3;
  std::uint64_t digest = kOffsetBasis;
  for (const std::unique_ptr<EngineSlot> &slot : engines_)
  {
    digest = (digest ^ slot->engine_.StateDigest()) * kPrime;
  }
  return digest;
}

std::size_t ExchangeStateMachine::Execute(const vsr::Operation &p_operation,
                                          std::span<const std::byte> p_body,
                                          std::span<std::byte> p_reply)
{
  contract_assert(p_operation.op_ < (std::uint64_t{1} << (64 - kBatchEntryIndexBits)));
  // A virtual function cannot have a precondition.
  contract_assert(p_reply.size() >= kBatchReplyBytes);
  const bool emit = p_operation.op_ > emit_after_op_ && p_operation.timestamp_ns_ >= emit_from_ns_;
  std::uint32_t applied_count = 0;
  std::uint32_t entry_index = 0;
  std::size_t offset = 0;
  // The body is bytes from the network. An entry that is not valid is not
  // applied; a frame that cannot be decoded ends the batch.
  while (entry_index < kBatchEntriesMax)
  {
    SessionId session = 0;
    if (!GetField(p_body, offset, session))
    {
      break;
    }
    const auto decoded = wire::DecodeClientFrame(p_body.subspan(offset));
    if (decoded.status_ != wire::DecodeStatus::Complete)
    {
      break;
    }
    offset += decoded.frame_size_;
    ++entry_index;
    const OpNumber op_number{(p_operation.op_ << kBatchEntryIndexBits) | entry_index};
    std::visit(
        [&]<typename M>(const M &p_message)
        {
          if constexpr (!std::same_as<M, wire::Logon>)
          {
            const auto route = std::ranges::lower_bound(routes_, p_message.symbol_id_, {},
                                                        &SymbolConfig::symbol_id_);
            if (session >= session_count_ || route == routes_.end() ||
                route->symbol_id_ != p_message.symbol_id_ || Check(*route, p_message).has_value())
            {
              return;
            }
            Engine &engine = engines_[route->engine_]->engine_;
            engine.SetReportsEnabled(emit);
            engine.Apply(Command{.op_number_ = op_number,
                                 .session_id_ = session,
                                 .symbol_slot_ = route->slot_,
                                 .order_ = p_message});
            ++applied_count;
          }
        },
        decoded.message_);
  }
  std::size_t reply_bytes = 0;
  PutField(p_reply, reply_bytes, applied_count);
  return reply_bytes;
}

std::expected<std::size_t, std::monostate>
ExchangeStateMachine::Snapshot(std::span<std::byte> p_out)
{
  std::size_t offset = 0;
  for (const std::unique_ptr<EngineSlot> &slot : engines_)
  {
    if (p_out.size() - offset < sizeof(std::uint64_t))
    {
      return std::unexpected(std::monostate{});
    }
    const std::size_t size_offset = offset;
    offset += sizeof(std::uint64_t);
    const std::optional<std::size_t> engine_bytes = slot->engine_.Snapshot(p_out.subspan(offset));
    if (!engine_bytes.has_value())
    {
      return std::unexpected(std::monostate{});
    }
    std::size_t at = size_offset;
    PutField(p_out, at, std::uint64_t{*engine_bytes});
    offset += *engine_bytes;
  }
  return offset;
}

bool ExchangeStateMachine::Restore(std::span<const std::byte> p_snapshot)
{
  std::size_t offset = 0;
  for (const std::unique_ptr<EngineSlot> &slot : engines_)
  {
    std::uint64_t engine_bytes = 0;
    if (!GetField(p_snapshot, offset, engine_bytes) || engine_bytes > p_snapshot.size() - offset)
    {
      return false;
    }
    const auto bytes = static_cast<std::size_t>(engine_bytes);
    if (!slot->engine_.Restore(p_snapshot.subspan(offset, bytes)))
    {
      return false;
    }
    offset += bytes;
  }
  return offset == p_snapshot.size();
}

vsr::Config MakeVsrConfig(const Config &p_config, std::uint8_t p_replica,
                          std::size_t p_snapshot_bytes_max)
{
  vsr::Config config;
  config.cluster_ = kClusterId;
  config.replica_count_ = static_cast<std::uint8_t>(p_config.replicas_.size());
  config.replica_ = p_replica;
  config.snapshot_bytes_max_ = p_snapshot_bytes_max;
  return config;
}

template class BasicReplicationBridge<vsr::io::Client>;

} // namespace mex
