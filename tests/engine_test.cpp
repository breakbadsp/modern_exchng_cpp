#include "engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mex
{

// Readable failure output: "session 2 type 68: 7 9 ..." (fields in wire order).
void PrintTo(const Report &p_report, std::ostream *p_os)
{
  *p_os << "{session " << p_report.session_id_;
  std::visit(
      [p_os](const auto &p_message)
      {
        *p_os << " type " << static_cast<int>(p_message.kType) << ":";
        p_message.Fields(
            [p_os](const auto &...p_field)
            {
              const auto print = [p_os]<typename F>(const F &p_value)
              {
                if constexpr (std::is_same_v<F, Price>)
                {
                  *p_os << ' ' << p_value.ticks_;
                }
                else if constexpr (std::is_same_v<F, Quantity>)
                {
                  *p_os << ' ' << p_value.units_;
                }
                else if constexpr (std::is_enum_v<F>)
                {
                  *p_os << ' ' << static_cast<int>(std::to_underlying(p_value));
                }
                else
                {
                  *p_os << ' ' << p_value;
                }
              };
              (print(p_field), ...);
            });
      },
      p_report.report_);
  *p_os << '}';
}

} // namespace mex

namespace
{

using mex::BookConfig;
using mex::ClientOrderIndex;
using mex::Command;
using mex::Engine;
using mex::OpNumber;
using mex::OrderId;
using mex::Price;
using mex::Quantity;
using mex::Report;
using mex::SessionId;
using mex::Side;
using mex::SpscQueue;
using mex::wire::ClientOrderId;
using mex::wire::ExchangeOrderId;
using mex::wire::Liquidity;
using mex::wire::Reason;
using mex::wire::RequestNumber;

constexpr SessionId kA = 1;
constexpr SessionId kB = 2;
constexpr SessionId kC = 3;
constexpr std::size_t kSessionCount = 4;
constexpr std::array<mex::wire::SymbolId, 2> kSymbols{7, 8};
constexpr std::size_t kRing = 1024;

// ---------------------------------------------------------------------------
// ClientOrderIndex

// Keys of owner kA whose probe starts at p_home, in increasing id order.
std::vector<ClientOrderId> KeysWithHome(const ClientOrderIndex &p_index, std::size_t p_home,
                                        std::size_t p_count)
{
  std::vector<ClientOrderId> keys;
  for (ClientOrderId id = 0; keys.size() < p_count; ++id)
  {
    if (p_index.HomeSlot(kA, id) == p_home)
    {
      keys.push_back(id);
    }
  }
  return keys;
}

TEST(ClientOrderIndexTest, CapacityIsAPowerOfTwoAtLeastTwiceMaxOrders)
{
  EXPECT_EQ(ClientOrderIndex{4}.Capacity(), 8U);
  EXPECT_EQ(ClientOrderIndex{5}.Capacity(), 16U);
  EXPECT_EQ(ClientOrderIndex{1}.Capacity(), 2U);
}

TEST(ClientOrderIndexTest, FindsByOwnerAndClientOrderId)
{
  ClientOrderIndex index{4};

  index.Insert(kA, 10, 3);

  EXPECT_EQ(index.Find(kA, 10), std::optional<OrderId>{3});
  EXPECT_FALSE(index.Find(kB, 10).has_value());
  EXPECT_FALSE(index.Find(kA, 11).has_value());
  EXPECT_EQ(index.Size(), 1U);
}

TEST(ClientOrderIndexTest, EraseRemovesOnlyThatKey)
{
  ClientOrderIndex index{4};
  index.Insert(kA, 10, 0);
  index.Insert(kB, 10, 1);
  index.Insert(kA, 11, 2);

  index.Erase(kA, 10);

  EXPECT_FALSE(index.Find(kA, 10).has_value());
  EXPECT_EQ(index.Find(kB, 10), std::optional<OrderId>{1});
  EXPECT_EQ(index.Find(kA, 11), std::optional<OrderId>{2});
  EXPECT_EQ(index.Size(), 2U);
}

TEST(ClientOrderIndexTest, EraseShiftsAWrappedRunBack)
{
  ClientOrderIndex index{4};
  const std::size_t last = index.Capacity() - 1;
  // Three keys that all start at the last entry wrap to entries 0 and 1.
  // A fourth key that starts at 0 is pushed to entry 2.
  const std::vector<ClientOrderId> at_last = KeysWithHome(index, last, 3);
  const ClientOrderId at_zero = KeysWithHome(index, 0, 1)[0];
  index.Insert(kA, at_last[0], 0);
  index.Insert(kA, at_last[1], 1);
  index.Insert(kA, at_last[2], 2);
  index.Insert(kA, at_zero, 3);

  index.Erase(kA, at_last[0]);

  EXPECT_FALSE(index.Find(kA, at_last[0]).has_value());
  EXPECT_EQ(index.Find(kA, at_last[1]), std::optional<OrderId>{1});
  EXPECT_EQ(index.Find(kA, at_last[2]), std::optional<OrderId>{2});
  EXPECT_EQ(index.Find(kA, at_zero), std::optional<OrderId>{3});

  // The freed space is usable: a new key at the last entry and a reinsert.
  index.Insert(kA, at_last[0], 4);
  EXPECT_EQ(index.Find(kA, at_last[0]), std::optional<OrderId>{4});
  EXPECT_EQ(index.Find(kA, at_zero), std::optional<OrderId>{3});
}

TEST(ClientOrderIndexTest, EraseKeepsAnEntryThatIsAtItsHome)
{
  ClientOrderIndex index{4};
  const std::size_t last = index.Capacity() - 1;
  const ClientOrderId at_last = KeysWithHome(index, last, 1)[0];
  const ClientOrderId at_zero = KeysWithHome(index, 0, 1)[0];
  index.Insert(kA, at_last, 0);
  index.Insert(kA, at_zero, 1);

  index.Erase(kA, at_last);

  EXPECT_EQ(index.Find(kA, at_zero), std::optional<OrderId>{1});
  const ClientOrderId other_at_last = KeysWithHome(index, last, 2)[1];
  index.Insert(kA, other_at_last, 2);
  EXPECT_EQ(index.Find(kA, other_at_last), std::optional<OrderId>{2});
  EXPECT_EQ(index.Find(kA, at_zero), std::optional<OrderId>{1});
}

TEST(ClientOrderIndexTest, MatchesAMapOverManyRandomInsertsAndErases)
{
  constexpr std::uint32_t kMaxOrders = 4;
  ClientOrderIndex index{kMaxOrders}; // 8 entries: runs collide and wrap often
  std::map<std::pair<SessionId, ClientOrderId>, OrderId> shadow;
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  const auto next = [&state]()
  {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
  };

  for (OrderId op = 0; op < 20000; ++op)
  {
    const auto owner = static_cast<SessionId>(1 + next() % 2U);
    const ClientOrderId id = next() % 6U;
    const auto it = shadow.find({owner, id});
    if (it != shadow.end())
    {
      index.Erase(owner, id);
      shadow.erase(it);
    }
    else if (shadow.size() < kMaxOrders)
    {
      index.Insert(owner, id, op);
      shadow[{owner, id}] = op;
    }
    for (SessionId o = 1; o <= 2; ++o)
    {
      for (ClientOrderId k = 0; k < 6; ++k)
      {
        const auto found = shadow.find({o, k});
        const std::optional<OrderId> expected =
            found == shadow.end() ? std::nullopt : std::optional<OrderId>{found->second};
        ASSERT_EQ(index.Find(o, k), expected) << "op " << op << " owner " << o << " id " << k;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Engine driven through its queues

Command Limit(SessionId p_session, ClientOrderId p_id, Side p_side, std::int64_t p_price,
              std::uint32_t p_qty, std::uint32_t p_slot = 0)
{
  return {.session_id_ = p_session,
          .symbol_slot_ = p_slot,
          .order_ = mex::wire::NewOrder{.client_order_id_ = p_id,
                                        .symbol_id_ = kSymbols[p_slot],
                                        .side_ = p_side,
                                        .type_ = mex::wire::OrderType::Limit,
                                        .price_ = Price{p_price},
                                        .qty_ = Quantity{p_qty}}};
}

Command Market(SessionId p_session, ClientOrderId p_id, Side p_side, std::uint32_t p_qty)
{
  return {.session_id_ = p_session,
          .symbol_slot_ = 0,
          .order_ = mex::wire::NewOrder{.client_order_id_ = p_id,
                                        .symbol_id_ = kSymbols[0],
                                        .side_ = p_side,
                                        .type_ = mex::wire::OrderType::Market,
                                        .qty_ = Quantity{p_qty}}};
}

Command CancelOf(SessionId p_session, ClientOrderId p_id)
{
  return {.session_id_ = p_session,
          .symbol_slot_ = 0,
          .order_ = mex::wire::Cancel{.client_order_id_ = p_id, .symbol_id_ = kSymbols[0]}};
}

Command ReplaceOf(SessionId p_session, ClientOrderId p_id, std::int64_t p_price,
                  std::uint32_t p_total)
{
  return {.session_id_ = p_session,
          .symbol_slot_ = 0,
          .order_ = mex::wire::Replace{.client_order_id_ = p_id,
                                       .symbol_id_ = kSymbols[0],
                                       .new_price_ = Price{p_price},
                                       .new_total_qty_ = Quantity{p_total}}};
}

Report AckR(SessionId p_session, ClientOrderId p_id, ExchangeOrderId p_exchange_id,
            std::uint32_t p_leaves, std::uint32_t p_slot = 0)
{
  return {p_session, mex::wire::Ack{.client_order_id_ = p_id,
                                    .symbol_id_ = kSymbols[p_slot],
                                    .exchange_order_id_ = p_exchange_id,
                                    .leaves_ = Quantity{p_leaves}}};
}

Report RejectR(SessionId p_session, ClientOrderId p_id, Reason p_reason)
{
  return {p_session, mex::wire::Reject{
                         .client_order_id_ = p_id, .symbol_id_ = kSymbols[0], .reason_ = p_reason}};
}

Report FillR(SessionId p_session, ExchangeOrderId p_exchange_id, ClientOrderId p_id,
             std::int64_t p_price, std::uint32_t p_qty, std::uint32_t p_leaves,
             Liquidity p_liquidity)
{
  return {p_session, mex::wire::Fill{.exchange_order_id_ = p_exchange_id,
                                     .client_order_id_ = p_id,
                                     .symbol_id_ = kSymbols[0],
                                     .price_ = Price{p_price},
                                     .qty_ = Quantity{p_qty},
                                     .leaves_ = Quantity{p_leaves},
                                     .liquidity_ = p_liquidity}};
}

Report CancelAckR(SessionId p_session, ClientOrderId p_id)
{
  return {p_session, mex::wire::CancelAck{.client_order_id_ = p_id, .symbol_id_ = kSymbols[0]}};
}

Report CancelRejectR(SessionId p_session, ClientOrderId p_id,
                     Reason p_reason = Reason::UnknownOrder)
{
  return {p_session, mex::wire::CancelReject{
                         .client_order_id_ = p_id, .symbol_id_ = kSymbols[0], .reason_ = p_reason}};
}

Report ReplaceAckR(SessionId p_session, ClientOrderId p_id, std::int64_t p_price,
                   std::uint32_t p_leaves)
{
  return {p_session, mex::wire::ReplaceAck{.client_order_id_ = p_id,
                                           .symbol_id_ = kSymbols[0],
                                           .price_ = Price{p_price},
                                           .leaves_ = Quantity{p_leaves}}};
}

Report ReplaceRejectR(SessionId p_session, ClientOrderId p_id, Reason p_reason)
{
  return {p_session, mex::wire::ReplaceReject{
                         .client_order_id_ = p_id, .symbol_id_ = kSymbols[0], .reason_ = p_reason}};
}

Report DoneR(SessionId p_session, ClientOrderId p_id, Reason p_reason)
{
  return {p_session, mex::wire::Done{
                         .client_order_id_ = p_id, .symbol_id_ = kSymbols[0], .reason_ = p_reason}};
}

// p_command with its place in the total order and its request number.
Command Numbered(const Command &p_command, OpNumber p_op, RequestNumber p_request)
{
  Command command = p_command;
  command.op_number_ = p_op;
  std::visit([p_request](auto &p_order) { p_order.request_number_ = p_request; }, command.order_);
  return command;
}

using Reports = std::vector<Report>;

OrderId SlotOf(ExchangeOrderId p_id) { return static_cast<OrderId>(p_id & 0xFFFFFFFFU); }

// The gateway side of one engine, on the test thread.
class Harness
{
public:
  explicit Harness(std::uint32_t p_max_orders = 16, std::uint32_t p_max_price_levels = 16,
                   std::size_t p_outbound_capacity = kRing)
      : books_{BookConfig{kSymbols[0], p_max_orders, p_max_price_levels},
               BookConfig{kSymbols[1], p_max_orders, p_max_price_levels}},
        outbound_{p_outbound_capacity}, engine_{books_, kSessionCount, inbound_, outbound_}
  {
  }

  // p_command with the next op number and a request number above every earlier one.
  Command Next(const Command &p_command) { return NextAs(last_request_ + 1, p_command); }

  // p_command with the next op number and request number p_request.
  Command NextAs(std::uint64_t p_request, const Command &p_command)
  {
    last_request_ = std::max(last_request_, p_request);
    ++last_op_;
    return Numbered(p_command, OpNumber{last_op_}, RequestNumber{p_request});
  }

  // Pushes one command, lets the engine handle it, and returns every report.
  Reports Send(const Command &p_command) { return SendNumbered(Next(p_command)); }

  // Send with request number p_request: a retry, or a number out of order.
  Reports SendAs(std::uint64_t p_request, const Command &p_command)
  {
    return SendNumbered(NextAs(p_request, p_command));
  }

  Reports SendNumbered(const Command &p_command)
  {
    EXPECT_TRUE(inbound_.TryPush(p_command));
    EXPECT_TRUE(engine_.ProcessOne());
    return Drain();
  }

  // Applies one command without the inbound queue, as a replicated log would.
  Reports ApplyNumbered(const Command &p_command)
  {
    engine_.Apply(p_command);
    return Drain();
  }

  Reports Drain()
  {
    Reports reports;
    while (const std::optional<Report> report = outbound_.TryPop())
    {
      reports.push_back(*report);
    }
    return reports;
  }

  // Sends an order that must rest whole; returns its exchange_order_id.
  ExchangeOrderId Rest(const Command &p_command)
  {
    const Reports reports = Send(p_command);
    const auto *ack =
        reports.size() == 1 ? std::get_if<mex::wire::Ack>(&reports[0].report_) : nullptr;
    EXPECT_NE(ack, nullptr) << "the order did not rest";
    return ack == nullptr ? mex::kNoExchangeOrderId : ack->exchange_order_id_;
  }

  SpscQueue<Command> &Inbound() { return inbound_; }
  SpscQueue<Report> &Outbound() { return outbound_; }
  Engine &EngineRef() { return engine_; }

private:
  std::array<BookConfig, 2> books_;
  SpscQueue<Command> inbound_{kRing};
  SpscQueue<Report> outbound_;
  Engine engine_;
  std::uint64_t last_op_ = 0;
  std::uint64_t last_request_ = 0;
};

TEST(EngineTest, EmptyInboundProcessesNothing)
{
  Harness h;

  EXPECT_FALSE(h.EngineRef().ProcessOne());
}

TEST(EngineTest, RestingOrderIsAckedWithItsLeaves)
{
  Harness h;

  const Reports reports = h.Send(Limit(kA, 1, Side::Buy, 100, 10));

  ASSERT_EQ(reports.size(), 1U);
  const auto &ack = std::get<mex::wire::Ack>(reports[0].report_);
  EXPECT_EQ(reports[0], AckR(kA, 1, ack.exchange_order_id_, 10));
  EXPECT_NE(ack.exchange_order_id_, mex::kNoExchangeOrderId);
}

TEST(EngineTest, ZeroQuantityIsRejected)
{
  Harness h;

  EXPECT_EQ(h.Send(Limit(kA, 1, Side::Buy, 100, 0)),
            (Reports{RejectR(kA, 1, Reason::InvalidQuantity)}));
}

TEST(EngineTest, DuplicateLiveIdIsRejectedAndTheBookIsUnchanged)
{
  Harness h;
  const ExchangeOrderId first = h.Rest(Limit(kA, 1, Side::Sell, 100, 10));

  EXPECT_EQ(h.Send(Limit(kA, 1, Side::Sell, 99, 5)),
            (Reports{RejectR(kA, 1, Reason::DuplicateClientOrderId)}));

  // Only the first order is in the book.
  EXPECT_EQ(h.Send(Market(kB, 9, Side::Buy, 20)),
            (Reports{FillR(kA, first, 1, 100, 10, 0, Liquidity::Maker),
                     FillR(kB, mex::kNoExchangeOrderId, 9, 100, 10, 10, Liquidity::Taker),
                     DoneR(kB, 9, Reason::MarketRemainder)}));
}

TEST(EngineTest, SameIdIsFreeForAnotherSessionOrAnotherSymbol)
{
  Harness h;
  h.Rest(Limit(kA, 1, Side::Buy, 100, 10));

  h.Rest(Limit(kB, 1, Side::Buy, 100, 10));
  h.Rest(Limit(kA, 1, Side::Buy, 100, 10, 1));
}

TEST(EngineTest, IdIsReusableAfterFullFillAndAfterCancel)
{
  Harness h;
  h.Rest(Limit(kA, 1, Side::Sell, 100, 5));
  static_cast<void>(h.Send(Market(kB, 9, Side::Buy, 5)));

  h.Rest(Limit(kA, 1, Side::Sell, 100, 5));
  EXPECT_EQ(h.Send(CancelOf(kA, 1)), (Reports{CancelAckR(kA, 1)}));

  h.Rest(Limit(kA, 1, Side::Sell, 100, 5));
}

TEST(EngineTest, IdIsStillLiveAfterAPartialMakerFill)
{
  Harness h;
  const ExchangeOrderId maker = h.Rest(Limit(kA, 1, Side::Sell, 100, 10));

  EXPECT_EQ(h.Send(Market(kB, 9, Side::Buy, 4)),
            (Reports{FillR(kA, maker, 1, 100, 4, 6, Liquidity::Maker),
                     FillR(kB, mex::kNoExchangeOrderId, 9, 100, 4, 0, Liquidity::Taker),
                     AckR(kB, 9, mex::kNoExchangeOrderId, 0)}));

  EXPECT_EQ(h.Send(Limit(kA, 1, Side::Sell, 100, 1)),
            (Reports{RejectR(kA, 1, Reason::DuplicateClientOrderId)}));
  EXPECT_EQ(h.Send(CancelOf(kA, 1)), (Reports{CancelAckR(kA, 1)}));
}

TEST(EngineTest, AnotherSessionCannotCancelOrReplaceAnOrderWithTheSameId)
{
  Harness h;
  h.Rest(Limit(kA, 1, Side::Buy, 100, 10));

  EXPECT_EQ(h.Send(CancelOf(kB, 1)), (Reports{CancelRejectR(kB, 1)}));
  EXPECT_EQ(h.Send(ReplaceOf(kB, 1, 101, 5)),
            (Reports{ReplaceRejectR(kB, 1, Reason::UnknownOrder)}));

  // B's own order with that id is a different order.
  h.Rest(Limit(kB, 1, Side::Buy, 99, 3));
  EXPECT_EQ(h.Send(CancelOf(kB, 1)), (Reports{CancelAckR(kB, 1)}));
  EXPECT_EQ(h.Send(CancelOf(kA, 1)), (Reports{CancelAckR(kA, 1)}));
}

TEST(EngineTest, UnknownOrderIsRejectedForCancelAndReplace)
{
  Harness h;

  EXPECT_EQ(h.Send(CancelOf(kA, 5)), (Reports{CancelRejectR(kA, 5)}));
  EXPECT_EQ(h.Send(ReplaceOf(kA, 5, 100, 10)),
            (Reports{ReplaceRejectR(kA, 5, Reason::UnknownOrder)}));
}

// A taker fully fills a maker and rests in the slot the maker freed, in one
// submit. The maker's report and index erase must happen before the claim.
TEST(EngineTest, MakerIsSettledBeforeTheTakerClaimsItsFreedSlot)
{
  Harness h{2}; // the taker rests in the slot the maker freed (LIFO free list)
  const ExchangeOrderId maker = h.Rest(Limit(kA, 1, Side::Sell, 100, 10));

  const Reports reports = h.Send(Limit(kB, 7, Side::Buy, 100, 15));

  ASSERT_EQ(reports.size(), 3U);
  const auto &ack = std::get<mex::wire::Ack>(reports[2].report_);
  EXPECT_EQ(SlotOf(ack.exchange_order_id_), SlotOf(maker)) << "the taker reused the slot";
  EXPECT_NE(ack.exchange_order_id_, maker) << "a new generation";
  EXPECT_EQ(reports, (Reports{FillR(kA, maker, 1, 100, 10, 0, Liquidity::Maker),
                              FillR(kB, ack.exchange_order_id_, 7, 100, 10, 5, Liquidity::Taker),
                              AckR(kB, 7, ack.exchange_order_id_, 5)}));

  // A's id left the index; B's id is in it.
  EXPECT_EQ(h.Send(CancelOf(kA, 1)), (Reports{CancelRejectR(kA, 1)}));
  EXPECT_EQ(h.Send(CancelOf(kB, 7)), (Reports{CancelAckR(kB, 7)}));
}

TEST(EngineTest, ReplaceAtOrBelowFilledQuantityIsTooLate)
{
  Harness h;
  h.Rest(Limit(kA, 1, Side::Buy, 100, 10));
  static_cast<void>(h.Send(Market(kB, 9, Side::Sell, 4))); // cum 4, leaves 6

  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 100, 4)), (Reports{ReplaceRejectR(kA, 1, Reason::TooLate)}));
  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 101, 3)), (Reports{ReplaceRejectR(kA, 1, Reason::TooLate)}));

  // Total 5 leaves 1, counting the 4 already filled.
  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 100, 5)), (Reports{ReplaceAckR(kA, 1, 100, 1)}));
}

TEST(EngineTest, ReduceInPlaceKeepsTimePriority)
{
  Harness h;
  const ExchangeOrderId first = h.Rest(Limit(kA, 1, Side::Buy, 100, 10));
  const ExchangeOrderId second = h.Rest(Limit(kB, 2, Side::Buy, 100, 10));

  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 100, 4)), (Reports{ReplaceAckR(kA, 1, 100, 4)}));

  EXPECT_EQ(h.Send(Market(kC, 9, Side::Sell, 6)),
            (Reports{FillR(kA, first, 1, 100, 4, 0, Liquidity::Maker),
                     FillR(kB, second, 2, 100, 2, 8, Liquidity::Maker),
                     FillR(kC, mex::kNoExchangeOrderId, 9, 100, 4, 2, Liquidity::Taker),
                     FillR(kC, mex::kNoExchangeOrderId, 9, 100, 2, 0, Liquidity::Taker),
                     AckR(kC, 9, mex::kNoExchangeOrderId, 0)}));
}

TEST(EngineTest, IncreaseLosesTimePriorityAndGetsANewExchangeOrderId)
{
  Harness h;
  const ExchangeOrderId first = h.Rest(Limit(kA, 1, Side::Buy, 100, 10));
  const ExchangeOrderId second = h.Rest(Limit(kB, 2, Side::Buy, 100, 10));

  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 100, 15)), (Reports{ReplaceAckR(kA, 1, 100, 15)}));

  const Reports reports = h.Send(Market(kC, 9, Side::Sell, 11));
  ASSERT_EQ(reports.size(), 5U);
  const ExchangeOrderId replaced = std::get<mex::wire::Fill>(reports[1].report_).exchange_order_id_;
  EXPECT_NE(replaced, first);
  EXPECT_EQ(reports, (Reports{FillR(kB, second, 2, 100, 10, 0, Liquidity::Maker),
                              FillR(kA, replaced, 1, 100, 1, 14, Liquidity::Maker),
                              FillR(kC, mex::kNoExchangeOrderId, 9, 100, 10, 1, Liquidity::Taker),
                              FillR(kC, mex::kNoExchangeOrderId, 9, 100, 1, 0, Liquidity::Taker),
                              AckR(kC, 9, mex::kNoExchangeOrderId, 0)}));
}

TEST(EngineTest, PriceChangeCanMatchAndRestsTheRemainder)
{
  Harness h;
  const ExchangeOrderId seller = h.Rest(Limit(kB, 5, Side::Sell, 101, 3));
  const ExchangeOrderId original = h.Rest(Limit(kA, 1, Side::Buy, 100, 10));

  const Reports reports = h.Send(ReplaceOf(kA, 1, 101, 10));

  ASSERT_EQ(reports.size(), 3U);
  const ExchangeOrderId replaced = std::get<mex::wire::Fill>(reports[1].report_).exchange_order_id_;
  EXPECT_NE(replaced, original);
  EXPECT_NE(replaced, mex::kNoExchangeOrderId);
  EXPECT_EQ(reports, (Reports{FillR(kB, seller, 5, 101, 3, 0, Liquidity::Maker),
                              FillR(kA, replaced, 1, 101, 3, 7, Liquidity::Taker),
                              ReplaceAckR(kA, 1, 101, 7)}));

  // The filled 3 count toward the total: total 3 is too late, total 5 leaves 2.
  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 101, 3)), (Reports{ReplaceRejectR(kA, 1, Reason::TooLate)}));
  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 101, 5)), (Reports{ReplaceAckR(kA, 1, 101, 2)}));
  EXPECT_EQ(h.Send(Market(kC, 9, Side::Sell, 2)),
            (Reports{FillR(kA, replaced, 1, 101, 2, 0, Liquidity::Maker),
                     FillR(kC, mex::kNoExchangeOrderId, 9, 101, 2, 0, Liquidity::Taker),
                     AckR(kC, 9, mex::kNoExchangeOrderId, 0)}));
}

TEST(EngineTest, ReplaceThatFullyFillsEndsTheOrder)
{
  Harness h;
  const ExchangeOrderId seller = h.Rest(Limit(kB, 5, Side::Sell, 101, 10));
  const ExchangeOrderId original = h.Rest(Limit(kA, 1, Side::Buy, 100, 4));

  EXPECT_EQ(
      h.Send(ReplaceOf(kA, 1, 101, 4)),
      (Reports{FillR(kB, seller, 5, 101, 4, 6, Liquidity::Maker),
               FillR(kA, original, 1, 101, 4, 0, Liquidity::Taker), ReplaceAckR(kA, 1, 101, 0)}));

  EXPECT_EQ(h.Send(CancelOf(kA, 1)), (Reports{CancelRejectR(kA, 1)}));
}

TEST(EngineTest, ReplaceThatNeedsALevelOnAFullLadderIsRejectedAndChangesNothing)
{
  Harness h{16, 1};
  const ExchangeOrderId first = h.Rest(Limit(kA, 1, Side::Buy, 100, 10));
  h.Rest(Limit(kA, 2, Side::Buy, 100, 10));

  EXPECT_EQ(h.Send(ReplaceOf(kA, 1, 99, 10)), (Reports{ReplaceRejectR(kA, 1, Reason::BookFull)}));

  EXPECT_EQ(h.Send(Market(kB, 9, Side::Sell, 1)),
            (Reports{FillR(kA, first, 1, 100, 1, 9, Liquidity::Maker),
                     FillR(kB, mex::kNoExchangeOrderId, 9, 100, 1, 0, Liquidity::Taker),
                     AckR(kB, 9, mex::kNoExchangeOrderId, 0)}));
}

TEST(EngineTest, PartialFillThatCannotRestIsDone)
{
  Harness h{16, 1};
  h.Rest(Limit(kC, 3, Side::Buy, 90, 1)); // the only bid level
  const ExchangeOrderId maker = h.Rest(Limit(kA, 1, Side::Sell, 100, 5));

  EXPECT_EQ(h.Send(Limit(kB, 9, Side::Buy, 101, 8)),
            (Reports{FillR(kA, maker, 1, 100, 5, 0, Liquidity::Maker),
                     FillR(kB, mex::kNoExchangeOrderId, 9, 100, 5, 3, Liquidity::Taker),
                     DoneR(kB, 9, Reason::CannotRest)}));
  EXPECT_EQ(h.Send(CancelOf(kB, 9)), (Reports{CancelRejectR(kB, 9)}));
}

TEST(EngineTest, UnfilledPartOfAMarketOrderIsDone)
{
  Harness h;
  const ExchangeOrderId maker = h.Rest(Limit(kA, 1, Side::Sell, 100, 5));

  EXPECT_EQ(h.Send(Market(kB, 9, Side::Buy, 8)),
            (Reports{FillR(kA, maker, 1, 100, 5, 0, Liquidity::Maker),
                     FillR(kB, mex::kNoExchangeOrderId, 9, 100, 5, 3, Liquidity::Taker),
                     DoneR(kB, 9, Reason::MarketRemainder)}));
  EXPECT_EQ(h.Send(Market(kB, 10, Side::Buy, 8)),
            (Reports{DoneR(kB, 10, Reason::MarketRemainder)}));
}

TEST(EngineTest, LimitOrderThatFillsNothingAndCannotRestIsRejected)
{
  Harness ladder_full{16, 1};
  ladder_full.Rest(Limit(kC, 3, Side::Buy, 90, 1));
  EXPECT_EQ(ladder_full.Send(Limit(kB, 9, Side::Buy, 95, 3)),
            (Reports{RejectR(kB, 9, Reason::BookFull)}));

  Harness pool_full{1};
  pool_full.Rest(Limit(kA, 1, Side::Sell, 100, 5));
  EXPECT_EQ(pool_full.Send(Limit(kB, 9, Side::Buy, 90, 1)),
            (Reports{RejectR(kB, 9, Reason::BookFull)}));
}

// ---------------------------------------------------------------------------
// Request numbers and op numbers

[[nodiscard]] bool IsOneAck(const Reports &p_reports)
{
  return p_reports.size() == 1 && std::holds_alternative<mex::wire::Ack>(p_reports[0].report_);
}

// Checks that a retried NewOrder does not execute again. Its first attempt
// fills completely, so its client order id is free and only the request number
// can stop the retry. The book must be as the first attempt left it.
TEST(EngineTest, RetriedNewOrderDoesNotExecuteAgainAfterItsFirstAttemptFilled)
{
  Harness h;
  h.Rest(Limit(kA, 1, Side::Sell, 100, 5));
  const ExchangeOrderId second = h.Rest(Limit(kA, 2, Side::Sell, 100, 5));
  const Command taker = Limit(kB, 7, Side::Buy, 100, 5);
  ASSERT_EQ(h.SendAs(10, taker).size(), 3U); // maker Fill, taker Fill, Ack

  EXPECT_EQ(h.SendAs(10, taker), (Reports{RejectR(kB, 7, Reason::DuplicateRequest)}));

  EXPECT_EQ(h.Send(Market(kC, 9, Side::Buy, 20)),
            (Reports{FillR(kA, second, 2, 100, 5, 0, Liquidity::Maker),
                     FillR(kC, mex::kNoExchangeOrderId, 9, 100, 5, 15, Liquidity::Taker),
                     DoneR(kC, 9, Reason::MarketRemainder)}));
}

// Checks that a retried Replace and a retried Cancel get the reject of their
// own type, and that the retry changes nothing: the order keeps its priority.
TEST(EngineTest, RetriedReplaceAndCancelGetTheRejectOfTheirType)
{
  Harness h;
  const ExchangeOrderId first = h.Rest(Limit(kA, 1, Side::Buy, 100, 10));
  h.Rest(Limit(kB, 2, Side::Buy, 100, 10));
  ASSERT_EQ(h.SendAs(20, ReplaceOf(kA, 1, 100, 6)), (Reports{ReplaceAckR(kA, 1, 100, 6)}));

  // Executed again, this Replace does not reduce, so it would lose priority.
  EXPECT_EQ(h.SendAs(20, ReplaceOf(kA, 1, 100, 6)),
            (Reports{ReplaceRejectR(kA, 1, Reason::DuplicateRequest)}));
  EXPECT_EQ(h.Send(Limit(kC, 9, Side::Sell, 100, 1)),
            (Reports{FillR(kA, first, 1, 100, 1, 5, Liquidity::Maker),
                     FillR(kC, mex::kNoExchangeOrderId, 9, 100, 1, 0, Liquidity::Taker),
                     AckR(kC, 9, mex::kNoExchangeOrderId, 0)}));

  ASSERT_EQ(h.SendAs(30, CancelOf(kA, 1)), (Reports{CancelAckR(kA, 1)}));
  EXPECT_EQ(h.SendAs(30, CancelOf(kA, 1)),
            (Reports{CancelRejectR(kA, 1, Reason::DuplicateRequest)}));
}

// Checks the rule "above the last applied number of that session": gaps
// pass, another session does not interfere, and a number that is equal, lower,
// or 0 is rejected.
TEST(EngineTest, RequestNumbersAreCheckedForEachSessionAndCanHaveGaps)
{
  Harness h;

  EXPECT_TRUE(IsOneAck(h.SendAs(5, Limit(kA, 1, Side::Buy, 100, 1))));
  // The session sent 6, 7, and 8 to another engine.
  EXPECT_TRUE(IsOneAck(h.SendAs(9, Limit(kA, 2, Side::Buy, 100, 1))));
  EXPECT_TRUE(IsOneAck(h.SendAs(3, Limit(kB, 1, Side::Buy, 100, 1))));

  EXPECT_EQ(h.SendAs(9, Limit(kA, 3, Side::Buy, 100, 1)),
            (Reports{RejectR(kA, 3, Reason::DuplicateRequest)}));
  EXPECT_EQ(h.SendAs(7, Limit(kA, 3, Side::Buy, 100, 1)),
            (Reports{RejectR(kA, 3, Reason::DuplicateRequest)}));
  EXPECT_EQ(h.SendAs(0, Limit(kC, 1, Side::Buy, 100, 1)),
            (Reports{RejectR(kC, 1, Reason::DuplicateRequest)}));
  EXPECT_TRUE(IsOneAck(h.SendAs(10, Limit(kA, 3, Side::Buy, 100, 1))));
}

// Checks that the engine records the op number of each command it applies,
// also when the command is a duplicate request. The op numbers of one engine
// have gaps, because the other engines apply the ops between them.
TEST(EngineTest, RecordsTheLastAppliedOp)
{
  Harness h;
  EXPECT_EQ(h.EngineRef().LastAppliedOp(), OpNumber{});

  const RequestNumber request{1};
  static_cast<void>(
      h.SendNumbered(Numbered(Limit(kA, 1, Side::Buy, 100, 1), OpNumber{4}, request)));
  EXPECT_EQ(h.EngineRef().LastAppliedOp(), OpNumber{4});

  EXPECT_EQ(h.SendNumbered(Numbered(Limit(kA, 2, Side::Buy, 100, 1), OpNumber{9}, request)),
            (Reports{RejectR(kA, 2, Reason::DuplicateRequest)}));
  EXPECT_EQ(h.EngineRef().LastAppliedOp(), OpNumber{9});
}

// Checks that Apply stops the process when an op number is not above the last
// applied one: the same op again, and a lower one.
TEST(EngineDeathTest, OpNumberThatDoesNotIncreaseViolatesTheContract)
{
  Harness h;
  const Command order = Limit(kA, 1, Side::Buy, 100, 1);
  static_cast<void>(h.SendNumbered(Numbered(order, OpNumber{5}, RequestNumber{1})));

  EXPECT_DEATH(h.EngineRef().Apply(Numbered(order, OpNumber{5}, RequestNumber{2})), "");
  EXPECT_DEATH(h.EngineRef().Apply(Numbered(order, OpNumber{4}, RequestNumber{2})), "");
}

// ---------------------------------------------------------------------------
// Determinism and the state digest

// A pseudo-random command stream for both books. The id, price, and quantity
// ranges are small, so fills, live-id duplicates, unknown orders, and full
// books are frequent. One command in 8 reuses a recent request number.
std::vector<Command> GeneratedCommands(std::uint64_t p_seed, std::uint32_t p_count)
{
  std::uint64_t state = p_seed;
  const auto next = [&state]()
  {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
  };
  constexpr std::uint64_t kFirstRequest = 5; // so that a reused number is not below 1
  std::vector<Command> commands;
  commands.reserve(p_count);
  for (std::uint32_t i = 0; i < p_count; ++i)
  {
    const auto session = static_cast<SessionId>(kA + next() % 3U);
    const ClientOrderId id = next() % 24U;
    const Side side = (next() & 1U) == 0U ? Side::Buy : Side::Sell;
    const auto price = static_cast<std::int64_t>(95 + next() % 10U);
    const auto qty = static_cast<std::uint32_t>(1 + next() % 9U);
    const auto slot = static_cast<std::uint32_t>(next() % kSymbols.size());
    const std::uint64_t pick = next() % 10U;
    const Command command = pick < 5U   ? Limit(session, id, side, price, qty, slot)
                            : pick < 6U ? Market(session, id, side, qty)
                            : pick < 8U ? CancelOf(session, id)
                                        : ReplaceOf(session, id, price, qty);
    const std::uint64_t back = next() % 8U == 0U ? 1 + next() % 4U : 0;
    commands.push_back(
        Numbered(command, OpNumber{std::uint64_t{i} + 1}, RequestNumber{kFirstRequest + i - back}));
  }
  return commands;
}

// The evidence that the engine is a valid replicated state machine. Two
// independent engines get one generated command sequence, one through its
// inbound queue and one through Apply. After every command their reports and
// their state digests must be equal. The sequence must reach every report type
// and every reason that the engine can give, or the comparison shows little.
TEST(EngineDeterminismTest, TwoEnginesGiveEqualReportsAndDigestsForOneCommandSequence)
{
  constexpr std::uint32_t kCommands = 20000;
  constexpr std::uint32_t kMaxOrders = 12;
  constexpr std::uint32_t kMaxPriceLevels = 4;
  Harness popped{kMaxOrders, kMaxPriceLevels};
  Harness applied{kMaxOrders, kMaxPriceLevels};
  std::set<std::size_t> report_types;
  std::set<Reason> reasons;

  for (const Command &command : GeneratedCommands(0x1234cafef00dULL, kCommands))
  {
    const Reports reports = popped.SendNumbered(command);
    ASSERT_EQ(applied.ApplyNumbered(command), reports) << "op " << command.op_number_.value_;
    ASSERT_EQ(applied.EngineRef().StateDigest(), popped.EngineRef().StateDigest())
        << "op " << command.op_number_.value_;
    for (const Report &report : reports)
    {
      report_types.insert(report.report_.index());
      std::visit(
          [&reasons](const auto &p_message)
          {
            if constexpr (requires { p_message.reason_; })
            {
              reasons.insert(p_message.reason_);
            }
          },
          report.report_);
    }
  }

  EXPECT_EQ(report_types.size(), std::variant_size_v<mex::OrderReport>);
  EXPECT_EQ(reasons, (std::set<Reason>{Reason::DuplicateClientOrderId, Reason::BookFull,
                                       Reason::UnknownOrder, Reason::TooLate, Reason::CannotRest,
                                       Reason::MarketRemainder, Reason::DuplicateRequest}));
}

// Captures the exact live bytes so comparisons also check canonical serialization.
std::vector<std::byte> SnapshotOf(const Engine &p_engine)
{
  std::vector<std::byte> bytes(p_engine.SnapshotBytesMax());
  const auto size = p_engine.Snapshot(bytes);
  EXPECT_TRUE(size.has_value());
  bytes.resize(size.value_or(0));
  return bytes;
}

// Checks restore at several op boundaries and compares every later report and digest.
TEST(EngineSnapshotTest, GeneratedCommandsContinueAfterRestore)
{
  const auto commands = GeneratedCommands(0x1234cafef00dULL, 3000);
  for (const std::size_t boundary : {0U, 1U, 37U, 500U, 1999U})
  {
    Harness a{12, 4};
    for (std::size_t i = 0; i < boundary; ++i)
    {
      static_cast<void>(a.ApplyNumbered(commands[i]));
    }
    Harness b{12, 4};
    const auto snapshot = SnapshotOf(a.EngineRef());
    ASSERT_TRUE(b.EngineRef().Restore(snapshot));
    ASSERT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest());
    ASSERT_EQ(b.EngineRef().LastAppliedOp(), a.EngineRef().LastAppliedOp());
    ASSERT_EQ(SnapshotOf(b.EngineRef()), snapshot);
    for (std::size_t i = boundary; i < commands.size(); ++i)
    {
      ASSERT_EQ(b.ApplyNumbered(commands[i]), a.ApplyNumbered(commands[i])) << i;
      ASSERT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest()) << i;
      ASSERT_EQ(b.EngineRef().LastAppliedOp(), a.EngineRef().LastAppliedOp());
    }
    EXPECT_EQ(SnapshotOf(b.EngineRef()), SnapshotOf(a.EngineRef()));
    // Restore must also replace an occupied book and clear the old client index.
    ASSERT_TRUE(b.EngineRef().Restore(snapshot));
    Harness original{12, 4};
    ASSERT_TRUE(original.EngineRef().Restore(snapshot));
    EXPECT_EQ(b.EngineRef().StateDigest(), original.EngineRef().StateDigest());
  }
}

// Changes one version-1 field. All fields have eight little-endian bytes.
void SetSnapshotField(std::vector<std::byte> &p_bytes, std::size_t p_field, std::uint64_t p_value)
{
  for (std::size_t i = 0; i < 8; ++i)
  {
    p_bytes[p_field * 8 + i] = static_cast<std::byte>((p_value >> (i * 8U)) & 0xffU);
  }
}

// Checks every truncation, trailing bytes, and configuration mismatches.
TEST(EngineSnapshotTest, RejectsShortTrailingAndUnequalConfiguration)
{
  Harness a{4, 2};
  a.Rest(Limit(kA, 1, Side::Buy, 100, 5));
  const auto bytes = SnapshotOf(a.EngineRef());
  Harness b{4, 2};
  for (std::size_t size = 0; size < bytes.size(); ++size)
  {
    EXPECT_FALSE(b.EngineRef().Restore(std::span{bytes}.first(size))) << size;
    std::vector<std::byte> short_output(size);
    EXPECT_FALSE(a.EngineRef().Snapshot(short_output).has_value()) << size;
  }
  auto trailing = bytes;
  trailing.push_back(std::byte{});
  EXPECT_FALSE(b.EngineRef().Restore(trailing));
  Harness other_orders{5, 2};
  Harness other_levels{4, 3};
  EXPECT_FALSE(other_orders.EngineRef().Restore(bytes));
  EXPECT_FALSE(other_levels.EngineRef().Restore(bytes));
  // The header contains version, book count, session count, then book configurations.
  for (const std::size_t field : {0U, 1U, 2U, 3U, 4U, 5U, 6U})
  {
    auto corrupt = bytes;
    SetSnapshotField(corrupt, field, 999);
    EXPECT_FALSE(b.EngineRef().Restore(corrupt)) << field;
  }
  EXPECT_TRUE(b.EngineRef().Restore(bytes));
  EXPECT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest());
}

// Checks individual invalid slots, keys, quantities, owners, and counts.
TEST(EngineSnapshotTest, RejectsCorruptStateFields)
{
  Harness a{4, 2};
  a.Rest(Limit(kA, 1, Side::Buy, 100, 5)); // slot 3
  a.Rest(Limit(kA, 2, Side::Buy, 100, 6)); // slot 2
  const auto bytes = SnapshotOf(a.EngineRef());
  // Header is 14 fields. Bid level: count, price, order count, then two orders.
  const std::array<std::pair<std::size_t, std::uint64_t>, 17> corruptions{{{14, 3},
                                                                           {16, 0},
                                                                           {16, 5},
                                                                           {17, 4},
                                                                           {22, 3},
                                                                           {18, 0},
                                                                           {18, 0x100000000ULL},
                                                                           {19, kSessionCount},
                                                                           {25, 1},
                                                                           {21, 0xffffffffULL},
                                                                           {27, 3},
                                                                           {28, 0x100000000ULL},
                                                                           {32, 1},
                                                                           {32, 5},
                                                                           {33, 4},
                                                                           {33, 3},
                                                                           {34, 0}}};
  for (const auto &[field, value] : corruptions)
  {
    Harness b{4, 2};
    auto corrupt = bytes;
    SetSnapshotField(corrupt, field, value);
    EXPECT_FALSE(b.EngineRef().Restore(corrupt)) << field;
    ASSERT_TRUE(b.EngineRef().Restore(bytes));
    EXPECT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest());
  }
}

// Checks strict ladder order for both sides and rejects crossed books.
TEST(EngineSnapshotTest, RejectsInvalidLadderPrices)
{
  for (const Side side : {Side::Buy, Side::Sell})
  {
    Harness a{4, 2};
    a.Rest(Limit(kA, 1, side, 99, 5));
    a.Rest(Limit(kA, 2, side, 100, 6));
    auto bytes = SnapshotOf(a.EngineRef());
    // The empty bid count precedes the asks for a sell-only book.
    const std::size_t first_price = side == Side::Buy ? 15 : 16;
    SetSnapshotField(bytes, first_price + 7, side == Side::Buy ? 99 : 100);
    Harness b{4, 2};
    EXPECT_FALSE(b.EngineRef().Restore(bytes));
  }
  Harness a{4, 2};
  a.Rest(Limit(kA, 1, Side::Buy, 99, 5));
  a.Rest(Limit(kA, 2, Side::Sell, 100, 6));
  auto bytes = SnapshotOf(a.EngineRef());
  SetSnapshotField(bytes, 23, 99); // Ask price equals the best bid.
  Harness b{4, 2};
  EXPECT_FALSE(b.EngineRef().Restore(bytes));
}

// Checks that live records increase size and that discard mode preserves state.
TEST(EngineSnapshotTest, ReportsCanBeDisabledAndEnabled)
{
  Harness a;
  Harness b;
  const auto empty_size = SnapshotOf(a.EngineRef()).size();
  const auto initial_digest = b.EngineRef().StateDigest();
  b.EngineRef().SetReportsEnabled(false);
  const Command rest = Numbered(Limit(kA, 1, Side::Sell, 100, 5), OpNumber{1}, RequestNumber{1});
  EXPECT_FALSE(a.ApplyNumbered(rest).empty());
  b.EngineRef().Apply(rest);
  EXPECT_FALSE(b.Outbound().TryPop().has_value());
  EXPECT_NE(b.EngineRef().StateDigest(), initial_digest);
  EXPECT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest());
  EXPECT_GT(SnapshotOf(a.EngineRef()).size(), empty_size);
  b.EngineRef().SetReportsEnabled(true);
  const Command take = Numbered(Market(kB, 9, Side::Buy, 3), OpNumber{2}, RequestNumber{2});
  EXPECT_EQ(a.ApplyNumbered(take), b.ApplyNumbered(take));
  EXPECT_EQ(b.EngineRef().StateDigest(), a.EngineRef().StateDigest());
}

// Checks that another generated sequence of the same length ends with another digest.
TEST(EngineDeterminismTest, DifferentCommandSequenceGivesADifferentDigest)
{
  constexpr std::uint32_t kCommands = 2000;
  Harness first;
  Harness other;
  for (const Command &command : GeneratedCommands(1, kCommands))
  {
    static_cast<void>(first.ApplyNumbered(command));
  }
  for (const Command &command : GeneratedCommands(2, kCommands))
  {
    static_cast<void>(other.ApplyNumbered(command));
  }

  EXPECT_NE(first.EngineRef().StateDigest(), other.EngineRef().StateDigest());
}

// The digest after p_commands, sent as requests 1, 2, and so on.
std::uint64_t DigestAfter(std::initializer_list<Command> p_commands)
{
  Harness h;
  for (const Command &command : p_commands)
  {
    static_cast<void>(h.Send(command));
  }
  return h.EngineRef().StateDigest();
}

// Checks that each part of the replicated state changes the digest. Each pair
// of sequences has the same length and the same request numbers, and differs
// in the one part that its comment names.
TEST(EngineDigestTest, DependsOnEachPartOfTheState)
{
  const Command rest_a1 = Limit(kA, 1, Side::Sell, 100, 10);
  const Command rest_a2 = Limit(kA, 2, Side::Sell, 100, 10);
  const std::uint64_t two_resting = DigestAfter({rest_a1, rest_a2});
  EXPECT_EQ(two_resting, DigestAfter({rest_a1, rest_a2}));

  // Book: quantity, price, and book (symbol slot) of a resting order.
  EXPECT_NE(two_resting, DigestAfter({rest_a1, Limit(kA, 2, Side::Sell, 100, 11)}));
  EXPECT_NE(two_resting, DigestAfter({rest_a1, Limit(kA, 2, Side::Sell, 101, 10)}));
  EXPECT_NE(two_resting, DigestAfter({rest_a1, Limit(kA, 2, Side::Sell, 100, 10, 1)}));
  // Time priority and client-order index: which client order id has which slot.
  EXPECT_NE(two_resting, DigestAfter({rest_a2, rest_a1}));
  // Order metadata: the filled quantity. Both books end with 6 resting.
  EXPECT_NE(DigestAfter({rest_a1, Limit(kB, 9, Side::Buy, 100, 4)}),
            DigestAfter({Limit(kA, 1, Side::Sell, 100, 6), CancelOf(kB, 9)}));
  // Slot generation: an order that rested and left, against no order at all.
  EXPECT_NE(DigestAfter({rest_a1, CancelOf(kA, 1)}),
            DigestAfter({CancelOf(kA, 1), CancelOf(kA, 1)}));
  // Free slots: the orders leave in another order, so the next order takes another slot.
  EXPECT_NE(DigestAfter({rest_a1, rest_a2, CancelOf(kA, 1), CancelOf(kA, 2)}),
            DigestAfter({rest_a1, rest_a2, CancelOf(kA, 2), CancelOf(kA, 1)}));
}

// Checks that the client table and the last applied op change the digest when
// the books are equal.
TEST(EngineDigestTest, DependsOnTheClientTableAndTheLastAppliedOp)
{
  Harness request_5;
  Harness request_6;
  static_cast<void>(request_5.SendAs(5, Limit(kA, 1, Side::Sell, 100, 10)));
  static_cast<void>(request_6.SendAs(6, Limit(kA, 1, Side::Sell, 100, 10)));
  EXPECT_NE(request_5.EngineRef().StateDigest(), request_6.EngineRef().StateDigest());

  // A duplicate request changes only the last applied op.
  const std::uint64_t before = request_5.EngineRef().StateDigest();
  ASSERT_EQ(request_5.SendAs(5, Limit(kA, 1, Side::Sell, 100, 10)),
            (Reports{RejectR(kA, 1, Reason::DuplicateRequest)}));
  EXPECT_NE(request_5.EngineRef().StateDigest(), before);
}

TEST(EngineTest, FullOutboundQueueStallsUntilTheGatewayDrainsIt)
{
  Harness h{16, 16, 2};
  for (ClientOrderId id = 1; id <= 3; ++id)
  {
    h.Rest(Limit(kA, id, Side::Sell, 100, 1));
  }
  EXPECT_EQ(h.EngineRef().OutboundStalls(), 0U);
  ASSERT_TRUE(h.Inbound().TryPush(h.Next(Market(kB, 9, Side::Buy, 3)))); // 7 reports, ring of 2

  Reports reports;
  std::thread gateway(
      [&h, &reports]()
      {
        // Drain only once the engine has seen a full ring.
        while (h.EngineRef().OutboundStalls() == 0)
        {
        }
        while (reports.size() < 7)
        {
          if (const std::optional<Report> report = h.Outbound().TryPop())
          {
            reports.push_back(*report);
          }
        }
      });
  EXPECT_TRUE(h.EngineRef().ProcessOne());
  gateway.join();

  EXPECT_GE(h.EngineRef().OutboundStalls(), 1U);
  ASSERT_EQ(reports.size(), 7U);
  EXPECT_EQ(reports.back(), AckR(kB, 9, mex::kNoExchangeOrderId, 0));
}

TEST(EngineTest, RunsOnItsOwnThreadWithRealQueues)
{
  constexpr std::uint32_t kPairs = 20000;
  constexpr std::size_t kSmallRing = 64;
  const std::array<BookConfig, 1> books{BookConfig{kSymbols[0], 64, 16}};
  SpscQueue<Command> inbound{kSmallRing};
  SpscQueue<Report> outbound{kSmallRing};
  Engine engine{books, kSessionCount, inbound, outbound};
  std::thread engine_thread([&engine]() { engine.Run(); });

  // Gateway: never blocks on a full inbound queue; drains outbound meanwhile.
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  std::uint64_t acks = 0;
  std::uint64_t other = 0;
  std::uint64_t received = 0;
  const auto drain = [&]()
  {
    while (const std::optional<Report> report = outbound.TryPop())
    {
      ++received;
      if (const auto *fill = std::get_if<mex::wire::Fill>(&report->report_))
      {
        const bool maker = fill->liquidity_ == Liquidity::Maker;
        maker_fills += maker && report->session_id_ == kA ? 1U : 0U;
        taker_fills += !maker && report->session_id_ == kB ? 1U : 0U;
      }
      else if (std::holds_alternative<mex::wire::Ack>(report->report_))
      {
        ++acks;
      }
      else
      {
        ++other;
      }
    }
  };
  for (std::uint32_t i = 0; i < kPairs; ++i)
  {
    // A rests 1 @ 100, then B takes it, so every pair ends with an empty book.
    const std::uint64_t number = std::uint64_t{2} * i + 1;
    for (const Command &command :
         {Numbered(Limit(kA, i, Side::Sell, 100, 1), OpNumber{number}, RequestNumber{number}),
          Numbered(Limit(kB, i, Side::Buy, 100, 1), OpNumber{number + 1},
                   RequestNumber{number + 1})})
    {
      while (!inbound.TryPush(command))
      {
        drain();
      }
    }
    drain();
  }
  const std::uint64_t expected = std::uint64_t{4} * kPairs; // A: Ack, Fill. B: Fill, Ack.
  while (received < expected)
  {
    drain();
  }
  engine.Stop();
  engine_thread.join();
  drain();

  EXPECT_EQ(received, expected);
  EXPECT_EQ(maker_fills, kPairs);
  EXPECT_EQ(taker_fills, kPairs);
  EXPECT_EQ(acks, std::uint64_t{2} * kPairs);
  EXPECT_EQ(other, 0U);
}

} // namespace
