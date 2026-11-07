#Requires -Version 7.0
# Builds LinuxLive.vcxproj (x64) - GUI and command line in one executable, C++23.
#
# Nothing is hard-coded: the toolchain is chosen by evidence on this machine.
#   * Visual Studio: every instance vswhere reports (-all -products *: SQL Server Management Studio is
#     listed too, so products without the C++ tools are skipped) plus a scan of "Microsoft Visual Studio";
#     usable = MSBuild.exe + the platform toolset named in the project + an MSVC with bin\Hostx64\x64\cl.exe.
#   * MSVC and Windows SDK: the newest version (sorted as [version]) whose files are really there -
#     cl.exe, and for the SDK x64\rc.exe, um\Windows.h and um\x64\kernel32.lib.
# Afterwards bin\build-receipt.json records toolchain, input hashes and output hashes.
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Rebuild,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

$project = Join-Path $PSScriptRoot 'LinuxLive.vcxproj'
$bin = Join-Path $PSScriptRoot 'bin'
$obj = Join-Path $PSScriptRoot 'obj'

if ($Clean) {
    Remove-Item -LiteralPath $obj -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $bin 'LinuxLive*'), (Join-Path $bin 'build-receipt.json') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $bin 'language') -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host 'Cleaned obj\ and the build output in bin\.' -ForegroundColor Green
    return
}

function Get-Inputs {
    $paths = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -Recurse -File | Select-Object -ExpandProperty FullName)
    $paths += $project, "$project.filters", $PSCommandPath
    @($paths | Sort-Object | ForEach-Object {
        [pscustomobject]@{ path = [IO.Path]::GetRelativePath($PSScriptRoot, $_); sha256 = (Get-FileHash -LiteralPath $_).Hash }
    })
}

# --- Toolchain --------------------------------------------------------------------
$toolset = ([xml](Get-Content -LiteralPath $project)).Project.PropertyGroup.PlatformToolset | Where-Object { $_ } | Select-Object -First 1
$programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$candidates = @()
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $candidates += & $vswhere -all -products '*' -prerelease -format value -property installationPath
}
foreach ($base in $env:ProgramFiles, $programFilesX86) {
    $vsBase = Join-Path $base 'Microsoft Visual Studio'
    if (Test-Path -LiteralPath $vsBase) {
        $candidates += Get-ChildItem -LiteralPath $vsBase -Directory |
            ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory } | Select-Object -ExpandProperty FullName
    }
}
$found = foreach ($vs in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild)) { continue }
    $hasToolset = Get-ChildItem -LiteralPath (Join-Path $vs 'MSBuild\Microsoft\VC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "Platforms\x64\PlatformToolsets\$toolset") }
    if (-not $hasToolset) { continue }
    Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -as [version] -and (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')) } |
        ForEach-Object { [pscustomobject]@{ msbuild = $msbuild; msvc = $_.Name; vs = $vs } }
}
$toolchain = $found | Sort-Object { [version]$_.msvc } -Descending | Select-Object -First 1
if (-not $toolchain) { throw "No Visual Studio with MSBuild, the $toolset toolset and the x64 C++ compiler was found." }

$kits = Join-Path $programFilesX86 'Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $kits 'bin') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -as [version] -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'x64\rc.exe')) -and
        (Test-Path -LiteralPath (Join-Path $kits "Include\$($_.Name)\um\Windows.h")) -and
        (Test-Path -LiteralPath (Join-Path $kits "Lib\$($_.Name)\um\x64\kernel32.lib")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'No complete Windows SDK (x64 rc.exe, headers and libraries) was found.' }
Write-Host "MSVC $($toolchain.msvc)  |  Windows SDK $($sdk.Name)  |  toolset $toolset  |  $Configuration x64" -ForegroundColor Cyan

# --- Build ------------------------------------------------------------------------
$inputs = Get-Inputs
$arguments = @(
    $project, "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:PlatformToolset=$toolset",
    "/p:VCToolsVersion=$($toolchain.msvc)", "/p:WindowsTargetPlatformVersion=$($sdk.Name)",
    "/t:$(if ($Rebuild) { 'Rebuild' } else { 'Build' })", '/m', '/nologo', '/v:minimal', '/clp:Summary'
)
& $toolchain.msbuild @arguments
if ($LASTEXITCODE) { throw "Build failed ($LASTEXITCODE)." }
if ((ConvertTo-Json $inputs -Compress) -ne (ConvertTo-Json (Get-Inputs) -Compress)) {
    throw 'Sources changed while compiling - build again.'
}

# --- Result -----------------------------------------------------------------------
$name = if ($Configuration -eq 'Debug') { 'LinuxLive_debug.exe' } else { 'LinuxLive.exe' }
$outputs = @(Get-Item -LiteralPath (Join-Path $bin $name)) + @(Get-ChildItem -LiteralPath (Join-Path $bin 'language') -Filter *.lng -ErrorAction SilentlyContinue)
$outputs = @($outputs | ForEach-Object {
    [pscustomobject]@{ path = [IO.Path]::GetRelativePath($bin, $_.FullName); length = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
})
[pscustomobject]@{
    configuration = $Configuration; msvc = $toolchain.msvc; sdk = $sdk.Name; toolset = $toolset
    visualStudio = $toolchain.vs; inputs = $inputs; outputs = $outputs
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $bin 'build-receipt.json') -Encoding utf8
$outputs | Format-Table -AutoSize
Write-Host "Build succeeded (/W4 /WX /sdl /guard:cf, CET). Receipt: bin\build-receipt.json" -ForegroundColor Green
