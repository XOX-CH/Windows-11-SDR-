param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src = Join-Path $root 'src'
$obj = Join-Path $root 'obj'
$bin = Join-Path $root 'bin'

if ($Clean) {
    Remove-Item $obj -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $bin -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $obj, $bin | Out-Null

# vcvars64.bat shells out to reg.exe, which locked-down environments block,
# so the build environment is assembled directly from vswhere + the SDK folders.
$pf86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'MSVC C++ build tools not found.' }
$vsPath = $vsPath.Trim()

$toolset = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $toolset) { throw 'MSVC toolset not found.' }

$sdkRoot = Join-Path $pf86 'Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { $_.Name -match '^10\.' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $sdkVersion) { throw 'Windows SDK not found.' }

$binDir = Join-Path $toolset.FullName 'bin\Hostx64\x64'
if (-not (Test-Path (Join-Path $binDir 'cl.exe'))) { throw "cl.exe not found under $binDir" }

$sdkInc = Join-Path $sdkRoot "Include\$($sdkVersion.Name)"
$sdkLib = Join-Path $sdkRoot "Lib\$($sdkVersion.Name)"
$env:INCLUDE = @(
    (Join-Path $toolset.FullName 'include'),
    (Join-Path $sdkInc 'ucrt'),
    (Join-Path $sdkInc 'um'),
    (Join-Path $sdkInc 'shared'),
    (Join-Path $sdkInc 'winrt')
) -join ';'
$env:LIB = @(
    (Join-Path $toolset.FullName 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'
$env:PATH = "$binDir;$env:PATH"

$common = @('/nologo', '/c', '/O2', '/GL', '/MT', '/EHsc', '/std:c++20', '/W4', '/permissive-', '/utf-8', '/DUNICODE', '/D_UNICODE')

Push-Location $root
try {
    & rc.exe /nologo /c 65001 /I $src /fo "$obj\app.res" "$src\app.rc"
    if ($LASTEXITCODE -ne 0) { throw "rc failed: $LASTEXITCODE" }

    & cl.exe @common /I $src /Fo"$obj\display_config.obj" "$src\display_config.cpp"
    if ($LASTEXITCODE -ne 0) { throw "cl display_config failed: $LASTEXITCODE" }

    & cl.exe @common /I $src /Fo"$obj\main.obj" "$src\main.cpp"
    if ($LASTEXITCODE -ne 0) { throw "cl main failed: $LASTEXITCODE" }

    & link.exe /nologo /LTCG /OPT:REF /OPT:ICF /MACHINE:X64 /SUBSYSTEM:WINDOWS /MANIFEST:NO `
        /OUT:"$bin\SdrTray.exe" `
        "$obj\display_config.obj" "$obj\main.obj" "$obj\app.res" `
        user32.lib gdi32.lib msimg32.lib shell32.lib advapi32.lib dwmapi.lib shcore.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { throw "link failed: $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$exe = Join-Path $bin 'SdrTray.exe'
if (-not (Test-Path $exe)) { throw 'SdrTray.exe was not produced.' }
$item = Get-Item $exe
"Built $($item.FullName) ($([math]::Round($item.Length / 1KB)) KB)"
