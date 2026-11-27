"use strict";

const symbol = document.querySelector("#symbol");
const status = document.querySelector("#status");
const spread = document.querySelector("#spread");
const sequence = document.querySelector("#sequence");
const bids = document.querySelector("#bids");
const asks = document.querySelector("#asks");
const books = new Map();
let selected = "";
let state = "connecting";
let paint = 0;

function drawLevels(body, levels) {
  if (levels.length === 0) {
    const row = document.createElement("tr");
    const cell = row.insertCell();
    cell.colSpan = 2;
    cell.className = "empty";
    cell.textContent = state === "live" ? "No orders" : "Waiting for data";
    body.replaceChildren(row);
    return;
  }
  const maximum = levels.reduce((max, [, size]) => BigInt(size) > max ? BigInt(size) : max, 1n);
  const rows = levels.map(([price, size]) => {
    const row = document.createElement("tr");
    row.style.setProperty("--depth", `${BigInt(size) * 100n / maximum}%`);
    row.insertCell().textContent = price;
    row.insertCell().textContent = size;
    return row;
  });
  body.replaceChildren(...rows);
}

function draw() {
  paint = 0;
  const ids = [...books.keys()].sort((a, b) => a - b).map(String);
  if (ids.join() !== Array.from(symbol.options, option => option.value).join()) {
    symbol.replaceChildren(...ids.map(id => new Option(id, id)));
  }
  if (ids.includes(selected)) {
    symbol.value = selected;
  }
  symbol.disabled = ids.length === 0;
  const book = books.get(Number(symbol.value));
  spread.textContent = book?.bids.length && book.asks.length
    ? String(BigInt(book.asks[0][0]) - BigInt(book.bids[0][0])) : "—";
  sequence.textContent = book?.seq ?? "—";
  drawLevels(bids, book?.bids ?? []);
  drawLevels(asks, book?.asks ?? []);
}

function schedulePaint() {
  if (!paint) {
    paint = requestAnimationFrame(draw);
  }
}

function reset(next) {
  state = next;
  books.clear();
  status.dataset.state = state;
  status.textContent = {connecting: "Connecting", live: "Live", reconnecting: "Reconnecting"}[state];
  schedulePaint();
}

symbol.addEventListener("change", () => {
  selected = symbol.value;
  schedulePaint();
});

const events = new EventSource("/events");
events.addEventListener("status", event => reset(JSON.parse(event.data).state));
events.addEventListener("book", event => {
  const book = JSON.parse(event.data);
  books.set(book.symbol, book);
  schedulePaint();
});
events.onerror = () => reset("reconnecting");
schedulePaint();
