# E2E: open ConvAI SDK (host build of the device sources)
#      <-> real convai.v1 gateway (D:\dev\router) + mock gRPC backends.
#
# Usage: powershell -File run_e2e.ps1
$ErrorActionPreference = "Stop"
$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$goldieos = Resolve-Path "$here\..\.."
$repoRoot = Resolve-Path "$goldieos\..\.."
$router   = "D:\dev\router"
$out      = "$here\out"

New-Item -ItemType Directory -Force $out | Out-Null

# ---- 1. build gateway + mock backends ----
# Opus needs cgo+libopus (absent on Windows); codec/opus_stub.go covers
# CGO-less builds 鈥?g711a E2E 涓嶉渶瑕?opus銆?$env:GOPROXY = "https://goproxy.cn,direct"
$env:CGO_ENABLED = "0"
Push-Location $router
Write-Host "== build router =="
go build -o "$out\router.exe" ./cmd/router
if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE }
Write-Host "== build mockbackends =="
go build -o "$out\mockbackends.exe" ./cmd/mockbackends
if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE }
Pop-Location

# ---- 2. build the device simulator (same sources as the WS63 firmware) ----
Write-Host "== build e2e device simulator (host) =="
$src = @(
    "$goldieos\aitalk\src\convai_open.c",
    "$goldieos\aitalk\src\convai_ws_client.c",
    "$goldieos\aitalk\src\convai_protocol.c",
    "$goldieos\aitalk\src\convai_ring.c",
    "$goldieos\aitalk\src\convai_codec.c",
    "$goldieos\aitalk\src\convai_codec_g711a.c",
    "$goldieos\aitalk\src\codec_g711.c",
    "$goldieos\aitalk\src\codec_pcm.c",
    "$goldieos\aitalk\src\codec_ima_adpcm.c",
    "$goldieos\aitalk\src\aitalk_core.c",
    "$goldieos\third_party\cjson\cJSON.c",
    "$here\goldie_osal_host.c",
    "$here\net_sockets_host.c",
    "$here\e2e_main.c"
)
$inc = @(
    "-I$here\include",
    "-I$goldieos\aitalk\include",
    "-I$goldieos\include\osal",
    "-I$goldieos\third_party\cjson",
    "-I$repoRoot\include"
)
Push-Location $out
& gcc -std=gnu11 -O1 -g -Wall -Wextra -o e2e_device.exe $src $inc -lws2_32 -lpthread -lm
if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE }
Pop-Location

# ---- 3. start mock backends + gateway ----
Write-Host "== start mock backends (asr :51051 llm :51052 tts :51061) =="
$mock = Start-Process -PassThru -NoNewWindow "$out\mockbackends.exe" `
        -ArgumentList "-asr :51051 -llm :51052 -tts :51061" `
        -RedirectStandardOutput "$out\mock.log" -RedirectStandardError "$out\mock.err"
Start-Sleep -Milliseconds 800
Write-Host "== start gateway (:9000) =="
$gw = Start-Process -PassThru -NoNewWindow "$out\router.exe" `
      -ArgumentList "-listen :19000 -asr 127.0.0.1:51051 -llm 127.0.0.1:51052 -tts 127.0.0.1:51061" `
      -RedirectStandardOutput "$out\router.log" -RedirectStandardError "$out\router.err"
Start-Sleep -Milliseconds 800

# ---- 4. run the device simulator ----
Write-Host "== run e2e device =="
& "$out\e2e_device.exe"
$rc = $LASTEXITCODE

# ---- 5. teardown ----
Stop-Process -Id $gw.Id   -Force -ErrorAction SilentlyContinue
Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "logs: $out\router.log, $out\mock.log"
exit $rc

