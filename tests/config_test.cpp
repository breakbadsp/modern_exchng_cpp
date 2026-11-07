#include "config.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace
{

using mex::Config;
using mex::ParseConfig;
using mex::Price;

mex::wire::Name NameOf(std::string_view p_text)
{
  mex::wire::Name name{};
  p_text.copy(name.data(), name.size());
  return name;
}

// Eight lines. A bad record appended to it is on line 9.
constexpr std::string_view kValid = "engine 0\n"
                                    "symbol 1 0 100 10 1 1000\n"
                                    "session alice secret\n"
                                    "gateway address 127.0.0.1\n"
                                    "gateway port 9000\n"
                                    "gateway max_connections 64\n"
                                    "gateway inbound_queue_capacity 1024\n"
                                    "gateway outbound_queue_capacity 1024\n";

std::string ValidWith(std::string_view p_line)
{
  return std::string{kValid} + std::string{p_line} + "\n";
}

void ExpectError(std::string_view p_text, std::size_t p_line, std::string_view p_reason)
{
  const auto result = ParseConfig(p_text);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().line_, p_line);
  EXPECT_EQ(result.error().reason_, p_reason);
}

void ExpectExampleConfig(const Config &p_config)
{
  ASSERT_EQ(p_config.engines_.size(), 2U);
  EXPECT_EQ(p_config.engines_[0].core_, 2U);
  EXPECT_EQ(p_config.engines_[1].core_, 3U);

  // Engine 0 serves symbols 1 and 3, engine 1 serves symbol 2.
  ASSERT_EQ(p_config.engines_[0].books_.size(), 2U);
  EXPECT_EQ(p_config.engines_[0].books_[0].symbol_id_, 1U);
  EXPECT_EQ(p_config.engines_[0].books_[0].max_orders_, 100000U);
  EXPECT_EQ(p_config.engines_[0].books_[0].max_price_levels_, 1024U);
  EXPECT_EQ(p_config.engines_[0].books_[1].symbol_id_, 3U);
  EXPECT_EQ(p_config.engines_[0].books_[1].max_orders_, 10000U);
  EXPECT_EQ(p_config.engines_[0].books_[1].max_price_levels_, 256U);
  ASSERT_EQ(p_config.engines_[1].books_.size(), 1U);
  EXPECT_EQ(p_config.engines_[1].books_[0].symbol_id_, 2U);
  EXPECT_EQ(p_config.engines_[1].books_[0].max_orders_, 100000U);
  EXPECT_EQ(p_config.engines_[1].books_[0].max_price_levels_, 1024U);

  // Routing, in file order.
  ASSERT_EQ(p_config.symbols_.size(), 3U);
  EXPECT_EQ(p_config.symbols_[0].symbol_id_, 1U);
  EXPECT_EQ(p_config.symbols_[0].engine_, 0U);
  EXPECT_EQ(p_config.symbols_[0].slot_, 0U);
  EXPECT_EQ(p_config.symbols_[0].min_price_, Price{1});
  EXPECT_EQ(p_config.symbols_[0].max_price_, Price{1000000});
  EXPECT_EQ(p_config.symbols_[1].symbol_id_, 2U);
  EXPECT_EQ(p_config.symbols_[1].engine_, 1U);
  EXPECT_EQ(p_config.symbols_[1].slot_, 0U);
  EXPECT_EQ(p_config.symbols_[1].min_price_, Price{1});
  EXPECT_EQ(p_config.symbols_[1].max_price_, Price{1000000});
  EXPECT_EQ(p_config.symbols_[2].symbol_id_, 3U);
  EXPECT_EQ(p_config.symbols_[2].engine_, 0U);
  EXPECT_EQ(p_config.symbols_[2].slot_, 1U);
  EXPECT_EQ(p_config.symbols_[2].min_price_, Price{1});
  EXPECT_EQ(p_config.symbols_[2].max_price_, Price{50000});

  ASSERT_EQ(p_config.sessions_.size(), 2U);
  EXPECT_EQ(p_config.sessions_[0].session_name_, NameOf("alice"));
  EXPECT_EQ(p_config.sessions_[0].token_, NameOf("alice-secret"));
  EXPECT_EQ(p_config.sessions_[1].session_name_, NameOf("bob"));
  EXPECT_EQ(p_config.sessions_[1].token_, NameOf("bob-secret"));

  EXPECT_EQ(p_config.gateway_.listen_address_, "127.0.0.1");
  EXPECT_EQ(p_config.gateway_.port_, 9000U);
  EXPECT_EQ(p_config.gateway_.max_connections_, 64U);
  EXPECT_EQ(p_config.gateway_.inbound_queue_capacity_, 65536U);
  EXPECT_EQ(p_config.gateway_.outbound_queue_capacity_, 65536U);
}

TEST(ConfigTest, ParsesEveryValueAndTheRouting)
{
  // Tabs, CRLF, comments, blank lines, and records in any order of kinds.
  constexpr std::string_view kText = "# exchange\n"
                                     "\n"
                                     "engine 2\r\n"
                                     "engine\t3\n"
                                     "   # indented comment\n"
                                     "session alice alice-secret\n"
                                     "symbol 1 0 100000 1024 1 1000000\n"
                                     "symbol 2 1 100000 1024 1 1000000\n"
                                     "gateway address 127.0.0.1\n"
                                     "symbol 3 0 10000 256 1 50000\n"
                                     "session bob bob-secret\n"
                                     "gateway port 9000\n"
                                     "gateway max_connections 64\n"
                                     "gateway inbound_queue_capacity 65536\n"
                                     "  gateway   outbound_queue_capacity   65536  ";
  const auto config = ParseConfig(kText);
  ASSERT_TRUE(config.has_value()) << config.error().line_ << ": " << config.error().reason_;
  ExpectExampleConfig(*config);
}

TEST(ConfigTest, LoadsTheExampleFileFromDisk)
{
  const auto config = mex::LoadConfig(MEX_EXAMPLE_CONFIG);
  ASSERT_TRUE(config.has_value()) << config.error().line_ << ": " << config.error().reason_;
  ExpectExampleConfig(*config);
}

TEST(ConfigTest, AMissingFileIsAnError)
{
  const auto config = mex::LoadConfig("/nonexistent/mex.cfg");
  ASSERT_FALSE(config.has_value());
  EXPECT_EQ(config.error().line_, 0U);
  EXPECT_EQ(config.error().reason_, "cannot open '/nonexistent/mex.cfg'");
}

TEST(ConfigTest, AcceptsNamesAndTokensOfExactly16Bytes)
{
  const auto config = ParseConfig(ValidWith("session 0123456789abcdef fedcba9876543210"));
  ASSERT_TRUE(config.has_value()) << config.error().reason_;
  ASSERT_EQ(config->sessions_.size(), 2U);
  EXPECT_EQ(config->sessions_[1].session_name_, NameOf("0123456789abcdef"));
  EXPECT_EQ(config->sessions_[1].token_, NameOf("fedcba9876543210"));
}

TEST(ConfigTest, AcceptsNegativePricesAndASinglePriceBand)
{
  const auto config = ParseConfig(ValidWith("symbol 2 0 1 0 -5 -5"));
  ASSERT_TRUE(config.has_value()) << config.error().reason_;
  EXPECT_EQ(config->symbols_[1].min_price_, Price{-5});
  EXPECT_EQ(config->symbols_[1].max_price_, Price{-5});
}

TEST(ConfigTest, RejectsAnUnknownKeyword)
{
  ExpectError(ValidWith("engines 1"), 9, "unknown keyword 'engines'");
}

TEST(ConfigTest, RejectsTooFewFields)
{
  ExpectError(ValidWith("symbol 2 0 100 10 1"), 9, "'symbol' needs 6 values, got 5");
}

TEST(ConfigTest, RejectsTooManyFields)
{
  ExpectError(ValidWith("engine 1 2 3 4 5 6 7 8 9"), 9, "'engine' needs 1 value, got 9");
}

TEST(ConfigTest, RejectsANonNumericValue)
{
  ExpectError(ValidWith("symbol 2 0 1x0 10 1 1000"), 9,
              "max_orders must be an integer in [1, 4294967295], got '1x0'");
}

TEST(ConfigTest, RejectsAnOutOfRangeValue)
{
  ExpectError(ValidWith("symbol 4294967296 0 100 10 1 1000"), 9,
              "symbol_id must be an integer in [0, 4294967295], got '4294967296'");
}

TEST(ConfigTest, RejectsANegativeUnsignedValue)
{
  ExpectError(ValidWith("engine -1"), 9, "core must be an integer in [0, 4294967295], got '-1'");
}

TEST(ConfigTest, RejectsAnOutOfRangePrice)
{
  ExpectError(ValidWith("symbol 2 0 100 10 1 9223372036854775808"), 9,
              "max_price must be an integer in [-9223372036854775808, 9223372036854775807], "
              "got '9223372036854775808'");
}

TEST(ConfigTest, RejectsADuplicateSymbolId)
{
  ExpectError(ValidWith("symbol 1 0 100 10 1 1000"), 9, "duplicate symbol_id 1");
}

TEST(ConfigTest, RejectsADuplicateSessionName)
{
  ExpectError(ValidWith("session alice other"), 9, "duplicate session_name 'alice'");
}

TEST(ConfigTest, RejectsASymbolOnAMissingEngine)
{
  ExpectError(ValidWith("symbol 2 1 100 10 1 1000"), 9,
              "engine 1 is not declared above (engines declared: 1)");
}

TEST(ConfigTest, RejectsMinPriceAboveMaxPrice)
{
  ExpectError(ValidWith("symbol 2 0 100 10 5 4"), 9, "min_price 5 is greater than max_price 4");
}

TEST(ConfigTest, RejectsZeroMaxOrders)
{
  ExpectError(ValidWith("symbol 2 0 0 10 1 1000"), 9,
              "max_orders must be an integer in [1, 4294967295], got '0'");
}

TEST(ConfigTest, RejectsASessionNameLongerThan16Bytes)
{
  ExpectError(ValidWith("session 0123456789abcdefg secret"), 9,
              "session_name '0123456789abcdefg' is longer than 16 bytes");
}

TEST(ConfigTest, RejectsATokenLongerThan16BytesWithoutEchoingIt)
{
  ExpectError(ValidWith("session bob 0123456789abcdefg"), 9, "token is longer than 16 bytes");
}

TEST(ConfigTest, RejectsANonPowerOfTwoQueueCapacity)
{
  constexpr std::string_view kText = "engine 0\n"
                                     "symbol 1 0 100 10 1 1000\n"
                                     "session alice secret\n"
                                     "gateway inbound_queue_capacity 1000\n";
  ExpectError(kText, 4, "inbound_queue_capacity must be a power of two, got 1000");
}

TEST(ConfigTest, RejectsAZeroQueueCapacity)
{
  constexpr std::string_view kText = "engine 0\n"
                                     "gateway outbound_queue_capacity 0\n";
  ExpectError(kText, 2, "outbound_queue_capacity must be an integer in [1, 4294967295], got '0'");
}

TEST(ConfigTest, RejectsADuplicateGatewayKey)
{
  ExpectError(ValidWith("gateway port 9001"), 9, "duplicate gateway key 'port'");
}

TEST(ConfigTest, RejectsAnUnknownGatewayKey)
{
  ExpectError(ValidWith("gateway backlog 10"), 9, "unknown gateway key 'backlog'");
}

TEST(ConfigTest, RejectsAnAddressThatIsNotIpv4)
{
  constexpr std::string_view kText = "gateway address localhost\n";
  ExpectError(kText, 1, "address must be an IPv4 address, got 'localhost'");
}

TEST(ConfigTest, RejectsPortZero)
{
  constexpr std::string_view kText = "gateway port 0\n";
  ExpectError(kText, 1, "port must be an integer in [1, 65535], got '0'");
}

TEST(ConfigTest, RejectsAMissingGatewayKey)
{
  constexpr std::string_view kText = "engine 0\n"
                                     "symbol 1 0 100 10 1 1000\n"
                                     "session alice secret\n"
                                     "gateway address 127.0.0.1\n"
                                     "gateway max_connections 64\n"
                                     "gateway inbound_queue_capacity 1024\n"
                                     "gateway outbound_queue_capacity 1024\n";
  ExpectError(kText, 0, "missing gateway key 'port'");
}

TEST(ConfigTest, RejectsAnEmptyFile) { ExpectError("", 0, "no engine records"); }

TEST(ConfigTest, RejectsNoSymbols)
{
  ExpectError("engine 0\nsession alice secret\n", 0, "no symbol records");
}

TEST(ConfigTest, RejectsNoSessions)
{
  ExpectError("engine 0\nsymbol 1 0 100 10 1 1000\n", 0, "no session records");
}

TEST(ConfigTest, RejectsAnEngineWithNoSymbols)
{
  ExpectError(ValidWith("engine 1"), 9, "engine 1 has no symbols");
}

} // namespace
