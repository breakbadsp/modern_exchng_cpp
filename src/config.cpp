#include "config.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mex
{
namespace
{

// The longest record is a symbol line: the keyword and six values.
constexpr std::size_t kMaxFields = 7;

using Fields = std::span<const std::string_view>;
using Status = std::expected<void, std::string>;

struct GatewayKeys
{
  std::optional<std::string> listen_address_{};
  std::optional<std::uint16_t> port_{};
  std::optional<std::uint32_t> max_connections_{};
  std::optional<std::uint32_t> inbound_queue_capacity_{};
  std::optional<std::uint32_t> outbound_queue_capacity_{};
};

struct ParseState
{
  Config config_{};
  std::size_t line_ = 0;                    // line being parsed, 1-based
  std::vector<std::size_t> engine_lines_{}; // line of each engine record
  GatewayKeys gateway_{};
};

[[nodiscard]] bool IsBlank(char p_char)
{
  return p_char == ' ' || p_char == '\t' || p_char == '\r';
}

// Stores at most p_fields.size() fields, but returns the full count.
[[nodiscard]] std::size_t Split(std::string_view p_line, std::span<std::string_view> p_fields)
{
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < p_line.size())
  {
    if (IsBlank(p_line[pos]))
    {
      ++pos;
      continue;
    }
    const std::size_t start = pos;
    while (pos < p_line.size() && !IsBlank(p_line[pos]))
    {
      ++pos;
    }
    if (count < p_fields.size())
    {
      p_fields[count] = p_line.substr(start, pos - start);
    }
    ++count;
  }
  return count;
}

// The whole token must be a decimal integer in [p_min, max of T].
template <typename T>
[[nodiscard]] std::expected<T, std::string> ParseInteger(std::string_view p_name,
                                                         std::string_view p_text,
                                                         T p_min = std::numeric_limits<T>::min())
{
  T value{};
  const char *const end = p_text.data() + p_text.size();
  const auto [ptr, ec] = std::from_chars(p_text.data(), end, value);
  if (ec != std::errc{} || ptr != end || value < p_min)
  {
    return std::unexpected(std::format("{} must be an integer in [{}, {}], got '{}'", p_name, p_min,
                                       std::numeric_limits<T>::max(), p_text));
  }
  return value;
}

[[nodiscard]] wire::Name ToName(std::string_view p_text) pre(p_text.size() <= wire::kNameSize)
{
  wire::Name name{};
  p_text.copy(name.data(), name.size());
  return name;
}

[[nodiscard]] Status ParseEngine(ParseState &p_state, Fields p_fields)
{
  auto core = ParseInteger<std::uint32_t>("core", p_fields[1]);
  if (!core)
  {
    return std::unexpected(std::move(core.error()));
  }
  p_state.config_.engines_.push_back(EngineConfig{.core_ = *core});
  p_state.engine_lines_.push_back(p_state.line_);
  return {};
}

[[nodiscard]] Status ParseSymbol(ParseState &p_state, Fields p_fields)
{
  auto symbol_id = ParseInteger<wire::SymbolId>("symbol_id", p_fields[1]);
  if (!symbol_id)
  {
    return std::unexpected(std::move(symbol_id.error()));
  }
  auto engine = ParseInteger<std::uint32_t>("engine", p_fields[2]);
  if (!engine)
  {
    return std::unexpected(std::move(engine.error()));
  }
  auto max_orders = ParseInteger<std::uint32_t>("max_orders", p_fields[3], 1);
  if (!max_orders)
  {
    return std::unexpected(std::move(max_orders.error()));
  }
  auto max_price_levels = ParseInteger<std::uint32_t>("max_price_levels", p_fields[4]);
  if (!max_price_levels)
  {
    return std::unexpected(std::move(max_price_levels.error()));
  }
  auto min_price = ParseInteger<std::int64_t>("min_price", p_fields[5]);
  if (!min_price)
  {
    return std::unexpected(std::move(min_price.error()));
  }
  auto max_price = ParseInteger<std::int64_t>("max_price", p_fields[6]);
  if (!max_price)
  {
    return std::unexpected(std::move(max_price.error()));
  }

  Config &config = p_state.config_;
  if (*engine >= config.engines_.size())
  {
    return std::unexpected(std::format("engine {} is not declared above (engines declared: {})",
                                       *engine, config.engines_.size()));
  }
  if (*min_price > *max_price)
  {
    return std::unexpected(
        std::format("min_price {} is greater than max_price {}", *min_price, *max_price));
  }
  if (std::ranges::find(config.symbols_, *symbol_id, &SymbolConfig::symbol_id_) !=
      config.symbols_.end())
  {
    return std::unexpected(std::format("duplicate symbol_id {}", *symbol_id));
  }

  std::vector<BookConfig> &books = config.engines_[*engine].books_;
  config.symbols_.push_back(SymbolConfig{.symbol_id_ = *symbol_id,
                                         .engine_ = *engine,
                                         .slot_ = static_cast<std::uint32_t>(books.size()),
                                         .min_price_ = Price{*min_price},
                                         .max_price_ = Price{*max_price}});
  books.push_back(BookConfig{.symbol_id_ = *symbol_id,
                             .max_orders_ = *max_orders,
                             .max_price_levels_ = *max_price_levels});
  return {};
}

[[nodiscard]] Status ParseSession(ParseState &p_state, Fields p_fields)
{
  const std::string_view session_name = p_fields[1];
  const std::string_view token = p_fields[2];
  if (session_name.size() > wire::kNameSize)
  {
    return std::unexpected(
        std::format("session_name '{}' is longer than {} bytes", session_name, wire::kNameSize));
  }
  if (token.size() > wire::kNameSize)
  {
    return std::unexpected(std::format("token is longer than {} bytes", wire::kNameSize));
  }
  const wire::Name name = ToName(session_name);
  std::vector<SessionConfig> &sessions = p_state.config_.sessions_;
  if (std::ranges::find(sessions, name, &SessionConfig::session_name_) != sessions.end())
  {
    return std::unexpected(std::format("duplicate session_name '{}'", session_name));
  }
  sessions.push_back(SessionConfig{.session_name_ = name, .token_ = ToName(token)});
  return {};
}

template <typename T>
[[nodiscard]] Status SetOnce(std::optional<T> &p_slot, std::string_view p_key, T p_value)
{
  if (p_slot.has_value())
  {
    return std::unexpected(std::format("duplicate gateway key '{}'", p_key));
  }
  p_slot = std::move(p_value);
  return {};
}

[[nodiscard]] Status SetCapacity(std::optional<std::uint32_t> &p_slot, std::string_view p_key,
                                 std::string_view p_value)
{
  auto capacity = ParseInteger<std::uint32_t>(p_key, p_value, 1);
  if (!capacity)
  {
    return std::unexpected(std::move(capacity.error()));
  }
  if (!std::has_single_bit(*capacity))
  {
    return std::unexpected(std::format("{} must be a power of two, got {}", p_key, *capacity));
  }
  return SetOnce(p_slot, p_key, *capacity);
}

[[nodiscard]] Status ParseGateway(ParseState &p_state, Fields p_fields)
{
  const std::string_view key = p_fields[1];
  const std::string_view value = p_fields[2];
  GatewayKeys &gateway = p_state.gateway_;
  if (key == "address")
  {
    std::string address{value};
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1)
    {
      return std::unexpected(std::format("address must be an IPv4 address, got '{}'", value));
    }
    return SetOnce(gateway.listen_address_, key, std::move(address));
  }
  if (key == "port")
  {
    auto port = ParseInteger<std::uint16_t>(key, value, 1);
    if (!port)
    {
      return std::unexpected(std::move(port.error()));
    }
    return SetOnce(gateway.port_, key, *port);
  }
  if (key == "max_connections")
  {
    auto max_connections = ParseInteger<std::uint32_t>(key, value, 1);
    if (!max_connections)
    {
      return std::unexpected(std::move(max_connections.error()));
    }
    return SetOnce(gateway.max_connections_, key, *max_connections);
  }
  if (key == "inbound_queue_capacity")
  {
    return SetCapacity(gateway.inbound_queue_capacity_, key, value);
  }
  if (key == "outbound_queue_capacity")
  {
    return SetCapacity(gateway.outbound_queue_capacity_, key, value);
  }
  return std::unexpected(std::format("unknown gateway key '{}'", key));
}

// Each parser gets exactly 1 + values_ fields; the keyword is p_fields[0].
using RecordParser = Status (*)(ParseState &p_state, Fields p_fields);

struct RecordKind
{
  std::string_view keyword_;
  std::size_t values_ = 0;
  RecordParser parse_ = nullptr;
};

constexpr std::array kRecordKinds{
    RecordKind{.keyword_ = "engine", .values_ = 1, .parse_ = &ParseEngine},
    RecordKind{.keyword_ = "symbol", .values_ = 6, .parse_ = &ParseSymbol},
    RecordKind{.keyword_ = "session", .values_ = 2, .parse_ = &ParseSession},
    RecordKind{.keyword_ = "gateway", .values_ = 2, .parse_ = &ParseGateway},
};

[[nodiscard]] Status ParseRecord(ParseState &p_state, Fields p_stored, std::size_t p_count)
{
  const std::string_view keyword = p_stored[0];
  const auto kind = std::ranges::find(kRecordKinds, keyword, &RecordKind::keyword_);
  if (kind == kRecordKinds.end())
  {
    return std::unexpected(std::format("unknown keyword '{}'", keyword));
  }
  if (p_count != kind->values_ + 1)
  {
    return std::unexpected(std::format("'{}' needs {} value{}, got {}", keyword, kind->values_,
                                       kind->values_ == 1 ? "" : "s", p_count - 1));
  }
  return kind->parse_(p_state, p_stored.first(p_count));
}

// Checks that need the whole file.
[[nodiscard]] std::expected<Config, ConfigError> Finish(ParseState &&p_state)
{
  Config &config = p_state.config_;
  if (config.engines_.empty())
  {
    return std::unexpected(ConfigError{.line_ = 0, .reason_ = "no engine records"});
  }
  if (config.symbols_.empty())
  {
    return std::unexpected(ConfigError{.line_ = 0, .reason_ = "no symbol records"});
  }
  if (config.sessions_.empty())
  {
    return std::unexpected(ConfigError{.line_ = 0, .reason_ = "no session records"});
  }
  for (std::size_t i = 0; i < config.engines_.size(); ++i)
  {
    if (config.engines_[i].books_.empty())
    {
      return std::unexpected(ConfigError{.line_ = p_state.engine_lines_[i],
                                         .reason_ = std::format("engine {} has no symbols", i)});
    }
  }

  GatewayKeys &keys = p_state.gateway_;
  const auto missing = [](std::string_view p_key)
  {
    return std::unexpected(
        ConfigError{.line_ = 0, .reason_ = std::format("missing gateway key '{}'", p_key)});
  };
  if (!keys.listen_address_)
  {
    return missing("address");
  }
  if (!keys.port_)
  {
    return missing("port");
  }
  if (!keys.max_connections_)
  {
    return missing("max_connections");
  }
  if (!keys.inbound_queue_capacity_)
  {
    return missing("inbound_queue_capacity");
  }
  if (!keys.outbound_queue_capacity_)
  {
    return missing("outbound_queue_capacity");
  }
  config.gateway_ = GatewayConfig{.listen_address_ = std::move(*keys.listen_address_),
                                  .port_ = *keys.port_,
                                  .max_connections_ = *keys.max_connections_,
                                  .inbound_queue_capacity_ = *keys.inbound_queue_capacity_,
                                  .outbound_queue_capacity_ = *keys.outbound_queue_capacity_};
  return std::move(config);
}

} // namespace

std::expected<Config, ConfigError> ParseConfig(std::string_view p_text)
{
  ParseState state{};
  std::string_view rest = p_text;
  while (!rest.empty())
  {
    const std::size_t end = rest.find('\n');
    const std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    ++state.line_;

    std::array<std::string_view, kMaxFields> fields{};
    const std::size_t count = Split(line, fields);
    if (count == 0 || fields[0].starts_with('#'))
    {
      continue;
    }
    const Fields stored{fields.data(), std::min(count, fields.size())};
    if (auto status = ParseRecord(state, stored, count); !status)
    {
      return std::unexpected(
          ConfigError{.line_ = state.line_, .reason_ = std::move(status.error())});
    }
  }
  return Finish(std::move(state));
}

std::expected<Config, ConfigError> LoadConfig(const std::filesystem::path &p_path)
{
  std::ifstream file(p_path, std::ios::binary);
  if (!file)
  {
    return std::unexpected(
        ConfigError{.line_ = 0, .reason_ = std::format("cannot open '{}'", p_path.string())});
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  if (file.bad())
  {
    return std::unexpected(
        ConfigError{.line_ = 0, .reason_ = std::format("cannot read '{}'", p_path.string())});
  }
  return ParseConfig(text);
}

} // namespace mex
