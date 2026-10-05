<#
.SYNOPSIS
从交付包安装已验证的 Arduino 依赖，保留被替换库的完整备份。
.DESCRIPTION
先解压并核验全部库的版本，再将同名旧库移到工程旁的备份目录，最后安装新库。
不修改开发板包，不执行上传。若安装中断，原库仍可从备份目录恢复。
#>
param([string]$ArduinoUserDirectory = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Arduino'))
$ErrorActionPreference = 'Stop'
$libraryDirectory = [IO.Path]::GetFullPath((Join-Path $ArduinoUserDirectory 'libraries'))
$deliveryDirectory = [IO.Path]::GetFullPath($PSScriptRoot)
$operationId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$backupDirectory = Join-Path $deliveryDirectory "Arduino旧库备份\$operationId"
$stagingDirectory = Join-Path $deliveryDirectory ".arduino-dependency-work\$operationId"
$libraryVersions = [ordered]@{ArduinoJson='7.3.1'; AsyncTCP='3.3.2'; ESPAsyncWebServer='3.6.0'}
New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $libraryDirectory -Force | Out-Null

# 1. 全部 ZIP 先进入暂存目录；缺包或版本错误时，用户原有库不会被移动。
foreach ($libraryName in $libraryVersions.Keys) {
  $version = $libraryVersions[$libraryName]
  $zipPath = Join-Path $deliveryDirectory "Arduino依赖库\$libraryName-$version.zip"
  Expand-Archive -LiteralPath $zipPath -DestinationPath $stagingDirectory
  $properties = Get-Content -LiteralPath (Join-Path $stagingDirectory "$libraryName\library.properties")
  if ($properties -notcontains "version=$version") { throw "依赖版本不符：$libraryName" }
}

# 2. 按库头文件识别重复安装目录，避免不同目录名的旧版库继续参与 Arduino 选库。
$oldLibraries = @(Get-ChildItem -LiteralPath $libraryDirectory -Directory | Where-Object {
  (Test-Path -LiteralPath (Join-Path $_.FullName 'src\ArduinoJson.h')) -or
  (Test-Path -LiteralPath (Join-Path $_.FullName 'src\AsyncTCP.h')) -or
  (Test-Path -LiteralPath (Join-Path $_.FullName 'src\ESPAsyncWebServer.h')) -or
  ($libraryVersions.Contains($_.Name))
})
if ($oldLibraries.Count -gt 0) { New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null }
foreach ($oldLibrary in $oldLibraries) {
  $sourcePath = [IO.Path]::GetFullPath($oldLibrary.FullName)
  $targetPath = [IO.Path]::GetFullPath((Join-Path $backupDirectory $oldLibrary.Name))
  # 移动前确认源在用户库目录、目标在本次备份目录；只移动已识别的单个库。
  if (-not $sourcePath.StartsWith($libraryDirectory.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
      -not $targetPath.StartsWith($backupDirectory.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw '库备份路径超出预期目录。'
  }
  Move-Item -LiteralPath $sourcePath -Destination $targetPath
}

# 3. 暂存目录整体移入；不覆盖残留旧文件，保证安装内容与交付 ZIP 完全一致。
foreach ($libraryName in $libraryVersions.Keys) {
  $sourcePath = [IO.Path]::GetFullPath((Join-Path $stagingDirectory $libraryName))
  $targetPath = [IO.Path]::GetFullPath((Join-Path $libraryDirectory $libraryName))
  if (-not $sourcePath.StartsWith($stagingDirectory.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
      -not $targetPath.StartsWith($libraryDirectory.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw '库安装路径超出预期目录。'
  }
  Move-Item -LiteralPath $sourcePath -Destination $targetPath
  Write-Output "已安装：$libraryName $($libraryVersions[$libraryName])"
}
if ($oldLibraries.Count -gt 0) { Write-Output "原库备份：$backupDirectory" }
Write-Output '请重新打开 Arduino IDE，使库列表刷新后再验证工程。'
