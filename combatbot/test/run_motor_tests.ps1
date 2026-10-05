<#
CombatBot · 电机硬件接口主机回归
============================================================================
职责：分别编译 core 2.x / 3.x API 替身，执行真实 motor.cpp 的输出与失败场景。
边界：不上传、不操作串口；产物写入 test-artifacts/motor-host。
============================================================================
#>
param([string]$Compiler = 'D:\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'test-artifacts\motor-host'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
if (-not (Test-Path -LiteralPath $Compiler)) { throw "Compiler not found: $Compiler" }
$previousPath = $env:PATH
$env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
try {
foreach ($coreMajor in @(2, 3)) {
    $executable = Join-Path $outputRoot "test_motor_core$coreMajor.exe"
    & $Compiler '-std=c++11' '-Wall' '-Wextra' '-Werror' '-pedantic' "-DESP_ARDUINO_VERSION_MAJOR=$coreMajor" "-I$(Join-Path $PSScriptRoot 'motor_host')" "-I$(Join-Path $projectRoot 'include')" (Join-Path $PSScriptRoot 'test_motor.cpp') '-o' $executable
    if ($LASTEXITCODE -ne 0) { throw "Core $coreMajor test compilation failed" }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw "Core $coreMajor normal operation regression failed" }
    foreach ($channel in 0..4) {
        & $executable 'attach-failure' "$channel"
        if ($LASTEXITCODE -ne 0) { throw "Core $coreMajor channel $channel failure gate regression failed" }
    }
    if ($coreMajor -eq 3) {
        foreach ($channel in 0..4) {
            & $executable 'write-failure' "$channel"
            if ($LASTEXITCODE -ne 0) { throw "Core $coreMajor channel $channel initial zero-write regression failed" }
        }
    }
}
Write-Host 'Motor compatibility and initialization safety regressions passed.'
} finally { $env:PATH = $previousPath }
