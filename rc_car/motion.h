// 눌린 키를 두 바퀴 값(-255~255)으로 바꾸는 판단. 아두이노 함수를 안 써서 PC에서도 시험됨 (test/tests.h)
//
// 지키는 규칙
//   1) 바퀴 방향이 바뀌는 동작(제자리 회전 ↔ 직진 ↔ 후진)은 반드시 두 바퀴를 같이 세운 뒤 시작
//   2) 달리는 중에는 바퀴를 거꾸로 돌리지 않음 (코너, Z/X 모두 한쪽 힘만 줄임)
//   3) C·Z·X의 시간은 차의 시계로 잼 (블루투스 지연과 상관없음)
#pragma once
#include <stdint.h>

enum Keys : uint8_t { KEY_UP = 1, KEY_DOWN = 2, KEY_LEFT = 4, KEY_RIGHT = 8, KEY_SHIFT = 16 };
enum Action : uint8_t { ACTION_NONE = 0, ACTION_C = 1, ACTION_Z = 2, ACTION_X = 3 };
enum SettingId : uint8_t { SET_TRIM = 0, SET_SLOW = 1, SET_UTURN = 2, SET_LAUNCH = 3, SET_NUDGE = 4, SET_CORNER = 5 };

// 사이트 슬라이더로 바꾸고 EEPROM에 저장되는 값
struct Settings {
  int8_t trim;          // 직진 보정. +면 왼쪽 바퀴를 줄임 (오른쪽으로 휠 때 +)
  uint8_t slowPct;      // Shift 속도 (최고 힘의 %)
  uint16_t uturnMs;     // C 회전 시간
  uint16_t launchMs;    // 출발 가속 시간
  uint8_t nudgeMs;      // Z/X 시간
  uint8_t cornerInner;  // 코너 안쪽 바퀴 (바깥 바퀴의 %)
};

inline Settings defaultSettings() {
  Settings s;
  s.trim = 5;           // 11/5 실측 250/255
  s.slowPct = 74;       // 힘 189 = 이전 사이트 Shift 60%와 같은 힘
  s.uturnMs = 310;      // 11/5 영상 실측값 (이전 사이트의 C와 같은 힘)
  s.launchMs = 500;     // 출발 튐을 잡는 쪽으로 넉넉하게. 튐이 없으면 줄이기
  s.nudgeMs = 40;       // 실차 확인 전
  s.cornerInner = 75;   // 이전 사이트 코너(바깥 230 / 안쪽 174)와 같은 비율
  return s;
}

inline long clampTo(long value, long low, long high) {
  return value < low ? low : value > high ? high : value;
}

// 범위를 바꾸면 pilot.js의 SETTINGS도 같이 바꾸기
inline void clampSettings(Settings& s) {
  s.trim = clampTo(s.trim, -40, 40);
  s.slowPct = clampTo(s.slowPct, 30, 100);
  s.uturnMs = clampTo(s.uturnMs, 100, 1000);
  s.launchMs = clampTo(s.launchMs, 0, 1000);
  s.nudgeMs = clampTo(s.nudgeMs, 10, 150);
  s.cornerInner = clampTo(s.cornerInner, 30, 95);
}

inline bool applySetting(Settings& s, uint8_t id, int16_t value) {
  switch (id) {
    case SET_TRIM: s.trim = clampTo(value, -128, 127); break;
    case SET_SLOW: s.slowPct = clampTo(value, 0, 255); break;
    case SET_UTURN: s.uturnMs = clampTo(value, 0, 32767); break;
    case SET_LAUNCH: s.launchMs = clampTo(value, 0, 32767); break;
    case SET_NUDGE: s.nudgeMs = clampTo(value, 0, 255); break;
    case SET_CORNER: s.cornerInner = clampTo(value, 0, 255); break;
    default: return false;
  }
  clampSettings(s);
  return true;
}

class Motion {
public:
  static const int16_t TOP_PWM = 255;
  static const int16_t LAUNCH_START_PWM = 130;  // 출발 첫 힘. 너무 낮으면 한쪽 바퀴가 먼저 구르며 틀어짐
  static const int16_t PIVOT_PWM = 165;         // ←→ 제자리 회전 (이전 사이트 45%)
  static const int16_t UTURN_PWM = 200;         // C 회전 (이전 사이트 65%)
  static const int16_t NUDGE_PWM = 170;         // 멈춰 있을 때 Z/X (이전 사이트 50%)
  static const int16_t REVERSE_PWM = 181;       // 후진 (이전 사이트 55%)
  static const uint16_t SETTLE_MS = 200;        // 방향이 바뀔 때 두 바퀴를 같이 세우는 시간
  static const uint8_t STEER_IN_MS = 140;       // 코너가 끝까지 꺾이는 시간
  static const uint8_t STEER_OUT_MS = 100;      // 키를 떼고 펴지는 시간

  Settings cfg = defaultSettings();

  // C·Z·X가 한 번 눌림. 회전이 도는 중이면 무시
  void press(Action action) {
    if (active != TIMED) pending = action;
  }

  // 통신이 끊김: 하던 일과 기다리던 일을 모두 취소
  void reset(uint32_t now) {
    if (active != NONE) leave(now);
    pending = ACTION_NONE;
  }

  void step(uint32_t now, uint8_t keys, int16_t& left, int16_t& right) {
    uint32_t dt = now - lastStep;
    lastStep = now;
    left = right = 0;

    if (active == TIMED) {
      if (isBefore(now, timedEnd)) {
        left = timedLeft;
        right = -timedLeft;
        return;
      }
      leave(now);
    }

    Kind want = wantedKind(keys);

    // 달리는 중 Z/X: 멈추지 않고 한쪽 힘만 잠깐 줄임
    if ((pending == ACTION_Z || pending == ACTION_X) && (active == FORWARD || active == REVERSE) && want == active) {
      nudgeSide = pending == ACTION_Z ? -1 : 1;
      nudgeUntil = now + cfg.nudgeMs;
      nudging = true;
      pending = ACTION_NONE;
    }

    // C, 멈춰 있을 때 Z/X: 멈춘 뒤에 제자리에서 정해진 시간만큼 돎
    if (pending != ACTION_NONE) {
      if (active != NONE) leave(now);
      if (isBefore(now, settleUntil)) return;
      startTimed(now);
      pending = ACTION_NONE;
      left = timedLeft;
      right = -timedLeft;
      return;
    }

    if (want != active) {
      if (active != NONE) leave(now);
      // 같은 방향으로 다시 가는 건 위험하지 않아서 안 기다림
      if (want != NONE && (want == lastKind || !isBefore(now, settleUntil))) {
        begin(want, keys);
        dt = 0;
      }
    }

    switch (active) {
      case FORWARD:
      case REVERSE:
        runWheels(now, dt, keys, left, right);
        break;
      case PIVOT_LEFT:
        left = -PIVOT_PWM;
        right = PIVOT_PWM;
        break;
      case PIVOT_RIGHT:
        left = PIVOT_PWM;
        right = -PIVOT_PWM;
        break;
      default:
        break;
    }
  }

private:
  enum Kind : uint8_t { NONE, FORWARD, REVERSE, PIVOT_LEFT, PIVOT_RIGHT, TIMED };

  Kind active = NONE, lastKind = NONE;
  Action pending = ACTION_NONE;
  uint32_t lastStep = 0, settleUntil = 0, timedEnd = 0, nudgeUntil = 0;
  int16_t timedLeft = 0;
  int8_t nudgeSide = 0;
  bool nudging = false;
  float speed = 0, steer = 0;

  static bool isBefore(uint32_t now, uint32_t when) { return (int32_t)(now - when) < 0; }

  static float approach(float value, float target, float change) {
    if (value < target) return value + change > target ? target : value + change;
    return value - change < target ? target : value - change;
  }

  static Kind wantedKind(uint8_t keys) {
    bool up = keys & KEY_UP, down = keys & KEY_DOWN, l = keys & KEY_LEFT, r = keys & KEY_RIGHT;
    if (up != down) return up ? FORWARD : REVERSE;
    if (up) return NONE;   // ↑↓ 같이
    if (l != r) return l ? PIVOT_LEFT : PIVOT_RIGHT;
    return NONE;
  }

  float targetSpeed(uint8_t keys) const {
    if (active == REVERSE) return REVERSE_PWM;
    return (keys & KEY_SHIFT) ? TOP_PWM * (cfg.slowPct / 100.0f) : TOP_PWM;
  }

  void begin(Kind kind, uint8_t keys) {
    active = kind;
    float target = targetSpeed(keys);
    speed = target < LAUNCH_START_PWM ? target : LAUNCH_START_PWM;
    steer = 0;
    nudging = false;
  }

  void leave(uint32_t now) {
    lastKind = active;
    active = NONE;
    settleUntil = now + SETTLE_MS;
  }

  void startTimed(uint32_t now) {
    active = TIMED;
    if (pending == ACTION_C) {
      timedLeft = UTURN_PWM;   // 오른쪽으로 돎 (코스 그림과 같은 방향)
      timedEnd = now + cfg.uturnMs;
    } else {
      timedLeft = pending == ACTION_X ? NUDGE_PWM : -NUDGE_PWM;
      timedEnd = now + cfg.nudgeMs;
    }
  }

  void runWheels(uint32_t now, uint32_t dt, uint8_t keys, int16_t& left, int16_t& right) {
    // 힘: 가속 시간에 맞춰 올리고, 내려갈 때(Shift)는 두 배로 빠르게
    float target = targetSpeed(keys);
    float change = (float)(TOP_PWM - LAUNCH_START_PWM) / (cfg.launchMs ? cfg.launchMs : 1) * dt;
    speed = approach(speed, target, speed > target ? 2 * change : change);

    // 코너: 키를 누르면 천천히 꺾이고, 떼면 더 빨리 펴짐
    int8_t turn = ((keys & KEY_RIGHT) ? 1 : 0) - ((keys & KEY_LEFT) ? 1 : 0);
    steer = approach(steer, turn, (float)dt / (turn ? STEER_IN_MS : STEER_OUT_MS));
    float amount = steer < 0 ? -steer : steer;
    float outer = speed * (1 - 0.15f * amount);   // 코너에서는 바깥 바퀴도 85%까지 줄임
    float inner = outer * (1 - (1 - cfg.cornerInner / 100.0f) * amount);
    float l = steer > 0 ? outer : inner;
    float r = steer > 0 ? inner : outer;

    // 달리는 중 Z/X
    if (nudging) {
      if (isBefore(now, nudgeUntil)) {
        if (nudgeSide < 0) l *= 0.7f;
        else r *= 0.7f;
      } else {
        nudging = false;
      }
    }

    // 직진 보정: 느릴수록 차이가 커서 두 배까지 키움 (실측 250/255, 173/180)
    float cut = cfg.trim * (2 - (l > r ? l : r) / TOP_PWM);
    if (cut > 0) l -= cut;
    else r += cut;
    if (l < 0) l = 0;   // 보정 때문에 달리는 중 바퀴가 거꾸로 돌면 안 됨
    if (r < 0) r = 0;

    int16_t sign = active == FORWARD ? 1 : -1;
    left = sign * (int16_t)(l + 0.5f);
    right = sign * (int16_t)(r + 0.5f);
  }
};
