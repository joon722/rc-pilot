// ============================================================
// RC PILOT 차 코드 · UNO R3 + DFRobot 쿼드 모터 드라이버 쉴드 [DRI0039] + HM-10
// 사이트(index.html)는 눌린 키만 보내고, 어떻게 움직일지는 여기(motion.h)서 정함
//
// 배선 (mission2_kart_dri0039와 같음)
//   HM-10: TXD → A0, A5 → 100Ω → RXD, 그 점에서 200Ω → GND, VCC → 5V, GND 공통
//   왼쪽 모터 → M3 단자, 오른쪽 모터 → M4 단자
//   건전지(AA 8개, 최대 13.5V) → 쉴드 모터 전원 단자
//
// 통신
//   사이트 → 차: A5 | 종류 | a | b | XOR  (protocol.h, 25번/초)
//   차 → 사이트: "A,끊김횟수\n" (0.5초마다), "S,세팅값들\n" (요청하거나 바꿨을 때)
// ============================================================
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include "motion.h"
#include "protocol.h"

const uint8_t BT_RX_PIN = A0, BT_TX_PIN = A5;
SoftwareSerial radio(BT_RX_PIN, BT_TX_PIN);

const uint8_t PWM_LEFT = 5, DIR_LEFT = 8;    // M3
const uint8_t PWM_RIGHT = 6, DIR_RIGHT = 7;  // M4
const uint8_t UNUSED_PWM[] = {3, 11};        // M1, M2 (안 씀 → 꺼 둠)
const uint8_t UNUSED_DIR[] = {4, 12};

// 앞으로 갈 때 방향 핀 값. 11/5 실측(mission2_kart_dri0039의 INVERT 값을 풀어 쓴 것).
// ↑를 눌렀는데 뒤로 도는 바퀴가 있으면 그쪽만 HIGH로 바꾸기
const uint8_t LEFT_FORWARD = LOW;
const uint8_t RIGHT_FORWARD = LOW;

const uint16_t WATCHDOG_MS = 350;  // 이만큼 명령이 안 오면 스스로 멈춤
const uint16_t ACK_EVERY_MS = 500;
const bool USB_LOG = true;         // USB로 동작 기록 보내기 (115200)

const int EEPROM_ADDR = 500;       // 미션 1 코드가 앞쪽에 주행 기록을 남겨 뒀을 수 있어서 뒤쪽을 씀
const uint16_t EEPROM_MAGIC = 0x5250;

Motion motion;
FrameReader reader;
ActionTracker tracker;
uint8_t keys = 0;
bool linkAlive = false;
uint32_t lastValid = 0, lastAck = 0, lastStep = 0;
uint16_t cuts = 0;
int16_t appliedLeft = 0, appliedRight = 0;

void setMotor(uint8_t pwmPin, uint8_t dirPin, uint8_t forwardLevel, int16_t value) {
  if (value != 0) digitalWrite(dirPin, value > 0 ? forwardLevel : !forwardLevel);
  analogWrite(pwmPin, abs(value));   // 0이면 TB6612가 브레이크
}

void loadSettings() {
  uint16_t magic;
  EEPROM.get(EEPROM_ADDR, magic);
  if (magic == EEPROM_MAGIC) {
    EEPROM.get(EEPROM_ADDR + 2, motion.cfg);
    clampSettings(motion.cfg);
  } else {
    motion.cfg = defaultSettings();
  }
}

void saveSettings() {
  EEPROM.put(EEPROM_ADDR, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR + 2, motion.cfg);
}

void sendSettings() {
  radio.print(F("S,"));
  radio.print(motion.cfg.trim);
  radio.write(',');
  radio.print(motion.cfg.slowPct);
  radio.write(',');
  radio.print(motion.cfg.uturnMs);
  radio.write(',');
  radio.print(motion.cfg.launchMs);
  radio.write(',');
  radio.print(motion.cfg.nudgeMs);
  radio.write(',');
  radio.print(motion.cfg.cornerInner);
  radio.write('\n');
}

void handleFrame(uint32_t now) {
  lastValid = now;
  if (reader.tag == TAG_KEYS) {
    if (!linkAlive) {   // 다시 붙은 첫 명령: 마지막 C/Z/X를 또 실행하지 않게 기준만 잡음
      linkAlive = true;
      tracker.desync();
    }
    keys = reader.a;
    Action pressed = tracker.update(reader.b);
    if (pressed != ACTION_NONE) motion.press(pressed);
    if (now - lastAck >= ACK_EVERY_MS) {
      lastAck = now;
      radio.print(F("A,"));
      radio.print(cuts);
      radio.write('\n');
    }
  } else if (reader.tag == TAG_REQUEST) {
    sendSettings();
  } else if (reader.tag == TAG_DEFAULTS) {
    motion.cfg = defaultSettings();
    saveSettings();
    sendSettings();
  } else if ((reader.tag & 0xF0) == TAG_SETTING) {
    if (applySetting(motion.cfg, reader.tag & 0x0F, reader.value())) saveSettings();
    sendSettings();
  }
}

void setup() {
  for (uint8_t p : UNUSED_PWM) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  for (uint8_t p : UNUSED_DIR) pinMode(p, OUTPUT);
  pinMode(PWM_LEFT, OUTPUT);
  pinMode(PWM_RIGHT, OUTPUT);
  pinMode(DIR_LEFT, OUTPUT);
  pinMode(DIR_RIGHT, OUTPUT);
  setMotor(PWM_LEFT, DIR_LEFT, LEFT_FORWARD, 0);
  setMotor(PWM_RIGHT, DIR_RIGHT, RIGHT_FORWARD, 0);
  loadSettings();
  radio.begin(9600);
  Serial.begin(115200);
  Serial.println(F("RC PILOT v1 ready"));
}

void loop() {
  // 명령이 몰려도 안전 정지 검사가 밀리지 않게 한 번에 32바이트까지만
  for (uint8_t count = 0; count < 32 && radio.available(); count++) {
    uint32_t t = millis();
    if (reader.push((uint8_t)radio.read(), t)) handleFrame(t);
  }

  uint32_t now = millis();
  if (linkAlive && now - lastValid >= WATCHDOG_MS) {
    linkAlive = false;
    keys = 0;
    if (appliedLeft != 0 || appliedRight != 0) cuts++;
    motion.reset(now);
  }

  if (now == lastStep) return;
  lastStep = now;
  int16_t left, right;
  motion.step(now, keys, left, right);
  if (left == appliedLeft && right == appliedRight) return;
  setMotor(PWM_LEFT, DIR_LEFT, LEFT_FORWARD, left);
  setMotor(PWM_RIGHT, DIR_RIGHT, RIGHT_FORWARD, right);
  appliedLeft = left;
  appliedRight = right;
  if (USB_LOG) {
    // 시각 K 눌린키 O 왼쪽/오른쪽 C 끊김
    Serial.print(now); Serial.print(F(" K ")); Serial.print(keys);
    Serial.print(F(" O ")); Serial.print(left); Serial.print('/'); Serial.print(right);
    Serial.print(F(" C ")); Serial.println(cuts);
  }
}
