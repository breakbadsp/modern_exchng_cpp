// Run with Node.js and Playwright available to require():
// MEX_CHROMIUM=/usr/bin/chromium node scripts/check-threading-guide.cjs
const assert = require('node:assert/strict');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');

const guide = pathToFileURL(path.join(__dirname, '../docs/threading-model.html')).href;

async function check(browser, width, reducedMotion) {
  const page = await browser.newPage({viewport: {width, height: 950}, reducedMotion, offline: true});
  const errors = [];
  const externalRequests = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('request', (request) => {
    if (!request.url().startsWith('file:')) externalRequests.push(request.url());
  });
  await page.addInitScript(() => {
    const animate = Element.prototype.animate;
    Element.prototype.animate = function (...args) {
      const animation = animate.apply(this, args);
      // Hold packet animations for inspection. Browser delays cannot remove them.
      if (this.classList.contains('packet')) animation.pause();
      return animation;
    };
  });
  await page.goto(guide);

  const byId = (id) => page.locator(`#${id}`);
  const button = (name) => page.getByRole('button', {name, exact: true});
  const next = () => button('Next step').click();
  const choose = (name) => button(name).click();
  const expectText = async (id, expected) => assert.equal(await byId(id).textContent(), expected);
  const routing = page.getByRole('region', {name: 'Routing by symbol'});
  assert.equal(await routing.count(), 1);
  const alice = routing.getByRole('group', {name: 'Alice'});
  const bob = routing.getByRole('group', {name: 'Bob'});
  async function checkRouting(mode) {
    for (const session of [alice, bob]) {
      assert.match(await session.innerText(), /TCP session\s+Symbol 1\b/);
    }
    assert.match(await routing.innerText(), /mex-gateway\s+Commands by symbol · Reports by session/);
    for (const engine of [0, 1]) {
      const lane = routing.getByRole('group', {name: `Engine ${engine}`});
      const reader = mode === 'Standalone' ? `mex-engine-${engine}` : 'mex-bridge';
      const writer = mode === 'Standalone' ? `mex-engine-${engine}` : 'mex-replica';
      const symbols = engine === 0 ? 'Symbols 1 and 3' : 'Symbol 2';
      assert.deepEqual((await lane.innerText()).split('\n'), [
        `Engine ${engine}`, symbols, 'Command ring', `Gateway → ${reader}`,
        'Report ring', `${writer} → Gateway`,
      ]);
    }
    assert.equal(await routing.evaluate((element) => element.scrollWidth <= element.clientWidth), true);
  }
  async function seek(position) {
    // Drive the range with keyboard input, the same path used by a reader.
    await byId('scrub').press('Home');
    for (let step = 0; step < position; step++) await byId('scrub').press('ArrowRight');
  }

  await expectText('step-title', 'One resting sell, one incoming buy');
  await checkRouting('Standalone');
  assert.match(await bob.innerText(), /Resting sell/);
  assert.match(await byId('apply').textContent(), /mex-engine-0/);
  assert.equal(await page.locator('#routes path').count(), 7);
  const [apply, outbound, sender, recipient] = await Promise.all(
    ['apply', 'out', 'tx', 'report'].map((id) => byId(id).boundingBox()),
  );
  assert.ok(outbound.y >= apply.y + apply.height, 'The report path stays below the matching thread');
  assert.equal(outbound.y, sender.y);
  assert.equal(sender.y, recipient.y);
  await next();
  await expectText('in-state', '1 / 4');
  assert.match(await alice.innerText(), /Buy sent/);
  assert.equal(await page.locator('.packet').count(), reducedMotion === 'reduce' ? 0 : 1);
  await next();
  await expectText('step-title', 'Engine 0 pops the command');
  await expectText('in-state', '0 / 4');
  await seek(4);
  await expectText('out-state', '3 / 4');
  assert.match(await byId('step-detail').textContent(), /maker fill.*taker fill.*Ack with zero leaves/);
  assert.equal(await page.locator('.packet').count(), reducedMotion === 'reduce' ? 0 : 3);
  await next();
  await expectText('out-state', '0 / 4');
  assert.match(await bob.innerText(), /Maker fill/);
  assert.match(await alice.innerText(), /Taker fill \+ Ack/);
  assert.equal(await button('Next step').isDisabled(), true);

  for (const mode of ['Standalone', 'Replicated']) {
    await choose(mode);
    await checkRouting(mode);
    await choose('Queue pressure');
    await seek(2);
    await expectText('step-title', 'Full inbound ring returns SystemBusy');
    await expectText('in-state', '4 / 4');
    assert.equal(await page.locator('#routes path[stroke-width="2"]').count(), 1,
      'Only the gateway-to-client path carries the reject');
    await next();
    await expectText('step-title', 'Full outbound ring stalls the producer');
    await expectText('out-state', '4 / 4');
    assert.equal(await page.locator('.packet').count(), 0);
    assert.equal(await page.locator('#apply.blocked').count(), 1);
    await next();
    await expectText('out-state', '3 / 4');
    await next();
    await expectText('out-state', '4 / 4');

    await choose('Shutdown');
    await next();
    await expectText('step-title', mode === 'Standalone'
      ? 'Main stops and joins the engines'
      : 'Main stops and joins the bridge');
    if (mode === 'Replicated') {
      await next();
      await expectText('step-title', 'Main stops and joins the replica');
    }
    await next();
    await expectText('step-title', 'Main stops the gateway last');
  }

  await choose('Follow an order');
  assert.match(await byId('consume').textContent(), /mex-bridge/);
  assert.match(await byId('apply').textContent(), /mex-replica/);
  assert.equal(await page.locator('#routes path').count(), 8);
  await seek(3);
  await expectText('work-state', 'Committed');
  assert.equal(await page.locator('#gate .filled').count(), 2);
  await next();
  await expectText('work-state', 'Trade 5 @ 100');
  await seek(6);
  assert.match(await bob.innerText(), /Maker fill/);
  assert.match(await alice.innerText(), /Taker fill \+ Ack/);

  await choose('Quorum delay');
  await seek(2);
  await expectText('work-state', 'A awaiting reply');
  await expectText('in-state', '2 / 4');
  assert.equal(await page.locator('#gate .filled').count(), 1);
  await next();
  await expectText('work-state', 'A committed');
  await next();
  await expectText('work-state', 'B in progress');
  await expectText('in-state', '0 / 4');

  await page.clock.install();
  await byId('speed').selectOption('2');
  await choose('Replay');
  await page.clock.runFor(1300);
  await expectText('step-index', 'STEP 2 / 5');
  await choose('Pause');
  await page.clock.runFor(3000);
  await expectText('step-index', 'STEP 2 / 5');
  await choose('Standalone');
  await expectText('step-title', 'One resting sell, one incoming buy');
  assert.equal(await button('Quorum delay').count(), 0);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  assert.deepEqual(errors, []);
  assert.deepEqual(externalRequests, []);
  await page.close();
}

async function main() {
  const browser = await chromium.launch({executablePath: process.env.MEX_CHROMIUM});
  try {
    await check(browser, 1000, 'no-preference');
    await check(browser, 360, 'reduce');
    console.log('Threading guide passed at 1000 px and 360 px with offline playback.');
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
