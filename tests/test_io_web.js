const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../main/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
const code = script.slice(script.indexOf('/* ---------- tabs and configurable IO debug'), script.indexOf('\nrefreshIo(true);'));
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {id, value:'', textContent:'', hidden:false, disabled:false,
    dataset:{}, options:[], setAttribute(k,v) { this[k] = v; },
    replaceChildren(...options) { this.options = options; }, add(option) { this.options.push(option); }, focus() {document.activeElement = this;} });
  return elements.get(id);
};
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]);
assert.equal(new Set(ids).size, ids.length);
const names = ['programmer','uart','i2c','spi','gpio','adc','ota'];
const panels = names.map(n => element('tab-' + n));
const tabs = names.map(n => element('tabBtn-' + n));
names.forEach(n => { assert(ids.includes('tab-' + n)); assert(ids.includes('tabBtn-' + n)); });
const selectors = [];
for (const match of html.matchAll(/<select id="([^"]+)" class="io-pin" data-owner="([^"]+)"([^>]*)>/g)) {
  const select = element(match[1]); select.dataset.owner = match[2];
  if (match[3].includes('data-adc')) select.dataset.adc = 'true';
  selectors.push(select);
}
const controls = [{disabled:false}, {disabled:false}];
let keydown;
const document = {getElementById:element, activeElement:tabs[0],
  querySelector:() => ({addEventListener:(event,fn) => {keydown = fn;}}),
  querySelectorAll:selector => ({'[role="tabpanel"]':panels,'[role="tab"]':tabs,'.io-pin':selectors,'.io-controls':controls}[selector])};
const state = {ok:true, pins:Array.from({length:22}, (_, pin) => ({pin, reserved:[3,4,7,10].includes(pin) ? '' : 'reserved', owner:'', adc:pin <= 4})), i2c:{active:false}, spi:{active:false}, adc:{active:false}};
let actionResponse = {ok:true}, actions = [];
const context = vm.createContext({document, Option:function(text,value) {this.text = text; this.value = value; this.disabled = false;},
  fetch:async (url, options) => {
    if (options.method === 'POST') { actions.push(JSON.parse(options.body)); return {ok:true,json:async () => actionResponse}; }
    return {ok:true,json:async () => state};
  }});
vm.runInContext('let otaBusy = false; function updateOtaButton() {}\n' + code, context);
const run = code => vm.runInContext(code, context);
(async () => {
  element('i2cWriteTx').value = '00 FF';
  run("showTab('i2c')");
  assert.deepEqual(panels.map(p => p.hidden), [true,true,false,true,true,true,true]);
  assert.equal(element('ioOverview').hidden, false);
  run("showTab('uart')");
  assert.equal(element('i2cWriteTx').value, '00 FF');
  assert.equal(element('ioOverview').hidden, true);
  document.activeElement = tabs[1];
  keydown({key:'ArrowRight',preventDefault:() => {}});
  assert.equal(document.activeElement, tabs[2]);
  assert.equal(tabs[2]['aria-selected'], 'true');
  await run('refreshIo()');
  assert.equal(element('adcPin').options.find(o => o.value === '7').disabled, true);
  assert.equal(element('i2cSda').options.find(o => o.value === '5').disabled, true);
  element('i2cSda').value = '3'; element('i2cScl').value = '4'; element('i2cHz').value = '10000';
  await run("configureIo('i2c')");
  assert.deepEqual(actions.pop(), {op:'i2c_config', sda:3, scl:4, hz:10000});
  assert.equal(element('i2cSda').value, '3');
  state.pins[3].owner = 'I2C';
  await run('refreshIo()');
  assert.equal(element('spiSclk').options.find(o => o.value === '3').disabled, true);
  assert.equal(element('i2cSda').options.find(o => o.value === '3').disabled, false);
  element('spiSclk').value = '';
  await run("configureIo('spi')");
  assert.equal(actions.length, 0);
  element('i2cReadRegisterBytes').value = '0';
  element('i2cWriteRegisterBytes').value = '0';
  element('i2cWriteAddress').value = '0x50';
  element('i2cWriteLength').value = '2';
  for (const value of ['0', 'GG', 'FF'.repeat(65)]) {
    element('i2cWriteTx').value = value; await run("transferI2c('write')");
    assert.equal(actions.length, 0);
  }
  element('i2cWriteTx').value = '00 ff';
  element('i2cReadAddress').value = '51';
  element('i2cReadLength').value = '2';
  actionResponse = {ok:true,hex:'007F'};
  await run("transferI2c('read')");
  assert.deepEqual(actions.pop(), {op:'i2c_transfer',hex:'',address:81,read:2});
  assert.match(element('i2cReadResult').textContent, /00 7F/);
  actionResponse = {ok:true,hex:''};
  await run("transferI2c('write')");
  assert.deepEqual(actions.pop(), {op:'i2c_transfer',hex:'00ff',address:80,read:0,write:2});
  assert.match(element('i2cWriteResult').textContent, /写入.*2/);
  assert.match(element('i2cReadResult').textContent, /00 7F/);
  for (const direction of ['read', 'write']) {
    const prefix = direction === 'read' ? 'i2cRead' : 'i2cWrite';
    for (const length of ['', '0', '-1', '65', '1.5']) {
      element(prefix + 'Length').value = length;
      await run("transferI2c('" + direction + "')");
      assert.equal(actions.length, 0);
    }
    element(prefix + 'Length').value = '2';
    for (const address of ['', '02', '78', '80', 'GG', '0x']) {
      element(prefix + 'Address').value = address;
      await run("transferI2c('" + direction + "')");
      assert.equal(actions.length, 0);
    }
    element(prefix + 'Address').value = '50';
  }
  element('i2cWriteLength').value = '1';
  await run("transferI2c('write')");
  assert.equal(actions.length, 0); // Never silently truncate or pad a write.
  element('i2cReadRegisterBytes').value = '2';
  element('i2cReadRegister').value = '0x1234';
  run("updateI2cRegister('read')");
  assert.equal(element('i2cReadRegister').disabled, false);
  await run("transferI2c('read')");
  assert.deepEqual(actions.pop(), {op:'i2c_transfer', address:80, hex:'', read:2, register_bytes:2, register:0x1234});
  element('i2cWriteRegisterBytes').value = '1';
  element('i2cWriteRegister').value = 'FE';
  element('i2cWriteLength').value = '2';
  await run("transferI2c('write')");
  assert.deepEqual(actions.pop(), {op:'i2c_transfer', address:80, hex:'00ff', read:0, write:2, register_bytes:1, register:254});
  for (const register of ['', '100', '0x100', '-1', '0xGG']) {
    element('i2cWriteRegister').value = register;
    await run("transferI2c('write')");
    assert.equal(actions.length, 0);
  }
  element('i2cReadRegisterBytes').value = '0';
  run("updateI2cRegister('read')");
  assert.equal(element('i2cReadRegister').disabled, true);
  await run("transferI2c('read')");
  assert.deepEqual(actions.pop(), {op:'i2c_transfer', address:80, hex:'', read:2});
  actionResponse = {ok:false,error:'pin busy'};
  await run("ioAction('spi_config','spi')");
  assert.match(element('spiResult').textContent, /pin busy/);
  assert(controls.every(f => !f.disabled));
  actionResponse = {ok:true,raw:2048,mv:null};
  await run("ioAction('adc_read','adc')");
  assert.match(element('adcResult').textContent, /2048.*校准不可用/);
  actions = [];
  run('otaBusy = true');
  await run("ioAction('gpio_write','gpio',{pin:3,level:1})");
  assert.equal(actions.length, 0);
  console.log('IO web: tab/keyboard navigation, form preservation, pin restrictions, strict HEX, config payloads, error recovery, ADC display and OTA guard PASS');
})().catch(error => {console.error(error); process.exitCode = 1;});
