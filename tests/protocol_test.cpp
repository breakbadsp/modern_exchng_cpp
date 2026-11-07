#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>
#include <variant>
#include <vector>

namespace
{

using namespace mex::wire;
using mex::Price;
using mex::Quantity;
using mex::Side;

using Buffer = std::array<std::byte, kMaxFrameSize>;

std::vector<std::byte> Bytes(std::initializer_list<int> p_values)
{
  std::vector<std::byte> bytes;
  for (const int value : p_values)
  {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

Name NameOf(std::string_view p_text)
{
  Name name{};
  p_text.copy(name.data(), name.size());
  return name;
}

TEST(ProtocolTest, NewOrderHasTheDocumentedByteLayout)
{
  const NewOrder order{.client_order_id_ = 0x0102030405060708,
                       .symbol_id_ = 0x0A0B0C0D,
                       .side_ = Side::Sell,
                       .type_ = OrderType::Limit,
                       .price_ = Price{-2},
                       .qty_ = Quantity{0x11223344}};
  Buffer buffer{};

  const std::size_t size = Encode(order, buffer);

  const std::vector<std::byte> expected =
      Bytes({0x1D, 0x00, 0x02,                               // length 29, type NewOrder
             0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // client_order_id
             0x0D, 0x0C, 0x0B, 0x0A,                         // symbol_id
             0x01,                                           // side Sell
             0x00,                                           // type Limit
             0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // price -2
             0x44, 0x33, 0x22, 0x11});                       // qty
  ASSERT_EQ(size, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), buffer.begin()));
}

TEST(ProtocolTest, ExchangeFrameCarriesSeqAfterTheHeader)
{
  Buffer buffer{};

  const std::size_t size = Encode(5, LogonAck{}, buffer);

  const std::vector<std::byte> expected =
      Bytes({0x0B, 0x00, 0x40, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  ASSERT_EQ(size, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), buffer.begin()));
}

TEST(ProtocolTest, EveryClientMessageRoundTrips)
{
  const std::vector<ClientMessage> messages = {
      Logon{.session_name_ = NameOf("alice"), .token_ = NameOf("0123456789abcdef")},
      NewOrder{.client_order_id_ = 7,
               .symbol_id_ = 3,
               .side_ = Side::Buy,
               .type_ = OrderType::Market,
               .price_ = Price{0},
               .qty_ = Quantity{100}},
      Cancel{.client_order_id_ = 8, .symbol_id_ = 4},
      Replace{.client_order_id_ = 9,
              .symbol_id_ = 5,
              .new_price_ = Price{12345},
              .new_total_qty_ = Quantity{50}},
  };
  for (const ClientMessage &message : messages)
  {
    Buffer buffer{};
    const std::size_t size =
        std::visit([&buffer](const auto &p_message) { return Encode(p_message, buffer); }, message);

    const Decoded<ClientMessage> decoded = DecodeClientFrame(std::span{buffer}.first(size));

    ASSERT_EQ(decoded.status_, DecodeStatus::Complete) << "index " << message.index();
    EXPECT_EQ(decoded.frame_size_, size);
    EXPECT_EQ(decoded.message_, message);
  }
}

TEST(ProtocolTest, EveryExchangeMessageRoundTripsWithItsSeq)
{
  const std::vector<ExchangeMessage> messages = {
      LogonAck{},
      LogonReject{.reason_ = Reason::AlreadyLoggedOn},
      Ack{.client_order_id_ = 1,
          .symbol_id_ = 2,
          .exchange_order_id_ = 0xFFFF'0000'0000'0001,
          .leaves_ = Quantity{10}},
      Reject{.client_order_id_ = 2, .symbol_id_ = 3, .reason_ = Reason::SystemBusy},
      Fill{.exchange_order_id_ = 3,
           .client_order_id_ = 4,
           .symbol_id_ = 5,
           .price_ = Price{-7},
           .qty_ = Quantity{6},
           .leaves_ = Quantity{1},
           .liquidity_ = Liquidity::Taker},
      CancelAck{.client_order_id_ = 5, .symbol_id_ = 6},
      CancelReject{.client_order_id_ = 6, .symbol_id_ = 7, .reason_ = Reason::UnknownOrder},
      ReplaceAck{
          .client_order_id_ = 7, .symbol_id_ = 8, .price_ = Price{9}, .leaves_ = Quantity{2}},
      ReplaceReject{.client_order_id_ = 8, .symbol_id_ = 9, .reason_ = Reason::TooLate},
      Done{.client_order_id_ = 9, .symbol_id_ = 10, .reason_ = Reason::MarketRemainder},
  };
  std::uint64_t seq = 1;
  for (const ExchangeMessage &message : messages)
  {
    Buffer buffer{};
    const std::size_t size = std::visit([&buffer, seq](const auto &p_message)
                                        { return Encode(seq, p_message, buffer); }, message);

    const Decoded<ExchangeMessage> decoded = DecodeExchangeFrame(std::span{buffer}.first(size));

    ASSERT_EQ(decoded.status_, DecodeStatus::Complete) << "index " << message.index();
    EXPECT_EQ(decoded.frame_size_, size);
    EXPECT_EQ(decoded.seq_, seq);
    EXPECT_EQ(decoded.message_, message);
    ++seq;
  }
}

TEST(ProtocolTest, PartialFrameNeedsMoreUntilTheLastByte)
{
  Buffer buffer{};
  const std::size_t size = Encode(Cancel{.client_order_id_ = 1, .symbol_id_ = 2}, buffer);

  for (std::size_t available = 0; available < size; ++available)
  {
    EXPECT_EQ(DecodeClientFrame(std::span{buffer}.first(available)).status_, DecodeStatus::NeedMore)
        << "available " << available;
  }
  EXPECT_EQ(DecodeClientFrame(std::span{buffer}.first(size)).status_, DecodeStatus::Complete);
}

TEST(ProtocolTest, DecodesBackToBackFramesOneAtATime)
{
  std::array<std::byte, 2 * kMaxFrameSize> buffer{};
  const Cancel first{.client_order_id_ = 1, .symbol_id_ = 2};
  const Replace second{.client_order_id_ = 3,
                       .symbol_id_ = 4,
                       .new_price_ = Price{5},
                       .new_total_qty_ = Quantity{6}};
  const std::size_t first_size = Encode(first, buffer);
  const std::size_t second_size = Encode(second, std::span{buffer}.subspan(first_size));
  const std::span<const std::byte> bytes = std::span{buffer}.first(first_size + second_size);

  const Decoded<ClientMessage> one = DecodeClientFrame(bytes);
  ASSERT_EQ(one.status_, DecodeStatus::Complete);
  ASSERT_EQ(one.frame_size_, first_size);
  const Decoded<ClientMessage> two = DecodeClientFrame(bytes.subspan(one.frame_size_));

  EXPECT_EQ(one.message_, ClientMessage{first});
  ASSERT_EQ(two.status_, DecodeStatus::Complete);
  EXPECT_EQ(two.message_, ClientMessage{second});
}

TEST(ProtocolTest, WrongLengthIsMalformedFromTheHeaderAlone)
{
  Buffer buffer{};
  ASSERT_EQ(Encode(Cancel{}, buffer), 15U);
  buffer[0] = std::byte{0x0E}; // Cancel is 15 bytes, not 14

  EXPECT_EQ(DecodeClientFrame(std::span{buffer}.first(kHeaderSize)).status_,
            DecodeStatus::Malformed);
}

TEST(ProtocolTest, UnknownTypeIsMalformed)
{
  const std::vector<std::byte> unknown = Bytes({0x03, 0x00, 0x00});

  EXPECT_EQ(DecodeClientFrame(unknown).status_, DecodeStatus::Malformed);
}

TEST(ProtocolTest, ExchangeFrameSentByAClientIsMalformed)
{
  Buffer buffer{};
  const std::size_t size = Encode(1, LogonAck{}, buffer);

  EXPECT_EQ(DecodeClientFrame(std::span{buffer}.first(size)).status_, DecodeStatus::Malformed);
}

TEST(ProtocolTest, OutOfRangeEnumFieldIsMalformed)
{
  Buffer buffer{};
  const std::size_t size = Encode(NewOrder{.qty_ = Quantity{1}}, buffer);
  constexpr std::size_t kSideOffset = kHeaderSize + 8 + 4;
  constexpr std::size_t kTypeOffset = kSideOffset + 1;

  Buffer bad_side = buffer;
  bad_side[kSideOffset] = std::byte{2};
  Buffer bad_type = buffer;
  bad_type[kTypeOffset] = std::byte{2};

  EXPECT_EQ(DecodeClientFrame(std::span{bad_side}.first(size)).status_, DecodeStatus::Malformed);
  EXPECT_EQ(DecodeClientFrame(std::span{bad_type}.first(size)).status_, DecodeStatus::Malformed);
}

TEST(ProtocolTest, OutOfRangeReasonIsMalformed)
{
  Buffer buffer{};
  const std::size_t size = Encode(1, LogonReject{}, buffer);
  buffer[size - 1] = std::byte{0xFF};

  EXPECT_EQ(DecodeExchangeFrame(std::span{buffer}.first(size)).status_, DecodeStatus::Malformed);
}

} // namespace
