// RC PILOT: 눌린 키를 블루투스로 차에 보냄. 움직임 판단은 전부 차(rc_car/motion.h)가 함
// 명령 한 개 = A5 | 종류 | a | b | XOR(종류, a, b)

const HEADER = 0xA5;
const TAG_KEYS = 0x4B;      // a = 눌린 키, b = C/Z/X 값
const TAG_SETTING = 0x10;   // 종류 = 0x10 + 세팅 번호, a·b = 16비트 값
const TAG_REQUEST = 0x52;   // 세팅 값 보내 달라고 요청
const TAG_DEFAULTS = 0x44;  // 세팅을 기본값으로

const KEY_BITS = { ArrowUp: 1, ArrowDown: 2, ArrowLeft: 4, ArrowRight: 8, ShiftLeft: 16, ShiftRight: 16 };
const ACTION_TYPES = { C: 1, Z: 2, X: 3 };
const ACTION_CODES = { KeyC: 'C', KeyZ: 'Z', KeyX: 'X' };

// 차가 "S,..."로 보내는 순서와 같아야 함. 범위는 motion.h의 clampSettings와 같게
const SETTINGS = {
  trim: { id: 0, label: '직진 보정', min: -40, max: 40, step: 1, unit: '', help: '오른쪽으로 휘면 +, 왼쪽으로 휘면 −' },
  slowPct: { id: 1, label: 'Shift 속도', min: 30, max: 100, step: 1, unit: '%', help: '주차할 때 쓰는 속도' },
  uturnMs: { id: 2, label: 'C 회전 시간', min: 100, max: 1000, step: 5, unit: 'ms', help: '180°보다 많이 돌면 줄이기' },
  launchMs: { id: 3, label: '출발 가속 시간', min: 0, max: 1000, step: 10, unit: 'ms', help: '출발할 때 튀면 늘리고, 답답하면 줄이기' },
  nudgeMs: { id: 4, label: 'Z·X 세기', min: 10, max: 150, step: 5, unit: 'ms', help: '살짝 도는 양' },
  cornerInner: { id: 5, label: '코너 안쪽 바퀴', min: 30, max: 95, step: 1, unit: '%', help: '낮을수록 많이 꺾임' },
};
const SETTING_KEYS = Object.keys(SETTINGS);

function frame(tag, a, b) {
  a &= 255; b &= 255;
  return Uint8Array.of(HEADER, tag, a, b, tag ^ a ^ b);
}

function keyByte(keys) {
  let bits = 0;
  for (const code of keys) bits |= KEY_BITS[code] || 0;
  return bits;
}

// 위 2비트 = 어떤 키, 아래 6비트 = 누른 번호. 값이 바뀌면 차가 새로 눌린 걸로 알아챔
function nextAction(previous, name) {
  return (ACTION_TYPES[name] << 6) | (((previous & 63) + 1) & 63);
}

function settingFrame(id, value) {
  const v = value & 0xffff;
  return frame(TAG_SETTING | id, v & 255, v >> 8);
}

function parseLine(line) {
  const ack = /^A,(\d+)$/.exec(line);
  if (ack) return { type: 'ack', cuts: Number(ack[1]) };
  const set = /^S,(-?\d+),(\d+),(\d+),(\d+),(\d+),(\d+)$/.exec(line);
  if (!set) return null;
  const values = {};
  SETTING_KEYS.forEach((key, i) => { values[key] = Number(set[i + 1]); });
  return { type: 'settings', values };
}

function startUi() {
  const $ = id => document.getElementById(id);
  const K = new Set();
  let action = 0;
  let device = null, ch = null, connected = false;
  let busy = false, wantKeys = false, oneShots = [];
  let lastHeard = 0, lastAsked = 0, cuts = null, settingsLoaded = false, rx = '';

  function status(text, warn = false) {
    const s = $('status');
    s.textContent = text;
    s.classList.toggle('warn', warn);
  }

  // ---------------- 보내기: 한 번에 하나씩, 키는 항상 가장 최신 상태로 ----------------
  async function pump() {
    if (busy || !ch) return;
    busy = true;
    try {
      while (ch) {
        let bytes;
        if (oneShots.length) bytes = oneShots.shift();
        else if (wantKeys) { wantKeys = false; bytes = frame(TAG_KEYS, keyByte(K), action); }
        else break;
        if (ch.properties.writeWithoutResponse) await ch.writeValueWithoutResponse(bytes);
        else await ch.writeValue(bytes);
      }
    } catch (e) {
      status('전송 오류: ' + e.message, true);
    } finally {
      busy = false;
    }
  }
  function sendKeys() { wantKeys = true; drawKeys(); pump(); }
  function sendOnce(bytes) { oneShots.push(bytes); pump(); }

  // ---------------- 키보드 ----------------
  window.addEventListener('keydown', e => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.code === 'Escape') { K.clear(); sendKeys(); return; }
    const name = ACTION_CODES[e.code];
    if (name) {
      e.preventDefault();
      if (!e.repeat) { action = nextAction(action, name); flash(name); sendKeys(); }
      return;
    }
    if (e.code in KEY_BITS) { e.preventDefault(); K.add(e.code); sendKeys(); }
  });
  window.addEventListener('keyup', e => {
    if (e.code in KEY_BITS) { e.preventDefault(); K.delete(e.code); sendKeys(); }
  });
  // 창을 벗어나면 키가 눌린 채로 남지 않게 전부 놓음
  window.addEventListener('blur', () => { K.clear(); sendKeys(); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) { K.clear(); sendKeys(); } });

  function drawKeys() {
    document.querySelectorAll('[data-code]').forEach(el => {
      const on = el.dataset.code === 'Shift'
        ? K.has('ShiftLeft') || K.has('ShiftRight')
        : K.has(el.dataset.code);
      el.classList.toggle('on', on);
    });
  }
  function flash(name) {
    const el = $('key' + name);
    el.classList.add('on');
    setTimeout(() => el.classList.remove('on'), 180);
  }

  // ---------------- 세팅: 값은 차에 저장되고, 화면은 차가 알려 준 값만 보여 줌 ----------------
  function buildSettings() {
    const box = $('settings');
    for (const key of SETTING_KEYS) {
      const s = SETTINGS[key];
      const label = document.createElement('label');
      label.innerHTML = `<span class="name">${s.label}<b id="v_${key}">-</b></span>`;
      const range = document.createElement('input');
      Object.assign(range, { type: 'range', min: s.min, max: s.max, step: s.step, id: 'r_' + key, disabled: true });
      range.addEventListener('input', () => { $('v_' + key).textContent = range.value + s.unit; });
      range.addEventListener('change', () => { sendOnce(settingFrame(s.id, Number(range.value))); range.blur(); });
      const help = document.createElement('small');
      help.textContent = s.help;
      label.append(range, help);
      box.appendChild(label);
    }
  }
  function showSettings(values) {
    settingsLoaded = true;
    for (const key of SETTING_KEYS) {
      const r = $('r_' + key);
      r.value = values[key];
      r.disabled = false;
      $('v_' + key).textContent = values[key] + SETTINGS[key].unit;
    }
    $('defaults').disabled = false;
  }
  function lockSettings() {
    settingsLoaded = false;
    for (const key of SETTING_KEYS) $('r_' + key).disabled = true;
    $('defaults').disabled = true;
  }

  // ---------------- 블루투스 (HM-10: FFE0 / FFE1) ----------------
  function onNotify(e) {
    const v = e.target.value;
    for (let i = 0; i < v.byteLength; i++) {
      const c = v.getUint8(i);
      if (c !== 10) { if (rx.length < 60) rx += String.fromCharCode(c); continue; }
      const msg = parseLine(rx.trim());
      rx = '';
      if (!msg) continue;
      lastHeard = performance.now();
      if (msg.type === 'settings') showSettings(msg.values);
      if (msg.type === 'ack') {
        if (cuts !== null && msg.cuts > cuts) status(`방금 통신이 끊겨서 차가 멈췄어요 (끊김 ${msg.cuts}회)`, true);
        cuts = msg.cuts;
      }
      $('link').textContent = `차 응답 정상 · 끊김 ${cuts === null ? 0 : cuts}회`;
    }
  }
  function onDisconnected() {
    connected = false; ch = null; oneShots = []; busy = false;
    lockSettings(); drawConnect();
    status('블루투스 연결이 끊겼어요. 다시 연결하세요.', true);
  }
  async function connect() {
    if (connected) {
      K.clear(); sendKeys();
      try { device.gatt.disconnect(); } catch {}
      return;
    }
    if (!navigator.bluetooth) { status('이 브라우저는 블루투스를 못 써요. 노트북 크롬이나 엣지로 열어 주세요.', true); return; }
    try {
      status('목록에서 차의 HM-10을 고르세요.');
      device = await navigator.bluetooth.requestDevice({ acceptAllDevices: true, optionalServices: [0xffe0] });
      device.addEventListener('gattserverdisconnected', onDisconnected);
      status('연결하는 중...');
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(0xffe0);
      ch = await service.getCharacteristic(0xffe1);
      await ch.startNotifications();
      ch.addEventListener('characteristicvaluechanged', onNotify);
      connected = true; cuts = null; rx = ''; lastHeard = performance.now(); lastAsked = 0;
      drawConnect();
      status(`${device.name || 'HM-10'} 연결됨`);
    } catch (e) {
      status(e.name === 'NotFoundError' ? '선택을 취소했어요.' : '연결 실패: ' + e.message, true);
    }
  }
  function drawConnect() {
    $('connect').textContent = connected ? '연결 해제' : '차량 연결';
    $('connect').classList.toggle('on', connected);
    if (!connected) $('link').textContent = '연결 안 됨';
  }

  $('connect').addEventListener('click', () => { $('connect').blur(); connect(); });
  $('defaults').addEventListener('click', () => { $('defaults').blur(); sendOnce(frame(TAG_DEFAULTS, 0, 0)); });

  buildSettings();
  drawConnect();
  drawKeys();

  // 25번/초: 같은 키 상태도 계속 보냄 (0.35초 안 오면 차가 스스로 멈춤)
  setInterval(() => {
    if (!connected) return;
    const now = performance.now();
    if (!settingsLoaded && now - lastAsked > 500) { lastAsked = now; sendOnce(frame(TAG_REQUEST, 0, 0)); }
    if (now - lastHeard > 1500) $('link').textContent = '차 응답 없음 · 전원과 업로드를 확인하세요';
    sendKeys();
  }, 40);

  window.addEventListener('pagehide', () => {
    if (ch) { try { ch.writeValueWithoutResponse(frame(TAG_KEYS, 0, action)); } catch {} }
  });
}

if (typeof document !== 'undefined') startUi();
if (typeof module !== 'undefined') module.exports = { SETTINGS, frame, keyByte, nextAction, settingFrame, parseLine };
