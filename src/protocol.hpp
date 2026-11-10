#pragma once

#include "types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

// Binary wire protocol between clients and the gateway (prep/io-design-spec.md).
// Frame: header {u16 length; u8 type;}, then for exchange -> client frames a
// u64 seq, then the body fields in declaration order with no padding.
// length is the full frame size, header included. All integers are little-endian.
// NewOrder, Cancel, and Replace start with the session's request number.
namespace mex::wire
{

// Fields are copied with memcpy, so the host byte order is the wire byte order.
static_assert(std::endian::native == std::endian::little);

using SymbolId = std::uint32_t;
using ClientOrderId = std::uint64_t;
using ExchangeOrderId = std::uint64_t;

// Identifies one NewOrder, Cancel, or Replace of a session, so that an engine
// executes it at most once. The client starts at 1 and increases the number
// for each new request, also across reconnects. A retry repeats the request
// unchanged, with its number. Distinct from ClientOrderId, which names an order.
struct RequestNumber
{
  std::uint64_t value_ = 0;

  constexpr auto operator<=>(const RequestNumber &) const = default;
};

static_assert(sizeof(RequestNumber) == 8);

constexpr std::size_t kNameSize = 16;

// Zero-padded, not zero-terminated when all 16 bytes are used.
using Name = std::array<char, kNameSize>;

enum class OrderType : std::uint8_t
{
  Limit = 0,
  Market,
};

enum class Liquidity : std::uint8_t
{
  Maker = 0,
  Taker,
};

// One reason code for LogonReject, Reject, CancelReject, ReplaceReject, and Done.
enum class Reason : std::uint8_t
{
  BadCredentials = 0,
  AlreadyLoggedOn,
  UnknownSymbol,
  InvalidQuantity,
  PriceOutOfRange,
  DuplicateClientOrderId,
  SystemBusy,
  BookFull,
  UnknownOrder,
  TooLate,
  CannotRest,
  MarketRemainder,
  DuplicateRequest,
};

enum class MsgType : std::uint8_t
{
  Logon = 1,
  NewOrder,
  Cancel,
  Replace,
  LogonAck = 64,
  LogonReject,
  Ack,
  Reject,
  Fill,
  CancelAck,
  CancelReject,
  ReplaceAck,
  ReplaceReject,
  Done,
};

[[nodiscard]] constexpr bool IsValid(Side p_value)
{
  return std::to_underlying(p_value) <= std::to_underlying(Side::Sell);
}

[[nodiscard]] constexpr bool IsValid(OrderType p_value)
{
  return std::to_underlying(p_value) <= std::to_underlying(OrderType::Market);
}

[[nodiscard]] constexpr bool IsValid(Liquidity p_value)
{
  return std::to_underlying(p_value) <= std::to_underlying(Liquidity::Taker);
}

[[nodiscard]] constexpr bool IsValid(Reason p_value)
{
  return std::to_underlying(p_value) <= std::to_underlying(Reason::DuplicateRequest);
}

// Each message lists its wire fields once, in Fields(). Encode, decode, and the
// frame size all come from that list.

// Client -> exchange.

struct Logon
{
  static constexpr MsgType kType = MsgType::Logon;
  Name session_name_{};
  Name token_{};

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.session_name_, p_self.token_);
  }
  bool operator==(const Logon &) const = default;
};

struct NewOrder
{
  static constexpr MsgType kType = MsgType::NewOrder;
  RequestNumber request_number_{};
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Side side_ = Side::Buy;
  OrderType type_ = OrderType::Limit;
  Price price_{};
  Quantity qty_{};

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.request_number_, p_self.client_order_id_, p_self.symbol_id_, p_self.side_,
            p_self.type_, p_self.price_, p_self.qty_);
  }
  bool operator==(const NewOrder &) const = default;
};

struct Cancel
{
  static constexpr MsgType kType = MsgType::Cancel;
  RequestNumber request_number_{};
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.request_number_, p_self.client_order_id_, p_self.symbol_id_);
  }
  bool operator==(const Cancel &) const = default;
};

struct Replace
{
  static constexpr MsgType kType = MsgType::Replace;
  RequestNumber request_number_{};
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Price new_price_{};
  Quantity new_total_qty_{}; // filled part included (FIX style)

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.request_number_, p_self.client_order_id_, p_self.symbol_id_, p_self.new_price_,
            p_self.new_total_qty_);
  }
  bool operator==(const Replace &) const = default;
};

// Exchange -> client. Order reports carry symbol_id_ because a client_order_id
// is unique only per session per symbol.

struct LogonAck
{
  static constexpr MsgType kType = MsgType::LogonAck;

  constexpr void Fields(this auto &, auto &&p_visit) { p_visit(); }
  bool operator==(const LogonAck &) const = default;
};

struct LogonReject
{
  static constexpr MsgType kType = MsgType::LogonReject;
  Reason reason_ = Reason::BadCredentials;

  constexpr void Fields(this auto &p_self, auto &&p_visit) { p_visit(p_self.reason_); }
  bool operator==(const LogonReject &) const = default;
};

struct Ack
{
  static constexpr MsgType kType = MsgType::Ack;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  ExchangeOrderId exchange_order_id_ = 0;
  Quantity leaves_{};

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.exchange_order_id_, p_self.leaves_);
  }
  bool operator==(const Ack &) const = default;
};

struct Reject
{
  static constexpr MsgType kType = MsgType::Reject;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Reason reason_ = Reason::BadCredentials;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.reason_);
  }
  bool operator==(const Reject &) const = default;
};

struct Fill
{
  static constexpr MsgType kType = MsgType::Fill;
  ExchangeOrderId exchange_order_id_ = 0;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Price price_{};
  Quantity qty_{};
  Quantity leaves_{};
  Liquidity liquidity_ = Liquidity::Maker;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.exchange_order_id_, p_self.client_order_id_, p_self.symbol_id_, p_self.price_,
            p_self.qty_, p_self.leaves_, p_self.liquidity_);
  }
  bool operator==(const Fill &) const = default;
};

struct CancelAck
{
  static constexpr MsgType kType = MsgType::CancelAck;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_);
  }
  bool operator==(const CancelAck &) const = default;
};

struct CancelReject
{
  static constexpr MsgType kType = MsgType::CancelReject;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Reason reason_ = Reason::UnknownOrder;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.reason_);
  }
  bool operator==(const CancelReject &) const = default;
};

struct ReplaceAck
{
  static constexpr MsgType kType = MsgType::ReplaceAck;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Price price_{};
  Quantity leaves_{};

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.price_, p_self.leaves_);
  }
  bool operator==(const ReplaceAck &) const = default;
};

struct ReplaceReject
{
  static constexpr MsgType kType = MsgType::ReplaceReject;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Reason reason_ = Reason::UnknownOrder;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.reason_);
  }
  bool operator==(const ReplaceReject &) const = default;
};

// Leaves dropped by the book: CannotRest or MarketRemainder.
struct Done
{
  static constexpr MsgType kType = MsgType::Done;
  ClientOrderId client_order_id_ = 0;
  SymbolId symbol_id_ = 0;
  Reason reason_ = Reason::CannotRest;

  constexpr void Fields(this auto &p_self, auto &&p_visit)
  {
    p_visit(p_self.client_order_id_, p_self.symbol_id_, p_self.reason_);
  }
  bool operator==(const Done &) const = default;
};

// The reject that answers each client order message.
[[nodiscard]] constexpr Reject RejectOf(const NewOrder &p_order, Reason p_reason)
{
  return {.client_order_id_ = p_order.client_order_id_,
          .symbol_id_ = p_order.symbol_id_,
          .reason_ = p_reason};
}

[[nodiscard]] constexpr CancelReject RejectOf(const Cancel &p_cancel, Reason p_reason)
{
  return {.client_order_id_ = p_cancel.client_order_id_,
          .symbol_id_ = p_cancel.symbol_id_,
          .reason_ = p_reason};
}

[[nodiscard]] constexpr ReplaceReject RejectOf(const Replace &p_replace, Reason p_reason)
{
  return {.client_order_id_ = p_replace.client_order_id_,
          .symbol_id_ = p_replace.symbol_id_,
          .reason_ = p_reason};
}

using ClientMessage = std::variant<Logon, NewOrder, Cancel, Replace>;
using ExchangeMessage = std::variant<LogonAck, LogonReject, Ack, Reject, Fill, CancelAck,
                                     CancelReject, ReplaceAck, ReplaceReject, Done>;

constexpr std::size_t kHeaderSize = sizeof(std::uint16_t) + sizeof(MsgType);
constexpr std::size_t kSeqSize = sizeof(std::uint64_t);

template <typename M>
constexpr bool kFromExchange =
    std::to_underlying(M::kType) >= std::to_underlying(MsgType::LogonAck);

template <typename M> [[nodiscard]] consteval std::size_t BodySize()
{
  const M message{};
  std::size_t size = 0;
  message.Fields(
      [&size]<typename... F>(const F &...)
      {
        // memcpy of a field is its wire form only if it has no padding bytes.
        static_assert((std::has_unique_object_representations_v<F> && ...));
        size = (std::size_t{0} + ... + sizeof(F));
      });
  return size;
}

template <typename M>
constexpr std::size_t kFrameSize = kHeaderSize + (kFromExchange<M> ? kSeqSize : 0) + BodySize<M>();

template <typename Variant> struct MaxFrameSize;

template <typename... M> struct MaxFrameSize<std::variant<M...>>
{
  static constexpr std::size_t kValue = std::max({kFrameSize<M>...});
};

// A read buffer of this size always holds at least one whole frame.
constexpr std::size_t kMaxFrameSize =
    std::max(MaxFrameSize<ClientMessage>::kValue, MaxFrameSize<ExchangeMessage>::kValue);

static_assert(kMaxFrameSize <= std::numeric_limits<std::uint16_t>::max());

namespace detail
{

class Writer final
{
public:
  explicit Writer(std::span<std::byte> p_out) : out_{p_out} {}

  template <typename F> void Put(const F &p_field)
  {
    std::memcpy(out_.subspan(offset_, sizeof(F)).data(), &p_field, sizeof(F));
    offset_ += sizeof(F);
  }

  [[nodiscard]] std::size_t Offset() const { return offset_; }

private:
  std::span<std::byte> out_;
  std::size_t offset_ = 0;
};

class Reader final
{
public:
  explicit Reader(std::span<const std::byte> p_in) : in_{p_in} {}

  // An enum field outside its enumerators marks the frame invalid.
  template <typename F> void Get(F &p_field)
  {
    std::memcpy(&p_field, in_.subspan(offset_, sizeof(F)).data(), sizeof(F));
    offset_ += sizeof(F);
    if constexpr (std::is_enum_v<F>)
    {
      valid_ = valid_ && IsValid(p_field);
    }
  }

  [[nodiscard]] bool Valid() const { return valid_; }

private:
  std::span<const std::byte> in_;
  std::size_t offset_ = 0;
  bool valid_ = true;
};

template <typename M>
std::size_t EncodeFrame(std::uint64_t p_seq, const M &p_message, std::span<std::byte> p_out)
{
  Writer writer{p_out};
  writer.Put(static_cast<std::uint16_t>(kFrameSize<M>));
  writer.Put(M::kType);
  if constexpr (kFromExchange<M>)
  {
    writer.Put(p_seq);
  }
  p_message.Fields([&writer](const auto &...p_field) { (writer.Put(p_field), ...); });
  return writer.Offset();
}

} // namespace detail

// Client -> exchange frame. Returns the bytes written, always kFrameSize<M>.
template <typename M>
  requires(!kFromExchange<M>)
[[nodiscard]] std::size_t Encode(const M &p_message, std::span<std::byte> p_out)
    pre(p_out.size() >= kFrameSize<M>)
{
  return detail::EncodeFrame(0, p_message, p_out);
}

// Exchange -> client frame with the session's outbound sequence number.
template <typename M>
  requires kFromExchange<M>
[[nodiscard]] std::size_t Encode(std::uint64_t p_seq, const M &p_message,
                                 std::span<std::byte> p_out) pre(p_out.size() >= kFrameSize<M>)
{
  return detail::EncodeFrame(p_seq, p_message, p_out);
}

enum class DecodeStatus : std::uint8_t
{
  Complete = 0, // message_ is set; consume frame_size_ bytes
  NeedMore,     // the buffer holds only part of a frame
  Malformed,    // unknown type, wrong length, or a bad enum value: close the connection
};

template <typename Variant> struct Decoded
{
  DecodeStatus status_ = DecodeStatus::NeedMore;
  std::size_t frame_size_ = 0;
  std::uint64_t seq_ = 0; // exchange -> client frames only
  Variant message_{};
};

namespace detail
{

template <typename Variant, typename M> Decoded<Variant> DecodeBody(std::span<const std::byte> p_in)
{
  Decoded<Variant> result{};
  Reader reader{p_in.subspan(kHeaderSize)};
  if constexpr (kFromExchange<M>)
  {
    reader.Get(result.seq_);
  }
  M message{};
  message.Fields([&reader](auto &...p_field) { (reader.Get(p_field), ...); });
  if (!reader.Valid())
  {
    result.status_ = DecodeStatus::Malformed;
    return result;
  }
  result.status_ = DecodeStatus::Complete;
  result.frame_size_ = kFrameSize<M>;
  result.message_ = message;
  return result;
}

template <typename Variant, std::size_t... I>
Decoded<Variant> DecodeFrame(std::span<const std::byte> p_in, std::index_sequence<I...>)
{
  if (p_in.size() < kHeaderSize)
  {
    return {.status_ = DecodeStatus::NeedMore};
  }
  std::uint16_t length = 0;
  MsgType type{};
  std::memcpy(&length, p_in.data(), sizeof(length));
  std::memcpy(&type, p_in.subspan(sizeof(length)).data(), sizeof(type));

  Decoded<Variant> result{.status_ = DecodeStatus::Malformed};
  // Check the header before the body arrives, so a bad header fails at once.
  const auto try_alternative = [&]<typename M>()
  {
    if (type != M::kType)
    {
      return false;
    }
    if (length != kFrameSize<M>)
    {
      result = {.status_ = DecodeStatus::Malformed};
    }
    else if (p_in.size() < kFrameSize<M>)
    {
      result = {.status_ = DecodeStatus::NeedMore};
    }
    else
    {
      result = DecodeBody<Variant, M>(p_in);
    }
    return true;
  };
  (try_alternative.template operator()<std::variant_alternative_t<I, Variant>>() || ...);
  return result;
}

} // namespace detail

// Decodes the first frame in p_in, sent by a client.
[[nodiscard]] inline Decoded<ClientMessage> DecodeClientFrame(std::span<const std::byte> p_in)
{
  return detail::DecodeFrame<ClientMessage>(
      p_in, std::make_index_sequence<std::variant_size_v<ClientMessage>>{});
}

// Decodes the first frame in p_in, sent by the exchange.
[[nodiscard]] inline Decoded<ExchangeMessage> DecodeExchangeFrame(std::span<const std::byte> p_in)
{
  return detail::DecodeFrame<ExchangeMessage>(
      p_in, std::make_index_sequence<std::variant_size_v<ExchangeMessage>>{});
}

} // namespace mex::wire
