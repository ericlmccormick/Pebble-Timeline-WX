'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/pkjs/index.js', 'utf8');
const context = {};
vm.createContext(context);
for (const [begin, end] of [
  ['function forecastClock', 'function applyWeatherPlusEnvelope'],
  ['function validHourlyRows', 'function fetchWeather'],
  ['function packForecastRows', 'function transmitHours'],
]) {
  vm.runInContext(source.slice(source.indexOf(begin), source.indexOf(end)), context);
}
const firstFall = Date.parse('2026-11-01T05:00:00Z') / 1000;
const firstSpring = Date.parse('2026-03-08T06:00:00Z') / 1000;
const fall = context.forecastClock('America/New_York', null, '12h');
assert.deepEqual([0, 1, 2].map(i => fall(firstFall + i * 3600)), ['1a', '1a*', '2a']);
const display24 = context.forecastClock('America/New_York', null, '24h');
assert.deepEqual([0, 1, 2].map(i => display24(firstFall + i * 3600)), ['01', '01*', '02']);
const spring = context.forecastClock('America/New_York', null, '24h');
assert.deepEqual([0, 1, 2].map(i => spring(firstSpring + i * 3600)), ['01', '03', '04']);
const offset = context.forecastClock('', -600, '12h');
assert.equal(offset(firstFall), '7p');
const rows = Array.from({length:48}, (_, i) => ({
  dt:firstFall + i * 3600,temp:15,wind:2,cond:1,night:0
}));
const duplicated = [rows[0], rows[0], ...rows.slice(1).reverse()];
assert.equal(context.validHourlyRows(duplicated, firstFall).length, 48);
assert.equal(context.validHourlyRows([rows[0], ...rows.slice(2)], firstFall).length, 1);
assert.deepEqual(Array.from(context.packForecastRows([{...rows[0],time:'11a*'}],0,1).slice(0,4)), [49,49,97,42]);
console.log('Forecast hourly ordering, fall-back and spring-forward regressions passed');
