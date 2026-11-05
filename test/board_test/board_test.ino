// 동작 시험을 UNO에서 그대로 돌려 USB로 결과를 보냄 (실제 보드의 16비트 int·float로 시험)
// 모터 핀은 꺼 둔 채로 두어서 바퀴는 돌지 않음. run_board_test.ps1이 헤더를 복사해서 올림
#include "tests.h"

void setup() {
  const uint8_t pwmPins[] = {3, 5, 6, 11};
  for (uint8_t p : pwmPins) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  Serial.begin(115200);
  delay(500);
  Serial.println(F("BOARD TEST START"));
  uint16_t failed = runAllTests();
  Serial.print(F("BOARD TEST DONE failed="));
  Serial.println(failed);
}

void loop() {}
