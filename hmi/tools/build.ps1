[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [string]$Ccache = '',
    [int]$Jobs = 6,
    [string]$CudaArchitectures = '',
    [switch]$Test,
    [switch]$Diagnostics,
    [Alias('Cudnn')][switch]$BuildCudnnSdk,
    [ValidateSet('Visual Studio 18 2026', 'Ninja')][string]$Generator,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')][string]$Configuration = 'Release',
    [string[]]$Targets = @()
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
if (-not $BuildDirectory) {
    $BuildDirectory = if ($Generator -eq 'Ninja') { 'build\ninja' } else { 'build' }
}
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
$buildRoot = Join-Path $root 'build'
if (-not $BuildDirectory.Equals($buildRoot, [StringComparison]::OrdinalIgnoreCase) -and
    -not $BuildDirectory.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must be project/build or a subdirectory of project/build.'
}
# A junction must not redirect generated files outside the physical build tree.
$ancestor = $BuildDirectory
while ($ancestor -and $ancestor -ne $root) {
    if (Test-Path -LiteralPath $ancestor) {
        if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw ('BuildDirectory must not traverse a junction or symbolic link: ' + $ancestor)
        }
    }
    $ancestor = Split-Path -Parent $ancestor
}
if ($Jobs -lt 1 -or $Jobs -gt 64) { throw 'Jobs must be 1..64.' }
$cacheFile = Join-Path $BuildDirectory 'CMakeCache.txt'
$cacheLines = if (Test-Path -LiteralPath $cacheFile) { @(Get-Content -LiteralPath $cacheFile) } else { @() }
function Read-CacheValue([string]$Name) {
    $pattern = '^' + [regex]::Escape($Name) + ':[^=]+=(.*)$'
    foreach ($line in $cacheLines) {
        if ($line -match $pattern) { return $matches[1] }
    }
    return ''
}
$cachedGenerator = Read-CacheValue 'CMAKE_GENERATOR'
if ($Generator -and $cachedGenerator -and $Generator -ne $cachedGenerator) {
    throw ('This build directory uses ' + $cachedGenerator + '. Select a different BuildDirectory under project/build for ' + $Generator + '.')
}
if (-not $Generator) { $Generator = if ($cachedGenerator) { $cachedGenerator } else { 'Visual Studio 18 2026' } }
if ($Generator -notin @('Visual Studio 18 2026', 'Ninja')) { throw ('Unsupported build generator: ' + $Generator) }
$installation = Read-CacheValue 'CMAKE_GENERATOR_INSTANCE'
if (-not $installation -or -not (Test-Path -LiteralPath (Join-Path $installation 'Common7\Tools\VsDevCmd.bat'))) {
    $vswhereCommand = Get-Command vswhere.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $vswhere = if ($vswhereCommand) { $vswhereCommand.Source } else { Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio C++ build tools are required.' }
    $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio C++ build tools were not found.' }
}
$developer = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$setup = 'call "' + $developer + '" -no_logo -arch=x64 -host_arch=x64 >nul && set'
foreach ($line in (& $env:ComSpec /d /s /c $setup)) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment setup failed.' }
$temporaryDirectory = Join-Path $BuildDirectory 'tmp'
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
if ((Get-Item -LiteralPath $temporaryDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Build temporary directory must not be redirected outside the project.'
}
foreach ($temporaryVariable in @('TEMP', 'TMP')) {
    [Environment]::SetEnvironmentVariable($temporaryVariable, $temporaryDirectory, 'Process')
}
$cmakeCommand = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { throw ('Build tool not found: ' + $cmake) }
if ($Test -and -not (Test-Path -LiteralPath $ctest -PathType Leaf)) { throw ('Build tool not found: ' + $ctest) }
$explicitCcache = [bool]$Ccache
if (-not $Ccache) { $Ccache = Read-CacheValue 'HMI_CCACHE_EXECUTABLE' }
if (-not $Ccache -or -not (Test-Path -LiteralPath $Ccache -PathType Leaf)) {
    $ccacheCommand = Get-Command ccache.exe, ccache -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $Ccache = if ($ccacheCommand) { $ccacheCommand.Source } else { '' }
}
$cachedCcache = Read-CacheValue 'HMI_USE_CCACHE'
$useCcache = [bool]$Ccache -and ($explicitCcache -or $cachedCcache -eq 'ON' -or (-not $cachedCcache -and $Generator -eq 'Ninja'))
$cacheOption = if ($useCcache) { 'ON' } else { 'OFF' }
$configure = @('-S', $root, '-B', $BuildDirectory,
    "-DHMI_USE_CCACHE=$cacheOption", "-DHMI_CCACHE_EXECUTABLE:FILEPATH=$Ccache",
    "-DHMI_CCACHE_DIR=$BuildDirectory/.ccache")
# Reuse the cached generator/platform without introducing a conflicting -A flag.
if (-not $cachedGenerator) { $configure += @('-G', $Generator) }
if ($Generator -eq 'Ninja') {
    $ninjaCommand = Get-Command ninja.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $ninja = if ($ninjaCommand) { $ninjaCommand.Source } else { Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' }
    if (-not (Test-Path -LiteralPath $ninja -PathType Leaf)) { throw ('Build tool not found: ' + $ninja) }
    $configure += @("-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Configuration")
}
if ($CudaArchitectures) {
    $configure += @("-DHMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES=$CudaArchitectures",
        "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures")
}
if ($Diagnostics) { $configure += '-DHMI_BUILD_GGML_KERNEL_PROBE=ON' }
if ($BuildCudnnSdk) {
    $configure += @('-DHMI_AUDIOCPP_SDK_CUDA=ON', '-DHMI_AUDIOCPP_SDK_CUDNN=ON')
}
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
$buildArguments = @('--build', $BuildDirectory, '--config', $Configuration, '--parallel', $Jobs)
foreach ($target in $Targets) {
    if ($target -notmatch '^[A-Za-z0-9_][A-Za-z0-9_.-]*$') { throw ('Invalid build target: ' + $target) }
}
if ($Targets.Count) { & $cmake @buildArguments --target @Targets }
else { & $cmake @buildArguments }
if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }
if ($Diagnostics) {
    & $cmake @buildArguments --target hmi_ggml_probe hmi_ggml_backend_ops hmi_app_startup_probe hmi_cudnn_kernel_probe hmi_audio_frontend_probe hmi_voice_probe hmi_tts_recovery_probe hmi_tts_quality_probe
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic build failed.' }
}
if ($Test) {
    & $ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
if ($useCcache -and $Generator -eq 'Ninja') {
    & $Ccache --config-path (Join-Path $BuildDirectory '.ccache\ccache.conf') --dir (Join-Path $BuildDirectory '.ccache') --show-stats
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read build cache statistics.' }
}
$executable = if ($Generator -eq 'Ninja') { 'bin\HMI.exe' } else { 'bin\' + $Configuration + '\HMI.exe' }
if ($Targets.Count) { Write-Host ('Built targets: ' + ($Targets -join ', ')) }
else { Write-Host ('Application: ' + (Join-Path $BuildDirectory $executable)) }
