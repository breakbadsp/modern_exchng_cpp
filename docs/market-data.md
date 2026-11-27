# Market data

Start the exchange with a separate market-data port. Run the client in another
terminal.

```sh
./build/mex_exchange config/example.cfg --market-data-port 9001
python3 tools/market_data/client.py --feed-port 9001 --port 8080
```

Open [the order book](http://127.0.0.1:8080/) and select a symbol. Orders on the
trading port change the displayed book. To generate traffic with the example
configuration, run:

```sh
./build/mex_loadgen --port 9000 --symbols 1,2,3 --orders 20000 --window 512
```

The feed uses the IPv4 address in the gateway configuration. Omit
`--market-data-port` to disable the feed. A port of `0` asks the OS for a free
port. The exchange prints the assigned port. Put `--io` before the market-data
option if you use both options. When you run replicas on the same host, give
each replica a different market-data port.

The client binds to loopback by default. Use `--feed-host` to connect to another
host. Both endpoints use cleartext. Use a trusted network or a TLS proxy for
remote access.

## Design

Coinbase Exchange publishes a Level 2 snapshot, then absolute size changes.
Binance Spot also uses absolute depth changes and checks sequence continuity
when merging them with a snapshot. [Coinbase Exchange Level 2](https://docs.cdp.coinbase.com/exchange/websocket-feed/channels#level2-channel) · [Binance Spot depth streams](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams)

Nasdaq TotalView-ITCH publishes order events through transports such as
MoldUDP64. CME MDP 3.0 publishes incremental market-by-price data and uses a
separate recovery feed to align snapshots with update sequence numbers.
[Nasdaq TotalView-ITCH](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf) · [CME MDP 3.0 recovery](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457672425)

MEX uses the best 20 price levels per side and combines changes within each
20 ms publication interval. The publisher takes its view after a whole
command. This preserves the final state of a multi-price sweep or replace.
Intermediate views can be overwritten during a burst. The feed is suitable for
depth displays and current-book clients.

A separate thread calculates and encodes changes. The matching thread copies
up to 40 levels per command into fixed storage, with no socket calls, locks, or
heap allocation. Up to 32 TCP subscribers share one publisher. The publisher
closes a connection if more than 1 MiB of output would be pending or a write
makes no progress for five seconds while output is pending.

Each replica publishes its locally applied book. A restart can rebuild some
state from disk and learn more committed operations from peers. A replica can
lag the primary. Sequences are independent across replicas and process starts.

## Feed protocol version 1

Connect to the TCP port and read UTF-8 JSON Lines. A connection sends one
`hello`, then one `snapshot` per listed symbol. Live `update` and
`heartbeat` messages follow.

```jsonl
{"type":"hello","version":1,"depth":20,"symbols":[1,2,3]}
{"type":"snapshot","symbol":1,"seq":"7","bids":[["101","4"]],"asks":[["103","9"]]}
{"type":"snapshot","symbol":2,"seq":"0","bids":[],"asks":[]}
{"type":"snapshot","symbol":3,"seq":"0","bids":[],"asks":[]}
{"type":"update","symbol":1,"seq":"8","bids":[["101","0"],["100","6"]],"asks":[]}
{"type":"heartbeat"}
```

- A level is a pair of price in integer ticks and total quantity in units.
  Price, quantity, and sequence use decimal strings to preserve 64-bit values.
- Both sides list their best price first. Apply all changes in a message before
  exposing the new book. A quantity of zero removes the level from the
  published view. A newly visible level can replace it.
- Each symbol has its own sequence. A snapshot sets the starting sequence.
  Each update for that symbol must increment it by one. A change outside the
  visible depth does not advance the sequence.
- The publisher sends a heartbeat about once per second. After a sequence
  gap, invalid message, or connection close, discard the connection's books
  and reconnect for fresh snapshots. The Python client also reconnects after a
  read timeout.
- The browser receives complete reconstructed books over Server-Sent Events.
  It clears the tables when the client loses the feed and repopulates them
  after the initial snapshots arrive.

Run the socket and HTTP journey with `ctest --preset ninja -R market_data_live`.
Run the handoff model and its implementation checks with
`verification/TLA/run check market-data`.
