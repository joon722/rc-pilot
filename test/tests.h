// 동작 판단(motion.h)과 통신(protocol.h) 시험. PC(host_main.cpp)와 UNO(board_test) 양쪽에서 돌아감
// 기대값은 코드를 쓰지 않고 손으로 계산한 값 (계산 과정은 각 시험의 주석)
#pragma once
#include "motion.h"
#include "protocol.h"

static uint16_t g_fail = 0, g_checks = 0;

#ifdef ARDUINO
  #define SAY(s) Serial.print(F(s))
  #define SAYN(v) Serial.print((long)(v))
#else
  #include <stdio.h>
  #define SAY(s) fputs(s, stdout)
  #define SAYN(v) printf("%ld", (long)(v))
#endif

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; SAY("  FAIL line "); SAYN(__LINE__); SAY(": " #c "\n"); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long _a = (a), _b = (b); if (_a != _b) { g_fail++; SAY("  FAIL line "); SAYN(__LINE__); SAY(": " #a " = "); SAYN(_a); SAY(", want "); SAYN(_b); SAY("\n"); } } while (0)
#define CHECK_NEAR(a, b, tol) do { g_checks++; long _a = (a), _b = (b); if (_a < _b - (tol) || _a > _b + (tol)) { g_fail++; SAY("  FAIL line "); SAYN(__LINE__); SAY(": " #a " = "); SAYN(_a); SAY(", want about "); SAYN(_b); SAY("\n"); } } while (0)
#define BEGIN(name) SAY("- " name "\n")

// 차 흉내: 5ms마다 동작 판단을 한 번 돌림. 세팅은 기본값이 바뀌어도 시험이 안 흔들리게 직접 넣음
struct Sim {
  Motion m;
  uint32_t now;
  int16_t l, r;
  Sim() : now(100000UL), l(1), r(1) {
    m.cfg.trim = 5;
    m.cfg.slowPct = 74;
    m.cfg.uturnMs = 310;
    m.cfg.launchMs = 500;
    m.cfg.nudgeMs = 40;
    m.cfg.cornerInner = 75;
  }
  void tick(uint8_t keys) { m.step(now, keys, l, r); now += 5; }
  void run(uint16_t ms, uint8_t keys) { for (uint16_t t = 0; t < ms; t += 5) tick(keys); }
};

// 끊기는 일: 출발 순간 최고 힘이 바로 들어가면 실패
static void testLaunchRisesFromLow() {
  BEGIN("launch starts low and rises to top");
  Sim s;
  s.tick(KEY_UP);   // 전원 켜고 맨 처음 판단에서도 튀면 안 됨
  CHECK(s.l > 0 && s.l <= Motion::LAUNCH_START_PWM);
  CHECK(s.r > 0 && s.r <= Motion::LAUNCH_START_PWM);
  int16_t prev = s.r;
  for (uint8_t i = 0; i < 50; i++) { s.tick(KEY_UP); CHECK(s.r >= prev); prev = s.r; }
  CHECK_NEAR(s.r, 192, 3);   // 250ms: 130 + 125 × 250/500 = 192.5
  s.run(300, KEY_UP);
  CHECK_EQ(s.l, 250);        // 보정 +5: 왼쪽 255 − 5 × (2 − 255/255) = 250
  CHECK_EQ(s.r, 255);
}

// 끊기는 일: 가속 시간 0일 때 나누기 오류나 값 깨짐
static void testLaunchZeroGoesStraightToTop() {
  BEGIN("launch 0ms reaches top at once");
  Sim s;
  s.m.cfg.launchMs = 0;
  s.tick(KEY_UP);
  s.tick(KEY_UP);
  CHECK_EQ(s.l, 250);
  CHECK_EQ(s.r, 255);
}

static void testReleaseStopsAtOnce() {
  BEGIN("release stops at once");
  Sim s;
  s.run(800, KEY_UP);
  s.tick(0);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
}

// 끊기는 일: 빙글빙글 원인. 제자리 회전 뒤 한쪽 바퀴만 먼저 밀면 실패
static void testPivotThenForwardStopsBothFirst() {
  BEGIN("pivot then forward: both wheels stop first");
  Sim s;
  s.run(300, KEY_RIGHT);
  CHECK(s.l > 0);
  CHECK_EQ(s.r, -s.l);
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_UP);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  s.tick(KEY_UP);
  CHECK(s.l > 0 && s.l <= Motion::LAUNCH_START_PWM);
  CHECK(s.r > 0 && s.r <= Motion::LAUNCH_START_PWM);
}

// 끊기는 일: 키를 뗐다가 잠깐 뒤 ↑를 눌러도 도는 힘이 남은 채 출발하면 실패
static void testPivotReleaseThenForwardStillWaits() {
  BEGIN("pivot, release, forward: settle still applies");
  Sim s;
  s.run(300, KEY_LEFT);
  s.run(50, 0);   // 50ms 쉼
  s.tick(KEY_UP);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
  s.run(Motion::SETTLE_MS, KEY_UP);
  CHECK(s.l > 0);
  CHECK(s.r > 0);
}

static void testPivotDirections() {
  BEGIN("pivot directions and opposite pivot settles");
  Sim s;
  s.tick(KEY_LEFT);
  CHECK(s.l < 0);
  CHECK(s.r > 0);
  s.run(100, KEY_LEFT);
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_RIGHT);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  s.tick(KEY_RIGHT);
  CHECK(s.l > 0);
  CHECK(s.r < 0);
}

// 끊기는 일: 예전 코드처럼 달리는 중 Z/X가 바퀴를 거꾸로 돌리거나 브레이크를 걸면 실패
static void testRunningNudgeOnlyTrims() {
  BEGIN("Z while running only trims left wheel");
  Sim s;
  s.run(800, KEY_UP);
  s.m.press(ACTION_Z);
  for (uint8_t i = 0; i < 8; i++) {   // 40ms = 8번
    s.tick(KEY_UP);
    CHECK_NEAR(s.l, 174, 1);   // 255 × 0.7 = 178.5, 보정 −5 → 173.5
    CHECK_EQ(s.r, 255);
  }
  s.tick(KEY_UP);
  CHECK_EQ(s.l, 250);
  CHECK_EQ(s.r, 255);
}

static void testRunningNudgeRight() {
  BEGIN("X while running only trims right wheel");
  Sim s;
  s.run(800, KEY_UP);
  s.m.press(ACTION_X);
  s.tick(KEY_UP);
  CHECK_EQ(s.l, 250);
  CHECK_NEAR(s.r, 178, 1);     // 255 × 0.7 = 178.5
  s.run(40, KEY_UP);
  CHECK_EQ(s.r, 255);
}

static void testStoppedNudgeIsShortPivot() {
  BEGIN("Z and X while stopped are short pivots");
  Sim s;
  s.m.press(ACTION_Z);
  for (uint8_t i = 0; i < 8; i++) {
    s.tick(0);
    CHECK(s.l < 0);
    CHECK_EQ(s.r, -s.l);
  }
  s.tick(0);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
  s.run(Motion::SETTLE_MS, 0);
  s.m.press(ACTION_X);
  s.tick(0);
  CHECK(s.l > 0);
  CHECK_EQ(s.r, -s.l);
}

static void testUturnFromStop() {
  BEGIN("C from stop turns right for the set time");
  Sim s;
  s.m.press(ACTION_C);
  for (uint8_t i = 0; i < 62; i++) {   // 310ms ÷ 5 = 62번
    s.tick(0);
    CHECK_EQ(s.l, Motion::UTURN_PWM);
    CHECK_EQ(s.r, -Motion::UTURN_PWM);
  }
  s.tick(0);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
}

// 끊기는 일: 달리다 바로 돌면 각도가 매번 달라짐
static void testUturnWhileRunningStopsFirst() {
  BEGIN("C while running: stop, turn, stop, launch");
  Sim s;
  s.run(800, KEY_UP);
  s.m.press(ACTION_C);
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_UP);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  for (uint8_t i = 0; i < 62; i++) {
    s.tick(KEY_UP);
    CHECK_EQ(s.l, Motion::UTURN_PWM);
    CHECK_EQ(s.r, -Motion::UTURN_PWM);
  }
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_UP);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  s.tick(KEY_UP);
  CHECK(s.l > 0 && s.l <= Motion::LAUNCH_START_PWM);
  CHECK(s.r > 0 && s.r <= Motion::LAUNCH_START_PWM);
}

// 끊기는 일: 도는 중 다른 키가 먹으면 180°가 안 나옴
static void testUturnIgnoresKeysAndRepeat() {
  BEGIN("C ignores other keys and a second C");
  Sim s;
  s.m.press(ACTION_C);
  s.run(150, 0);
  s.m.press(ACTION_C);
  s.m.press(ACTION_Z);
  for (uint8_t i = 0; i < 32; i++) {   // 남은 160ms = 32번
    s.tick(KEY_LEFT | KEY_UP);
    CHECK_EQ(s.l, Motion::UTURN_PWM);
    CHECK_EQ(s.r, -Motion::UTURN_PWM);
  }
  s.tick(0);
  CHECK_EQ(s.l, 0);
  s.run(600, 0);        // 두 번째 C가 나중에 실행되면 여기서 돌게 됨
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
}

// 끊기는 일: 코너에서 안쪽 바퀴가 거꾸로 돌거나 멈추면 미끄러짐
static void testCornerRight() {
  BEGIN("corner right never reverses a wheel");
  Sim s;
  s.run(800, KEY_UP);
  for (uint8_t i = 0; i < 60; i++) {
    s.tick(KEY_UP | KEY_RIGHT);
    CHECK(s.l > 0);
    CHECK(s.r > 0);
  }
  // 바깥 255 × 0.85 = 216.75, 안쪽 × 0.75 = 162.56, 보정 5 × (2 − 216.75/255) = 5.75
  CHECK_NEAR(s.l, 211, 1);
  CHECK_NEAR(s.r, 163, 1);
}

static void testCornerLeft() {
  BEGIN("corner left mirrors");
  Sim s;
  s.run(800, KEY_UP);
  s.run(300, KEY_UP | KEY_LEFT);
  CHECK_NEAR(s.l, 157, 1);    // 162.56 − 5.75
  CHECK_NEAR(s.r, 217, 1);    // 216.75
}

// 끊기는 일: 키를 누르자마자 끝까지 꺾이면 살짝 눌러도 많이 돎
static void testCornerStartsGently() {
  BEGIN("corner turns in gradually and straightens on release");
  Sim s;
  s.run(800, KEY_UP);
  s.tick(KEY_UP | KEY_RIGHT);
  CHECK(s.r > 210);
  s.run(300, KEY_UP | KEY_RIGHT);
  CHECK(s.r < 170);
  s.run(150, KEY_UP);
  CHECK_EQ(s.l, 250);
  CHECK_EQ(s.r, 255);
}

// 끊기는 일: ↑를 잠깐 떼었다 누를 때마다 0.2초씩 멈추면 조종이 답답함 (같은 방향은 위험하지 않음)
static void testRepressForwardIsImmediate() {
  BEGIN("re-press forward restarts at once");
  Sim s;
  s.run(800, KEY_UP);
  s.run(30, 0);
  s.tick(KEY_UP);
  CHECK(s.l > 0);
  CHECK(s.r > 0);
}

// 끊기는 일: 달리다 ↑만 떼고 ←를 잡으면 바로 제자리 회전이 들어가 차가 튐
static void testForwardToPivotWaits() {
  BEGIN("forward to pivot waits for settle");
  Sim s;
  s.run(800, KEY_UP);
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_LEFT);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  s.tick(KEY_LEFT);
  CHECK(s.l < 0);
  CHECK(s.r > 0);
}

static void testShiftSlows() {
  BEGIN("shift holds the slow speed");
  Sim s;
  s.run(800, KEY_UP | KEY_SHIFT);
  // 255 × 74% = 188.7, 보정 5 × (2 − 188.7/255) = 6.3
  CHECK_NEAR(s.r, 189, 1);
  CHECK_NEAR(s.l, 182, 1);
  s.run(800, KEY_UP);
  CHECK_EQ(s.r, 255);
  s.run(800, KEY_UP | KEY_SHIFT);   // 달리다 Shift: 내려오고, 바퀴는 안 멈춤
  CHECK_NEAR(s.r, 189, 1);
}

static void testShiftBelowStartNeverJumpsAbove() {
  BEGIN("shift below launch start stays low");
  Sim s;
  s.m.cfg.slowPct = 40;   // 255 × 40% = 102
  s.tick(KEY_UP | KEY_SHIFT);
  CHECK(s.r <= 102);
}

static void testNegativeTrimCutsRight() {
  BEGIN("negative trim cuts the right wheel");
  Sim s;
  s.m.cfg.trim = -5;
  s.run(800, KEY_UP);
  CHECK_EQ(s.l, 255);
  CHECK_EQ(s.r, 250);
}

// 끊기는 일: 보정이 안쪽 바퀴 힘보다 크면 값이 음수가 되어 달리는 중 바퀴가 거꾸로 돎
// 힘 76.5(30%), 안쪽 0.85 × 0.3 = 19.5, 보정 40 × (2 − 65/255) = 69.8 → 왼쪽 음수
static void testExtremeSettingsNeverReverse() {
  BEGIN("extreme settings never reverse a wheel while driving");
  Sim s;
  s.m.cfg.trim = 40;
  s.m.cfg.slowPct = 30;
  s.m.cfg.cornerInner = 30;
  s.m.cfg.launchMs = 0;
  s.run(100, KEY_UP | KEY_SHIFT);
  for (uint8_t i = 0; i < 80; i++) {
    s.tick(KEY_UP | KEY_SHIFT | KEY_LEFT);
    CHECK(s.l >= 0);
    CHECK(s.r >= 0);
  }
  s.m.cfg.trim = -40;
  for (uint8_t i = 0; i < 80; i++) {
    s.tick(KEY_UP | KEY_SHIFT | KEY_RIGHT);
    CHECK(s.l >= 0);
    CHECK(s.r >= 0);
  }
}

static void testDownBrakesThenReverses() {
  BEGIN("down brakes then reverses");
  Sim s;
  s.run(800, KEY_UP);
  for (uint8_t i = 0; i < Motion::SETTLE_MS / 5; i++) {
    s.tick(KEY_DOWN);
    CHECK_EQ(s.l, 0);
    CHECK_EQ(s.r, 0);
  }
  s.run(800, KEY_DOWN);
  CHECK(s.l < 0);
  CHECK(s.r < 0);
  CHECK_NEAR(s.r, -Motion::REVERSE_PWM, 1);
}

static void testUpAndDownTogetherStops() {
  BEGIN("up and down together stop");
  Sim s;
  s.run(800, KEY_UP);
  s.tick(KEY_UP | KEY_DOWN);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
}

// 끊기는 일: 통신이 끊겼는데 돌던 회전이 계속되거나, 다시 붙었을 때 이어서 돌면 위험
static void testResetStopsEverything() {
  BEGIN("reset cancels rotation and pending");
  Sim s;
  s.m.press(ACTION_C);
  s.run(100, 0);
  s.m.reset(s.now);
  s.tick(0);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);
  s.run(1000, 0);
  CHECK_EQ(s.l, 0);
  CHECK_EQ(s.r, 0);

  Sim t;
  t.run(800, KEY_UP);
  t.m.press(ACTION_C);   // 멈추기를 기다리는 C도 취소
  t.tick(KEY_UP);
  t.m.reset(t.now);
  t.run(1000, 0);
  CHECK_EQ(t.l, 0);
  CHECK_EQ(t.r, 0);
}

// 끊기는 일: EEPROM에 예전 프로그램의 쓰레기 값이 있으면 차가 이상하게 움직임
static void testClampSettings() {
  BEGIN("settings are clamped to safe ranges");
  Settings c;
  c.trim = 100; c.slowPct = 5; c.uturnMs = 50; c.launchMs = 5000; c.nudgeMs = 1; c.cornerInner = 10;
  clampSettings(c);
  CHECK_EQ(c.trim, 40);
  CHECK_EQ(c.slowPct, 30);
  CHECK_EQ(c.uturnMs, 100);
  CHECK_EQ(c.launchMs, 1000);
  CHECK_EQ(c.nudgeMs, 10);
  CHECK_EQ(c.cornerInner, 30);
  c.trim = -100; c.slowPct = 200; c.uturnMs = 9000; c.launchMs = 0; c.nudgeMs = 255; c.cornerInner = 99;
  clampSettings(c);
  CHECK_EQ(c.trim, -40);
  CHECK_EQ(c.slowPct, 100);
  CHECK_EQ(c.uturnMs, 1000);
  CHECK_EQ(c.launchMs, 0);
  CHECK_EQ(c.nudgeMs, 150);
  CHECK_EQ(c.cornerInner, 95);
}

static void testApplySetting() {
  BEGIN("site changes one setting by id");
  Settings c = defaultSettings();
  CHECK(applySetting(c, SET_UTURN, 320));
  CHECK_EQ(c.uturnMs, 320);
  CHECK(applySetting(c, SET_TRIM, -7));
  CHECK_EQ(c.trim, -7);
  CHECK(applySetting(c, SET_SLOW, 5000));   // 범위 밖은 끝값으로
  CHECK_EQ(c.slowPct, 100);
  CHECK(!applySetting(c, 99, 1));           // 없는 번호는 무시
}

// 사이트 쪽 시험과 같은 바이트: A5 4B 11 41 1B (4B xor 11 xor 41 = 1B)
static void feed(FrameReader& f, const uint8_t* b, uint8_t n, uint32_t& now, bool& done) {
  done = false;
  for (uint8_t i = 0; i < n; i++) { if (f.push(b[i], now)) done = true; now += 1; }
}

static void testFrameGood() {
  BEGIN("frame: good one decodes");
  FrameReader f;
  const uint8_t good[] = {0xA5, 0x4B, 0x11, 0x41, 0x1B};
  uint32_t now = 1000; bool done;
  feed(f, good, 5, now, done);
  CHECK(done);
  CHECK_EQ(f.tag, 0x4B);
  CHECK_EQ(f.a, 0x11);
  CHECK_EQ(f.b, 0x41);
}

static void testFrameBadChecksum() {
  BEGIN("frame: bad checksum is dropped, next one still works");
  FrameReader f;
  const uint8_t bad[] = {0xA5, 0x4B, 0x11, 0x41, 0x1C};
  const uint8_t good[] = {0xA5, 0x4B, 0x01, 0x00, 0x4A};   // 4B xor 01 xor 00 = 4A
  uint32_t now = 1000; bool done;
  feed(f, bad, 5, now, done);
  CHECK(!done);
  feed(f, good, 5, now, done);
  CHECK(done);
  CHECK_EQ(f.a, 0x01);
}

// 끊기는 일: 조각만 온 명령 뒤에 다음 명령이 이어 붙어 잘못 읽힘
static void testFrameGapDropsPartial() {
  BEGIN("frame: a long gap drops the partial frame");
  FrameReader f;
  const uint8_t part[] = {0xA5, 0x4B, 0x11};
  const uint8_t good[] = {0xA5, 0x4B, 0x01, 0x00, 0x4A};
  uint32_t now = 1000; bool done;
  feed(f, part, 3, now, done);
  now += 50;
  feed(f, good, 5, now, done);
  CHECK(done);
  CHECK_EQ(f.a, 0x01);
}

static void testFrameResyncAfterGarbage() {
  BEGIN("frame: garbage before a frame is skipped");
  FrameReader f;
  const uint8_t junk[] = {0x00, 0x7F, 0x4B};
  const uint8_t good[] = {0xA5, 0x4B, 0x01, 0x00, 0x4A};
  uint32_t now = 1000; bool done;
  feed(f, junk, 3, now, done);
  CHECK(!done);
  feed(f, good, 5, now, done);
  CHECK(done);
}

// 사이트 쪽 시험과 같은 바이트. 끊기는 일: 음수 세팅(직진 보정)이 큰 양수로 읽힘
static void testFrameSettingValue() {
  BEGIN("frame: setting value reads 16 bits with sign");
  FrameReader f;
  const uint8_t turn[] = {0xA5, 0x12, 0x40, 0x01, 0x53};
  const uint8_t trim[] = {0xA5, 0x10, 0xF9, 0xFF, 0x16};
  uint32_t now = 1000; bool done;
  feed(f, turn, 5, now, done);
  CHECK(done);
  CHECK_EQ(f.value(), 320);
  feed(f, trim, 5, now, done);
  CHECK(done);
  CHECK_EQ(f.value(), -7);
}

// 끊기는 일: 차가 재시작하거나 통신이 다시 붙을 때 마지막 C가 또 실행되면 혼자 돎
static void testActionTracker() {
  BEGIN("action: only new presses count, first frame only syncs");
  ActionTracker a;
  CHECK_EQ(a.update(0x41), ACTION_NONE);   // 첫 명령은 기준으로만
  CHECK_EQ(a.update(0x41), ACTION_NONE);   // 같은 값 반복
  CHECK_EQ(a.update(0x42), ACTION_C);      // 01 000010 = C, 2번째
  CHECK_EQ(a.update(0x42), ACTION_NONE);
  CHECK_EQ(a.update(0x83), ACTION_Z);      // 10 000011 = Z
  CHECK_EQ(a.update(0xC4), ACTION_X);      // 11 000100 = X
  CHECK_EQ(a.update(0x05), ACTION_NONE);   // 00 = 눌린 것 없음
  a.desync();
  CHECK_EQ(a.update(0xC9), ACTION_NONE);   // 다시 붙은 첫 명령도 기준으로만
  CHECK_EQ(a.update(0xCA), ACTION_X);
}

static uint16_t runAllTests() {
  testLaunchRisesFromLow();
  testLaunchZeroGoesStraightToTop();
  testReleaseStopsAtOnce();
  testPivotThenForwardStopsBothFirst();
  testPivotReleaseThenForwardStillWaits();
  testPivotDirections();
  testRunningNudgeOnlyTrims();
  testRunningNudgeRight();
  testStoppedNudgeIsShortPivot();
  testUturnFromStop();
  testUturnWhileRunningStopsFirst();
  testUturnIgnoresKeysAndRepeat();
  testCornerRight();
  testCornerLeft();
  testCornerStartsGently();
  testRepressForwardIsImmediate();
  testForwardToPivotWaits();
  testShiftSlows();
  testShiftBelowStartNeverJumpsAbove();
  testNegativeTrimCutsRight();
  testExtremeSettingsNeverReverse();
  testDownBrakesThenReverses();
  testUpAndDownTogetherStops();
  testResetStopsEverything();
  testClampSettings();
  testApplySetting();
  testFrameGood();
  testFrameBadChecksum();
  testFrameGapDropsPartial();
  testFrameResyncAfterGarbage();
  testFrameSettingValue();
  testActionTracker();
  SAY("checks "); SAYN(g_checks); SAY(", failed "); SAYN(g_fail); SAY("\n");
  return g_fail;
}
