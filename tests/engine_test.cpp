#include "engine.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <ostream>
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

constexpr SessionId kA = 1;
constexpr SessionId kB = 2;
constexpr SessionId kC = 3;
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

Report CancelRejectR(SessionId p_session, ClientOrderId p_id)
{
  return {p_session, mex::wire::CancelReject{.client_order_id_ = p_id,
                                             .symbol_id_ = kSymbols[0],
                                             .reason_ = Reason::UnknownOrder}};
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
        outbound_{p_outbound_capacity}, engine_{books_, inbound_, outbound_}
  {
  }

  // Pushes one command, lets the engine handle it, and returns every report.
  Reports Send(const Command &p_command)
  {
    EXPECT_TRUE(inbound_.TryPush(p_command));
    EXPECT_TRUE(engine_.ProcessOne());
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

TEST(EngineTest, FullOutboundQueueStallsUntilTheGatewayDrainsIt)
{
  Harness h{16, 16, 2};
  for (ClientOrderId id = 1; id <= 3; ++id)
  {
    h.Rest(Limit(kA, id, Side::Sell, 100, 1));
  }
  EXPECT_EQ(h.EngineRef().OutboundStalls(), 0U);
  ASSERT_TRUE(h.Inbound().TryPush(Market(kB, 9, Side::Buy, 3))); // 7 reports, ring of 2

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
  Engine engine{books, inbound, outbound};
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
    for (const Command &command :
         {Limit(kA, i, Side::Sell, 100, 1), Limit(kB, i, Side::Buy, 100, 1)})
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
