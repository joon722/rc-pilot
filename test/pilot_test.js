// 사이트 통신 부분 시험: node --test test/pilot_test.js
// 기대 바이트는 손으로 계산한 값이고, 차 쪽 시험(tests.h)과 같은 값을 씀
const test = require('node:test');
const assert = require('node:assert');
const p = require('../pilot.js');

const hex = bytes => [...bytes].map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');

test('명령 한 개는 머리 + 내용 3바이트 + XOR', () => {
  assert.strictEqual(hex(p.frame(0x4B, 0x11, 0x41)), 'A5 4B 11 41 1B');
});

test('눌린 키가 비트로 바뀜', () => {
  assert.strictEqual(p.keyByte(new Set()), 0);
  assert.strictEqual(p.keyByte(new Set(['ArrowUp'])), 1);
  assert.strictEqual(p.keyByte(new Set(['ArrowDown'])), 2);
  assert.strictEqual(p.keyByte(new Set(['ArrowLeft'])), 4);
  assert.strictEqual(p.keyByte(new Set(['ArrowRight'])), 8);
  assert.strictEqual(p.keyByte(new Set(['ArrowUp', 'ShiftLeft'])), 0x11);
  assert.strictEqual(p.keyByte(new Set(['ArrowUp', 'ShiftRight'])), 0x11);
});

test('C·Z·X는 누를 때마다 값이 바뀌어서, 차가 새로 눌린 걸 알 수 있음', () => {
  assert.strictEqual(p.nextAction(0, 'C'), 0x41);      // 01 000001
  assert.strictEqual(p.nextAction(0x41, 'Z'), 0x82);   // 10 000010
  assert.strictEqual(p.nextAction(0x82, 'X'), 0xC3);   // 11 000011
  assert.strictEqual(p.nextAction(0x41, 'C'), 0x42);   // 같은 키를 또 눌러도 값이 바뀜
  assert.strictEqual(p.nextAction(0x7F, 'C'), 0x40);   // 번호 63 다음은 0
});

test('세팅 명령은 번호와 16비트 값을 담음', () => {
  assert.strictEqual(hex(p.settingFrame(p.SETTINGS.uturnMs.id, 320)), 'A5 12 40 01 53');
  assert.strictEqual(hex(p.settingFrame(p.SETTINGS.trim.id, -7)), 'A5 10 F9 FF 16');
});

test('차가 보낸 줄을 읽음', () => {
  assert.deepStrictEqual(p.parseLine('A,3'), { type: 'ack', cuts: 3 });
  assert.deepStrictEqual(p.parseLine('S,5,74,310,500,40,75'), {
    type: 'settings',
    values: { trim: 5, slowPct: 74, uturnMs: 310, launchMs: 500, nudgeMs: 40, cornerInner: 75 },
  });
  assert.deepStrictEqual(p.parseLine('S,-7,74,310,500,40,75').values.trim, -7);
});

test('깨진 줄은 무시함', () => {
  assert.strictEqual(p.parseLine('S,1,2'), null);
  assert.strictEqual(p.parseLine('A,'), null);
  assert.strictEqual(p.parseLine('A,x'), null);
  assert.strictEqual(p.parseLine('아무말'), null);
  assert.strictEqual(p.parseLine(''), null);
});
