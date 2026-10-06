<#
.SYNOPSIS
编译并执行运动数学、真实控制器及配置模块的主机断言测试。
.DESCRIPTION
使用指定 C++ 编译器生成 test-artifacts 中的可执行文件，任一步失败立即退出。
控制器以硬件和 RTOS 替身运行真实实现；主机测试不验证上板行为或真实调度时延。
配置测试依赖项目已安装的 ArduinoJson 库，不使用控制器测试的 JSON 占位头。
#>
param([string]$Compiler = 'g++')
$ErrorActionPreference = 'Stop'
# 1. 定位工程与产物目录，并将编译器目录加入本进程 PATH。
$projectDirectory = Split-Path -Parent $PSScriptRoot
$artifactDirectory = Join-Path $projectDirectory 'test-artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$env:PATH = (Split-Path -Parent $compilerCommand.Source) + ';' + $env:PATH
Push-Location $projectDirectory
try {
  # 2. 数学工具：验证换算、PID、斜坡和安全判据。
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -Iinclude test/test_motion.cpp -o "$artifactDirectory/test_motion.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Motion test compilation failed.' }
  & "$artifactDirectory/test_motion.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Motion tests failed.' }
  # 3. 控制器：直接包含生产实现，以确定性替身推进各安全与标定场景。
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -Itest/host -Iinclude test/test_controller.cpp src/arena_model.cpp -o "$artifactDirectory/test_controller.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Controller test compilation failed.' }
  & "$artifactDirectory/test_controller.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Controller tests failed.' }
  # 4. 配置：真实 JSON 与存储替身共同验证字段校验和失败保留。
  & $compilerCommand.Source -std=c++17 -Wall -Wextra -Werror -I.pio/libdeps/esp32s3/ArduinoJson/src -Itest/host -Iinclude test/test_config.cpp -o "$artifactDirectory/test_config.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Config test compilation failed; install PlatformIO dependencies first.' }
  & "$artifactDirectory/test_config.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Config tests failed.' }
# 无论成功或失败，都恢复进入脚本前的工作目录。
} finally { Pop-Location }
