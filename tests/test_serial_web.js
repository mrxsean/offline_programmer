// Run with Node.js; exercises the actual embedded page without a browser or ESP32.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../main/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
const serialCode = script.slice(script.indexOf('/* ---------- external UART0 debug'), script.indexOf('async function refreshInfo()'));
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, { value:'', textContent:'', disabled:false, scrollHeight:0 });
  return elements.get(id);
};
const requests = [], timers = [];
let response = { ok:true, written:3 };
let networkError = false;
const context = vm.createContext({
  TextDecoder, TextEncoder, Uint8Array,
  document:{ getElementById:element },
  setTimeout:(fn, delay) => { timers.push(delay); return timers.length; },
  clearTimeout:() => {},
  fetch:async (url, options) => {
    requests.push({url, options});
    if (networkError) throw new Error('offline');
    return { ok:response.ok, json:async () => response };
  }
});
vm.runInContext('let otaBusy = false;\n' + serialCode, context);
const run = code => vm.runInContext(code, context);

(async () => {
  element('serialTxMode').value = 'hex';
  for (const value of ['', '0', 'GG', '0x01']) {
    element('serialTx').value = value;
    await run('sendSerial()');
  }
  assert.equal(requests.length, 0);
  element('serialTx').value = '00 ff\n7F';
  await run('sendSerial()');
  assert.deepEqual(Array.from(requests.pop().options.body), [0, 255, 127]);
  element('serialTxMode').value = 'text';
  element('serialEnding').value = 'crlf';
  element('serialTx').value = '中文';
  response.written = 8;
  await run('sendSerial()');
  assert.deepEqual(Array.from(requests.pop().options.body), Array.from(new TextEncoder().encode('中文\r\n')));
  for (const value of ['A'.repeat(257), '中'.repeat(90)]) {
    element('serialTx').value = value;
    await run('sendSerial()');
    assert.equal(requests.length, 0);
  }
  element('serialTx').value = 'A';
  response = {ok:false, written:2, error:'partial'};
  await run('sendSerial()');
  assert.match(element('serialNotice').textContent, /partial.*2/);
  assert.equal(requests.length, 1);
  assert.equal(element('serialSend').disabled, false);
  requests.length = 0;
  networkError = true;
  await run('sendSerial()');
  assert.match(element('serialNotice').textContent, /结果可能未确认/);
  assert.equal(requests.length, 1);
  networkError = false;
  requests.length = 0;
  run('otaBusy = true');
  await run('sendSerial()');
  await run('configureSerial()');
  assert.equal(requests.length, 0);
  run('otaBusy = false; serialActive = true');
  element('serialRxMode').value = 'text';
  response = {ok:true, hex:'E4B8', next:2, lost:0, errors:0, baud:115200, more:true};
  await run('pollSerial()');
  assert.equal(element('serialRx').value, '');
  assert.equal(timers.pop(), 0);
  response = {...response, hex:'AD', next:3, more:false};
  await run('pollSerial()');
  assert.equal(element('serialRx').value, '中');
  assert.equal(timers.pop(), 100);
  assert.equal(requests.pop().url, '/api/serial?cursor=2');
  element('serialRxMode').value = 'hex';
  run('clearSerial()');
  response = {...response, hex:'00FF7F', next:6, lost:4, errors:2};
  await run('pollSerial()');
  assert.equal(element('serialRx').value, '00 FF 7F ');
  assert.match(element('serialStats').textContent, /缓冲丢失：4.*错误事件：2/);
  element('serialRx').value = 'A'.repeat(32768);
  await run('pollSerial()');
  assert.equal(element('serialRx').value.length, 32768);
  networkError = true;
  await run('pollSerial()');
  assert.equal(run('serialActive'), false);
  assert.match(element('serialStats').textContent, /offline/);
  networkError = false;
  element('serialBaud').value = '9600';
  response = {ok:true};
  await run('configureSerial()');
  assert.match(element('serialNotice').textContent, /9600/);
  assert.equal(element('serialApply').disabled, false);
  console.log('serial web: syntax, HEX/UTF-8, line endings, length limits, failed sends, OTA guard, streaming RX, bounded display and errors PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
