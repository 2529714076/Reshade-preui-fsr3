# =============================================================================
#  下载构建依赖到 third_party/
#    * ReShade add-on 头文件  —— 固定 v6.8.0（RESHADE_API_VERSION = 20）
#    * MinHook                 —— master 分支
#
#  幂等：目录里已经有文件就跳过，不会重复下载。
#  用法：pwsh -File scripts/fetch-deps.ps1
# =============================================================================
param(
    [string] $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$thirdParty = Join-Path $Root 'third_party'
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

# ---------------------------------------------------------------------------
# 1. ReShade add-on 头文件（8 个，缺一个就编译不过）
# ---------------------------------------------------------------------------
$reshadeDir = Join-Path $thirdParty 'reshade/include'
New-Item -ItemType Directory -Force -Path $reshadeDir | Out-Null

$reshadeFiles = @(
    'reshade.hpp',
    'reshade_api.hpp',
    'reshade_api_device.hpp',
    'reshade_api_format.hpp',
    'reshade_api_pipeline.hpp',
    'reshade_api_resource.hpp',
    'reshade_events.hpp',
    'reshade_overlay.hpp'
)

$base = 'https://raw.githubusercontent.com/crosire/reshade/v6.8.0/include'

foreach ($file in $reshadeFiles) {
    $dest = Join-Path $reshadeDir $file
    if (Test-Path $dest) {
        Write-Host "  [skip] reshade/$file"
        continue
    }
    Write-Host "  [get ] reshade/$file"
    Invoke-WebRequest -Uri "$base/$file" -OutFile $dest -UseBasicParsing
}

# ---------------------------------------------------------------------------
# 2. MinHook
# ---------------------------------------------------------------------------
$minhookDir = Join-Path $thirdParty 'minhook'
if (Test-Path (Join-Path $minhookDir 'include/MinHook.h')) {
    Write-Host '  [skip] minhook'
}
else {
    $work = Join-Path $env:TEMP ('minhook-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    $zip = Join-Path $work 'minhook.zip'
    Write-Host '  [get ] minhook'
    Invoke-WebRequest -Uri 'https://codeload.github.com/TsudaKageyu/minhook/zip/refs/heads/master' `
        -OutFile $zip -UseBasicParsing

    Expand-Archive -Path $zip -DestinationPath $work -Force

    $src = Get-ChildItem -Path $work -Directory | Select-Object -First 1
    if (-not $src) { throw 'MinHook archive layout unexpected (no top-level folder)' }

    # 复制「内容」到 third_party/minhook，避免目录已存在时变成 minhook/minhook-master
    New-Item -ItemType Directory -Force -Path $minhookDir | Out-Null
    Copy-Item -Recurse -Force (Join-Path $src.FullName '*') $minhookDir
    Remove-Item -Recurse -Force $work
}

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'dependencies ready:'
Get-ChildItem (Join-Path $thirdParty 'reshade/include') | Select-Object Name, Length | Format-Table -AutoSize
Write-Host ('  minhook: ' + (Test-Path (Join-Path $minhookDir 'include/MinHook.h')))
