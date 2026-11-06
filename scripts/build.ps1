param(
    [switch]$Test,
    [switch]$ConfigureOnly,
    [switch]$Experiments,
    [string]$QtPrefix = $env:ADE_QT_PREFIX,
    [string]$ToolchainDirectory = $env:ADE_TOOLCHAIN_DIRECTORY,
    [string]$OpenSslRoot = $env:ADE_OPENSSL_ROOT,
    [string]$BuildDirectory = 'build',
    [ValidateRange(1, 64)][int]$Jobs = 3
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
# Compatibility defaults for the original development machine. Other machines
# can pass paths directly or set the three ADE_* environment variables above.
if (!$QtPrefix) { $QtPrefix = 'C:/Qt/6.11.2-mingw64-static' }
if (!$ToolchainDirectory) { $ToolchainDirectory = 'C:/mingw64/bin' }
if (!$OpenSslRoot) { $OpenSslRoot = 'C:/Qt/openssl-3.5.8-mingw64-static' }
foreach ($directory in @($QtPrefix, $ToolchainDirectory, $OpenSslRoot)) {
    if (!(Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Missing build dependency directory: $directory. Pass -QtPrefix, -ToolchainDirectory and -OpenSslRoot for your installation."
    }
}
$QtPrefix = (Resolve-Path -LiteralPath $QtPrefix).Path
$ToolchainDirectory = (Resolve-Path -LiteralPath $ToolchainDirectory).Path
$OpenSslRoot = (Resolve-Path -LiteralPath $OpenSslRoot).Path
$cmake = Join-Path $ToolchainDirectory 'cmake.exe'
$ctest = Join-Path $ToolchainDirectory 'ctest.exe'
$compiler = Join-Path $ToolchainDirectory 'g++.exe'
$cCompiler = Join-Path $ToolchainDirectory 'gcc.exe'
$ninja = Join-Path $ToolchainDirectory 'ninja.exe'
foreach ($program in @($cmake, $ctest, $compiler, $cCompiler, $ninja)) {
    if (!(Test-Path -LiteralPath $program -PathType Leaf)) { throw "Missing build tool: $program" }
}
if (![IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$originalPath = $env:PATH
try {
    $env:PATH = "$ToolchainDirectory;$(Join-Path $QtPrefix 'bin');$originalPath"
    $configure = @('-S', $projectRoot, '-B', $BuildDirectory, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$compiler",
        "-DCMAKE_C_COMPILER=$cCompiler", "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_PREFIX_PATH=$QtPrefix", "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        '-DOPENSSL_USE_STATIC_LIBS=TRUE')
    $experimentMode = if ($Experiments) { 'ON' } else { 'OFF' }
    $configure += "-DADE_BUILD_EXPERIMENTS=$experimentMode"
    & $cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    if ($ConfigureOnly) { return }
    & $cmake --build $BuildDirectory --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    if ($Test) {
        & $ctest --test-dir $BuildDirectory --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            $failedTestsPath = Join-Path $BuildDirectory 'Testing/Temporary/LastTestsFailed.log'
            if (Test-Path -LiteralPath $failedTestsPath) {
                foreach ($failedTest in Get-Content -LiteralPath $failedTestsPath) {
                    if ($failedTest -match '^\d+:(native_[a-z0-9_]+)$') {
                        $detailsPath = Join-Path $BuildDirectory ("Testing/{0}.txt" -f $Matches[1])
                        if (Test-Path -LiteralPath $detailsPath) {
                            Write-Host "Test details: $detailsPath"
                            Get-Content -LiteralPath $detailsPath
                        }
                    }
                }
            }
            throw 'Native tests failed'
        }
    }
} finally { $env:PATH = $originalPath }
