'use strict';
// Page logic for the MEX fault game. The server is the one source of the
// cluster size, quorum, mission text, and process state. This file polls
// /api/state, shows it, and sends the player's actions to /api/action.

const POLL_MS = 700;
const ACTION_ERROR_MS = 8000;

const $ = id => document.getElementById(id);
const plural = (count, word) => `${count} ${word}${count === 1 ? '' : 's'}`;

const STATE_LABELS = {running: 'Running', frozen: 'Frozen', stopped: 'Crashed'};
const REPLICA_BUTTONS = [
  {action: 'crash', label: 'Crash', usable: state => state !== 'stopped'},
  {action: 'freeze', label: 'Freeze', usable: state => state === 'running'},
  {action: 'restart', label: 'Restore', usable: state => state !== 'running'},
];
const PROOF_STATUS = {unchecked: 'Not checked', checking: 'Checking', passed: 'Passed',
                      inconclusive: 'Inconclusive', failed: 'Failed', error: 'Error'};

let actionPending = false;   // One player action at a time.
let pollInFlight = null;     // One state request at a time.
let actionErrorTimer = 0;

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setUsable(button, usable) {
  button.setAttribute('aria-disabled', String(!usable));
}

// ---- Rendering -----------------------------------------------------------

function renderIntro(cluster) {
  $('intro').textContent =
    `${cluster.replica_count} real replicas run one exchange. Orders commit while a quorum of ` +
    `${cluster.quorum} runs, so the cluster tolerates ${plural(cluster.fault_limit, 'failed replica')}. ` +
    'Crash or freeze replicas while real orders flow, restore them, and check that their state agrees.';
}

function renderQuorum(s) {
  const {replica_count: count, quorum} = s.cluster;
  $('running').textContent = `${s.running} / ${count}`;
  $('quorum').textContent = quorum;
  $('running').closest('.quorum').classList.toggle('lost', s.running < quorum);
  const spare = s.running - quorum;
  $('capacity').textContent =
    spare > 0 ? `Orders can commit. ${plural(spare, 'more replica')} can fail before orders stop.`
    : spare === 0 ? 'Orders can commit, but one more failure stops them.'
    : `Orders cannot commit. Restore ${plural(-spare, 'replica')} to get a quorum.`;
}

function renderStatus(s) {
  const fresh = s.report_age !== null && s.report_age < s.cluster.stall_seconds;
  const status = $('status');
  status.textContent =
    s.verifying ? 'Verifying state'
    : fresh ? 'Orders are committing'
    : s.running < s.cluster.quorum ? 'No quorum. Orders wait.'
    : 'Waiting for order reports';
  status.classList.toggle('bad', !fresh && !s.verifying);
  status.classList.toggle('wait', s.verifying);

  $('client').textContent =
    s.verifying ? 'Order client: paused for verification'
    : s.gateway !== null ? `Order client: connected to the gateway of replica ${s.gateway}`
    : `Order client: reconnecting${s.error ? ` (${s.error})` : ''}`;
}

function buildReplica(replica) {
  const card = element('li', 'replica');
  card.dataset.replica = replica.id;
  card.append(element('h3', '', `Replica ${replica.id}`), element('p', 'state'), element('p', 'detail'));
  const controls = element('div', 'controls');
  controls.setAttribute('role', 'group');
  controls.setAttribute('aria-label', `Replica ${replica.id} controls`);
  for (const {action, label} of REPLICA_BUTTONS) {
    const button = element('button', '', label);
    button.type = 'button';
    button.dataset.action = action;
    button.dataset.replica = replica.id;
    button.setAttribute('aria-label', `${label} replica ${replica.id}`);
    controls.append(button);
  }
  card.append(controls);
  return card;
}

// Cards are made one time and then updated, so keyboard focus stays on a button.
function renderReplicas(s) {
  const list = $('replicas');
  if (list.children.length !== s.replicas.length) {
    list.replaceChildren(...s.replicas.map(buildReplica));
  }
  s.replicas.forEach((replica, index) => {
    const card = list.children[index];
    card.dataset.state = replica.state;
    card.querySelector('.state').textContent = STATE_LABELS[replica.state];
    card.querySelector('.detail').textContent =
      `gateway :${replica.port}` + (s.gateway === replica.id ? ' · order client is here' : '');
    card.querySelectorAll('button').forEach((button, i) =>
      setUsable(button, !s.verifying && REPLICA_BUTTONS[i].usable(replica.state)));
  });
}

function renderMissions(s) {
  const list = $('missions');
  if (list.children.length !== s.missions.length) {
    list.replaceChildren(...s.missions.map(mission => {
      const item = element('li', 'mission');
      const text = element('div');
      text.append(element('strong', '', mission.title), element('p', '', mission.instruction));
      item.append(element('span', 'check'), text);
      return item;
    }));
  }
  const next = s.missions.find(mission => !mission.done);
  s.missions.forEach((mission, index) => {
    const item = list.children[index];
    item.classList.toggle('done', mission.done);
    item.classList.toggle('current', mission === next);
    const check = item.querySelector('.check');
    check.textContent = mission.done ? '✓' : index + 1;
    check.setAttribute('aria-label', mission.done ? 'Done' : `Check ${index + 1}, not done`);
  });
  const done = s.missions.filter(mission => mission.done).length;
  $('score').textContent = `${done} / ${s.missions.length}`;
  if (s.verifying) {
    $('next-title').textContent = 'Wait for verification';
    $('next-text').textContent = 'The controls are locked until the check is complete.';
  } else if (next) {
    $('next-title').textContent = next.title;
    $('next-text').textContent = next.instruction;
  } else {
    $('next-title').textContent = `All ${s.missions.length} checks are complete`;
    $('next-text').textContent = 'Inject more faults, verify again, or stop the server with Ctrl+C.';
  }
  setUsable($('verify'), !s.verifying);
  setUsable($('heal'), !s.verifying);
  $('verify').textContent = s.verifying ? 'Checking state…' : 'Verify recovered state';
}

function proofText(s) {
  const proof = s.verification;
  const count = s.cluster.replica_count;
  switch (proof.status) {
    case 'checking':
      return `${proof.step} The order client is paused. All replicas restart with their existing storage.`;
    case 'passed':
      return `All ${count} replicas had the same state digest at the same commit position when the ` +
             `check ran (after ${proof.reports.toLocaleString()} terminal order reports). The workload has resumed.`;
    case 'inconclusive':
      return 'The replicas stopped at different commit positions, so their digests cannot be compared. ' +
             'This does not show divergence. Verify again.';
    case 'failed':
      return 'The digests differ at one commit position. Inspect the retained logs.';
    case 'error':
      return `Verification did not complete: ${proof.error}. Verify again.`;
    default:
      return `Verification pauses the order client, restores all ${count} replicas, sends a probe order ` +
             'through each gateway, and stops the replicas briefly to read their state digests. ' +
             'It then restarts them with the same storage files.';
  }
}

function renderVerification(s) {
  const proof = s.verification;
  const status = $('proof-status');
  status.textContent = PROOF_STATUS[proof.status] || proof.status;
  status.classList.toggle('bad', proof.status === 'failed' || proof.status === 'error');
  status.classList.toggle('wait', proof.status === 'checking' || proof.status === 'inconclusive');
  $('proof-text').textContent = proofText(s);

  const progress = $('proof-progress');
  progress.hidden = proof.status !== 'checking';
  progress.max = s.cluster.replica_count;
  progress.value = proof.probed || 0;
  progress.setAttribute('aria-label', `Gateways probed: ${progress.value} of ${progress.max}`);

  const table = $('digests');
  table.hidden = proof.digests.length === 0;
  table.tBodies[0].replaceChildren(...proof.digests.map(digest => {
    const row = element('tr');
    row.append(element('td', '', digest.replica), element('td', '', digest.digest),
               element('td', '', digest.operation));
    return row;
  }));
}

function renderTimeline(s) {
  $('events').replaceChildren(...s.events.map(event => {
    const row = element('li');
    row.append(element('time', '', `${event.time.toFixed(1)} s`), event.message);
    return row;
  }));
  $('elapsed').textContent = `${s.seconds.toFixed(0)} s`;
  $('work').textContent = s.work_dir;
}

function renderChart(samples) {
  const rates = samples.slice(1).map((sample, i) =>
    Math.max(0, (sample.reports - samples[i].reports) / Math.max(.1, sample.time - samples[i].time)));
  const top = Math.max(1, ...rates);
  const points = rates.map((rate, i) => `${i * 700 / Math.max(1, rates.length - 1)},${75 - rate * 65 / top}`);
  $('chart-line').setAttribute('d', points.length ? 'M' + points.join('L') : '');
  $('chart-area').setAttribute('d', points.length ? 'M0,75L' + points.join('L') + 'L700,75Z' : '');
}

function render(s) {
  renderIntro(s.cluster);
  renderQuorum(s);
  renderStatus(s);
  renderReplicas(s);
  renderMissions(s);
  renderVerification(s);
  renderTimeline(s);
  renderChart(s.samples);
  for (const key of ['acks', 'duplicates', 'fills']) $(key).textContent = s[key].toLocaleString();
}

// ---- Server communication ------------------------------------------------

function showAlert(id, message) {
  $(id).textContent = message;
  $(id).hidden = !message;
}

async function fetchState() {
  try {
    const response = await fetch('/api/state');
    if (!response.ok) throw Error('State request failed');
    render(await response.json());
    showAlert('connection', '');   // A good answer removes the old warning.
  } catch {
    $('status').textContent = 'Server disconnected';
    $('status').classList.add('bad');
    showAlert('connection', 'No answer from the game server. The page shows old data. It tries again automatically.');
  }
}

// Share the request in flight, so that requests cannot overlap or arrive out of order.
function refresh() {
  pollInFlight ??= fetchState().finally(() => { pollInFlight = null; });
  return pollInFlight;
}

async function pollForever() {
  await refresh();
  setTimeout(pollForever, POLL_MS);
}

async function sendAction(action, replica) {
  if (actionPending) return;
  actionPending = true;
  document.body.setAttribute('aria-busy', 'true');
  clearTimeout(actionErrorTimer);
  showAlert('action-error', '');
  try {
    const response = await fetch('/api/action', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({action, replica}),
    });
    const result = await response.json();
    if (!response.ok) throw Error(result.error);
  } catch (error) {
    showAlert('action-error', `Action not done: ${error.message}`);
    actionErrorTimer = setTimeout(() => showAlert('action-error', ''), ACTION_ERROR_MS);
  } finally {
    // A request that started before the action can hold the old state.
    await pollInFlight;
    await refresh();
    actionPending = false;
    document.body.removeAttribute('aria-busy');
  }
}

document.addEventListener('click', event => {
  const button = event.target.closest('button[data-action]');
  if (!button || button.getAttribute('aria-disabled') === 'true') return;
  const replica = button.dataset.replica;
  sendAction(button.dataset.action, replica === undefined ? undefined : Number(replica));
});

pollForever();
