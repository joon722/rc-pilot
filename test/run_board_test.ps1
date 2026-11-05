# 동작 시험을 UNO에 올려서 돌리고 결과를 읽음. 끝나면 rc_car를 다시 올려야 함
# 사용: powershell -File test\run_board_test.ps1 [-Port COM9]
param([string]$Port = 'COM9')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"

# 스케치 폴더 밖의 헤더는 컴파일러가 못 찾아서, 임시 폴더에 모아서 빌드
$build = Join-Path $env:TEMP 'rc_pilot_board_test\board_test'
New-Item -ItemType Directory -Force $build | Out-Null
Copy-Item "$root\test\board_test\board_test.ino", "$root\test\tests.h", "$root\rc_car\motion.h", "$root\rc_car\protocol.h" $build -Force

& $cli compile --fqbn arduino:avr:uno --upload --port $Port $build
if ($LASTEXITCODE -ne 0) { throw "업로드 실패" }

# 포트를 열면 보드가 재시작해서 시험이 처음부터 돎. 이 보드에는 글자를 보내지 않음 (부트로더가 망가질 수 있음)
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$p.DtrEnable = $true
$p.ReadTimeout = 500
$p.Open()
$deadline = (Get-Date).AddSeconds(25)
$failed = $null
try {
  while ((Get-Date) -lt $deadline -and $null -eq $failed) {
    try { $line = $p.ReadLine() } catch [System.TimeoutException] { continue }
    Write-Output $line.TrimEnd()
    if ($line -match 'BOARD TEST DONE failed=(\d+)') { $failed = [int]$Matches[1] }
  }
} finally { $p.Close() }
if ($null -eq $failed) { throw '시험 결과를 받지 못함' }
exit $failed
