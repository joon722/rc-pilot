"""실제 차로 처음부터 끝까지 시험: 노트북 → 블루투스 → 차 코드 → USB 기록.

사이트와 똑같은 명령을 HM-10으로 보내고, 차가 USB로 찍는 모터 출력을 차의 시계로 잼.
건전지를 끄고 USB만 꽂으면 바퀴는 안 돌고 출력만 확인됨 (HM-10은 USB 전원으로 켜짐).

준비: 사이트에서 차 연결을 끊고, rc_car를 올린 상태에서
사용: python test/e2e_test.py [--port COM9] [--name qwer]
USB로는 글자를 절대 보내지 않음 (재시작 직후 글자를 보내면 부트로더가 플래시를 지울 수 있음).
"""
import argparse
import asyncio
import re
import sys
import threading
import time

import serial
from bleak import BleakClient, BleakScanner

CHAR = "0000ffe1-0000-1000-8000-00805f9b34fb"
UP, DOWN, LEFT, RIGHT, SHIFT = 1, 2, 4, 8, 16
LOG = re.compile(r"^(\d+) K (\d+) O (-?\d+)/(-?\d+) C (\d+)")


def frame(tag, a, b):
    a &= 255
    b &= 255
    return bytes([0xA5, tag, a, b, tag ^ a ^ b])


class Car:
    """USB 기록을 모으고, 블루투스로 명령을 보냄."""

    def __init__(self, port, client):
        self.client = client
        self.action = 0
        self.lines = []        # 차가 블루투스로 보낸 줄
        self.rx = ""
        self.log = []          # (차 시각 ms, 키, 왼쪽, 오른쪽, 끊김)
        self.usb = port
        self.stop_reading = False
        self.reader = threading.Thread(target=self._read_usb, daemon=True)
        self.reader.start()

    def _read_usb(self):
        while not self.stop_reading:
            line = self.usb.readline().decode("ascii", "ignore").strip()
            m = LOG.match(line)
            if m:
                self.log.append(tuple(int(x) for x in m.groups()))

    def on_notify(self, _sender, data):
        self.rx += data.decode("ascii", "ignore")
        while "\n" in self.rx:
            line, self.rx = self.rx.split("\n", 1)
            self.lines.append(line.strip())

    async def send(self, data):
        await self.client.write_gatt_char(CHAR, data, response=False)

    async def hold(self, keys, ms):
        end = time.monotonic() + ms / 1000
        while time.monotonic() < end:
            await self.send(frame(0x4B, keys, self.action))
            await asyncio.sleep(0.04)

    def press(self, name):
        kind = {"C": 1, "Z": 2, "X": 3}[name]
        self.action = (kind << 6) | (((self.action & 63) + 1) & 63)

    async def settings(self):
        """세팅을 요청해서 차가 답한 줄을 돌려줌."""
        self.lines.clear()
        for _ in range(10):
            await self.send(frame(0x52, 0, 0))
            await self.hold(0, 300)
            found = [x for x in self.lines if x.startswith("S,")]
            if found:
                return found[-1]
        return None

    async def set_value(self, setting_id, value):
        v = value & 0xFFFF
        await self.send(frame(0x10 | setting_id, v & 255, v >> 8))
        await self.hold(0, 300)

    async def take_log(self, settle=0.3):
        await asyncio.sleep(settle)
        taken, self.log = self.log, []
        return taken


results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'통과' if ok else '실패'}  {name}" + (f"  ({detail})" if detail else ""))


def spans(log):
    """기록을 (왼쪽, 오른쪽, 그 값이 유지된 ms) 목록으로. 마지막 값은 길이를 모름(None)."""
    out = []
    for i, row in enumerate(log):
        t, _, l, r, _ = row
        out.append((l, r, log[i + 1][0] - t if i + 1 < len(log) else None))
    return out


async def test_pivot_then_forward(car):
    print("- 제자리 회전 뒤 직진: 두 바퀴를 같이 세운 뒤 낮은 힘부터")
    await car.hold(RIGHT, 500)
    await car.hold(UP, 1000)
    await car.hold(0, 300)
    s = spans(await car.take_log())
    pivot = [i for i, x in enumerate(s) if x[0] > 0 and x[1] < 0]
    check("제자리 회전이 나옴", bool(pivot))
    if not pivot:
        return
    after = s[pivot[-1] + 1:]
    check("회전 다음은 0/0", after and after[0][:2] == (0, 0), str(after[:1]))
    if not after or after[0][2] is None:
        return
    check("0.2초 멈춤", 190 <= after[0][2] <= 230, f"{after[0][2]}ms")
    first = after[1] if len(after) > 1 else None
    check("두 바퀴가 같이 낮은 힘에서 출발", first and 0 < first[0] <= 130 and 0 < first[1] <= 130, str(first))
    check("거꾸로 도는 바퀴 없음", all(x[0] >= 0 and x[1] >= 0 for x in after))
    check("한쪽만 도는 순간 없음", all((x[0] > 0) == (x[1] > 0) for x in after))
    peak = max(x[1] for x in after)
    check("최고 속도까지 올라감", peak == 255, f"오른쪽 최고 {peak}")


async def test_launch_time(car):
    print("- 출발 가속: 0.5초에 걸쳐 최고로")
    await car.hold(UP, 1000)
    await car.hold(0, 300)
    log = await car.take_log()
    top = next((row for row in log if row[3] == 255), None)
    check("최고 속도 도달", top is not None)
    if top:
        took = top[0] - log[0][0]
        check("가속 시간 약 0.5초", 450 <= took <= 560, f"{took}ms")


async def test_uturn_time(car):
    print("- C: 차 시계로 잰 회전 시간이 세팅값과 같음")
    for _ in range(3):
        car.press("C")
        await car.hold(0, 800)
        s = spans(await car.take_log())
        turn = [x for x in s if x[0] > 0 and x[1] < 0]
        check("정해진 힘으로 오른쪽 회전", len(turn) == 1 and turn[0][0] == 200 and turn[0][1] == -200, str(turn))
        if turn and turn[0][2] is not None:
            check("회전 시간 310ms", abs(turn[0][2] - 310) <= 3, f"{turn[0][2]}ms")


async def test_running_nudge(car):
    print("- 달리는 중 Z/X: 한쪽 힘만 잠깐 줄임")
    await car.hold(UP, 900)
    await car.take_log(0)
    car.press("Z")
    await car.hold(UP, 300)
    z = spans(await car.take_log(0))
    car.press("X")
    await car.hold(UP, 300)
    x = spans(await car.take_log(0))
    await car.hold(0, 300)
    await car.take_log()
    check("Z: 왼쪽만 줄어듦", any(a[0] < 200 and a[1] == 255 for a in z), str(z[:3]))
    check("Z: 바퀴가 안 멈춤", all(a[0] > 0 and a[1] > 0 for a in z))
    check("X: 오른쪽만 줄어듦", any(a[1] < 200 and a[0] == 250 for a in x), str(x[:3]))
    check("X: 바퀴가 안 멈춤", all(a[0] > 0 and a[1] > 0 for a in x))
    cut = next((a for a in z if a[0] < 200 and a[2] is not None), None)
    if cut:
        check("줄이는 시간 40ms", abs(cut[2] - 40) <= 3, f"{cut[2]}ms")


async def test_corner(car):
    print("- 코너: 안쪽 바퀴도 계속 앞으로")
    await car.hold(UP, 800)
    await car.hold(UP | RIGHT, 400)
    await car.hold(UP | LEFT, 500)
    await car.hold(0, 300)
    s = spans(await car.take_log())
    moving = s[:-1]
    check("달리는 내내 거꾸로 도는 바퀴 없음", all(a[0] > 0 and a[1] > 0 for a in moving))
    check("오른쪽 코너 값 211/163", any(abs(a[0] - 211) <= 1 and abs(a[1] - 163) <= 1 for a in s), "")
    check("왼쪽 코너 값 157/217", any(abs(a[0] - 157) <= 1 and abs(a[1] - 217) <= 1 for a in s), "")


async def test_link_loss(car):
    print("- 통신 끊김: 0.35초 뒤 스스로 멈추고, 다시 붙어도 C가 또 안 나옴")
    car.press("C")
    await car.hold(0, 800)
    await car.take_log()
    await car.hold(UP, 700)
    before = await car.take_log(0)
    cuts_before = before[-1][4] if before else 0
    last_sent = time.monotonic()
    await asyncio.sleep(0.8)      # 명령을 안 보냄
    log = await car.take_log(0)
    stop = next((row for row in log if row[2] == 0 and row[3] == 0), None)
    check("스스로 멈춤", stop is not None)
    if stop:
        check("끊김 횟수가 올라감", stop[4] == cuts_before + 1, f"{cuts_before} → {stop[4]}")
    await car.hold(0, 800)        # 같은 C 값으로 다시 연결
    again = await car.take_log()
    check("다시 붙어도 회전 안 함", not any(row[2] != 0 or row[3] != 0 for row in again), str(again[:2]))
    assert last_sent


async def test_settings(car, port):
    print("- 세팅: 사이트에서 바꾸면 차에 저장되고 재시작해도 남음")
    start = await car.settings()
    check("세팅을 알려 줌", start is not None, str(start))
    await car.set_value(2, 320)
    await car.set_value(0, -7)
    now = await car.settings()
    check("바뀐 값이 반영됨", now and now.split(",")[1] == "-7" and now.split(",")[3] == "320", str(now))
    car.press("C")
    await car.hold(0, 800)
    s = spans(await car.take_log())
    turn = [x for x in s if x[0] > 0 and x[1] < 0]
    if turn and turn[0][2] is not None:
        check("C 시간에 적용됨", abs(turn[0][2] - 320) <= 3, f"{turn[0][2]}ms")
    # USB 포트의 DTR로 차를 재시작 (글자는 보내지 않음)
    port.dtr = False
    await asyncio.sleep(0.1)
    port.dtr = True
    await asyncio.sleep(2.5)
    again = await car.settings()
    check("재시작해도 남음", again == now, str(again))
    await car.send(frame(0x44, 0, 0))
    await car.hold(0, 300)
    final = await car.settings()
    print(f"  기본값으로 되돌림: {final}")
    await car.take_log()


async def run(args):
    port = serial.Serial(args.port, 115200, timeout=0.2)
    port.dtr = True
    await asyncio.sleep(2.5)     # 포트를 열면 재시작됨. 끝날 때까지 기다림
    print("BLE 찾는 중...")
    found = await BleakScanner.discover(timeout=8)
    named = [d for d in found if d.name and args.name.lower() in d.name.lower()]
    print("  찾음:", [(d.name, d.address) for d in named])
    if len(named) != 1:
        print("차를 하나로 못 골랐어요. 사이트 연결을 끊었는지, --name이 맞는지 확인하세요.")
        return 2
    async with BleakClient(named[0]) as client:
        car = Car(port, client)
        await client.start_notify(CHAR, car.on_notify)
        await car.hold(0, 500)
        await car.send(frame(0x44, 0, 0))      # 기본값에서 시작
        await car.hold(0, 300)
        await car.take_log()
        await test_pivot_then_forward(car)
        await test_launch_time(car)
        await test_uturn_time(car)
        await test_running_nudge(car)
        await test_corner(car)
        await test_link_loss(car)
        await test_settings(car, port)
        await car.hold(0, 200)
        car.stop_reading = True
    port.close()
    print(f"\n{sum(results)}/{len(results)} 통과")
    return 0 if all(results) else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--name", default="qwer")
    sys.exit(asyncio.run(run(ap.parse_args())))
