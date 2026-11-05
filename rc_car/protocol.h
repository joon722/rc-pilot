// 사이트 → 차 명령 읽기. 명령 한 개 = A5 | 종류 | a | b | XOR(종류, a, b)
#pragma once
#include <stdint.h>
#include "motion.h"

const uint8_t FRAME_HEADER = 0xA5;
const uint8_t TAG_KEYS = 0x4B;      // a = 눌린 키, b = C/Z/X 값
const uint8_t TAG_SETTING = 0x10;   // 0x10 + 세팅 번호, a·b = 16비트 값
const uint8_t TAG_REQUEST = 0x52;   // 세팅 값 보내 달라는 요청
const uint8_t TAG_DEFAULTS = 0x44;  // 세팅을 기본값으로

class FrameReader {
public:
  uint8_t tag = 0, a = 0, b = 0;

  // 바이트 하나를 넣음. 온전한 명령이 완성되면 true
  bool push(uint8_t value, uint32_t now) {
    if (used && now - lastByte > GAP_MS) used = 0;   // 끊긴 조각은 버림
    lastByte = now;
    if (used == 0) {
      if (value == FRAME_HEADER) used = 1;
      return false;
    }
    body[used - 1] = value;
    if (++used < 5) return false;
    used = 0;
    if ((body[0] ^ body[1] ^ body[2]) != body[3]) return false;
    tag = body[0];
    a = body[1];
    b = body[2];
    return true;
  }

  int16_t value() const { return (int16_t)((uint16_t)a | ((uint16_t)b << 8)); }

private:
  static const uint8_t GAP_MS = 10;   // 5바이트는 약 5ms에 다 옴
  uint8_t body[4];
  uint8_t used = 0;
  uint32_t lastByte = 0;
};

// C/Z/X 값: 위 2비트 = 어떤 키, 아래 6비트 = 누른 번호. 사이트가 같은 값을 계속 보내므로
// 값이 바뀌었을 때만 새로 눌린 것 (명령 하나가 빠져도 다음 명령으로 알 수 있음)
class ActionTracker {
public:
  Action update(uint8_t raw) {
    if (!synced) {   // 첫 명령은 기준으로만: 재시작 뒤 마지막 C가 또 실행되면 안 됨
      synced = true;
      last = raw;
      return ACTION_NONE;
    }
    if (raw == last) return ACTION_NONE;
    last = raw;
    return (Action)(raw >> 6);
  }

  void desync() { synced = false; }

private:
  uint8_t last = 0;
  bool synced = false;
};
