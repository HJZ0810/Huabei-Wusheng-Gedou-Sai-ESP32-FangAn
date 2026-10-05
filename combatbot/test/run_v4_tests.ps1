<#
.SYNOPSIS
编译执行 V4 真实入口和控制器集成回归，产物存入 test-artifacts。
#>
param([string]$Compiler='g++')
$ErrorActionPreference='Stop'
$projectDirectory=Split-Path -Parent $PSScriptRoot
$artifactDirectory=Join-Path $projectDirectory 'test-artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$compilerCommand=Get-Command $Compiler -ErrorAction Stop
$env:PATH=(Split-Path -Parent $compilerCommand.Source)+';'+$env:PATH
Push-Location $projectDirectory
try {
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -Iinclude test/test_cloud_protocol.cpp -o "$artifactDirectory/test_cloud_protocol.exe"
  if($LASTEXITCODE -ne 0){throw 'Cloud protocol compilation failed.'}
  & "$artifactDirectory/test_cloud_protocol.exe"
  if($LASTEXITCODE -ne 0){throw 'Cloud protocol tests failed.'}
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -I.pio/libdeps/esp32s3/ArduinoJson/src -Itest/host -Iinclude test/test_gateway.cpp -o "$artifactDirectory/test_gateway.exe"
  if($LASTEXITCODE -ne 0){throw 'Gateway compilation failed.'}
  & "$artifactDirectory/test_gateway.exe"
  if($LASTEXITCODE -ne 0){throw 'Gateway tests failed.'}
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -Itest/host -Iinclude test/test_controller_v4.cpp src/arena_model.cpp -o "$artifactDirectory/test_controller_v4.exe"
  if($LASTEXITCODE -ne 0){throw 'V4 controller compilation failed.'}
  & "$artifactDirectory/test_controller_v4.exe"
  if($LASTEXITCODE -ne 0){throw 'V4 controller tests failed.'}
}finally{Pop-Location}
