[CmdletBinding()]
param(
    [ValidateSet('full', 'recorder-only')]
    [string]$Mode = 'full',
    [ValidateSet('Win32', 'x64')]
    [string]$Architecture = 'x64',
    [string]$BuildType = 'Release',
    [string]$Generator = '',
    [string[]]$CMakeOption = @()
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root "build-win-$Mode"
$LastBuild = Join-Path $Root '.meteoris-last-build-windows'

# Fail early if this script is not sitting at the Meteoris repository root.
$RootCMake = Join-Path $Root 'CMakeLists.txt'
$RootVersion = Join-Path $Root 'VERSION'
$RecorderSource = Join-Path $Root 'src\meteoris.cpp'
if (-not (Test-Path -LiteralPath $RootCMake -PathType Leaf) -or
    -not (Test-Path -LiteralPath $RootVersion -PathType Leaf) -or
    -not (Test-Path -LiteralPath $RecorderSource -PathType Leaf)) {
    throw "build.ps1 must be run from the Meteoris repository root. Expected CMakeLists.txt, VERSION and src\meteoris.cpp under: $Root"
}

# A build directory can retain the source directory it was first configured
# with. If an older build-win-* tree was accidentally configured from sim\,
# discard only that generated build tree and configure it again from $Root.
$CacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $CacheFile -PathType Leaf) {
    $homeLine = Get-Content -LiteralPath $CacheFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } |
        Select-Object -First 1
    if ($homeLine) {
        $cachedSource = ($homeLine -split '=', 2)[1]
        $expectedSource = (Resolve-Path -LiteralPath $Root).Path
        try {
            $cachedResolved = (Resolve-Path -LiteralPath $cachedSource -ErrorAction Stop).Path
        } catch {
            $cachedResolved = $cachedSource
        }
        if (-not [string]::Equals($cachedResolved, $expectedSource, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Warning "Removing stale build directory configured from '$cachedSource' instead of '$expectedSource'."
            Remove-Item -LiteralPath $BuildDir -Recurse -Force
        }
    }
}

function Test-CMakeOptionDefined {
    param([Parameter(Mandatory = $true)][string]$Name)
    foreach ($option in $CMakeOption) {
        if ($option -match "^-D$([regex]::Escape($Name))(?::[^=]+)?=") {
            return $true
        }
    }
    return $false
}


function Find-CMakePackageDir {
    param(
        [Parameter(Mandatory = $true)][string[]]$SearchRoots,
        [Parameter(Mandatory = $true)][string[]]$ConfigNames
    )

    foreach ($searchRoot in $SearchRoots) {
        if ([string]::IsNullOrWhiteSpace($searchRoot)) { continue }
        if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) { continue }

        foreach ($configName in $ConfigNames) {
            # Check the root itself first, then common CMake package locations,
            # and finally recurse a few directory levels through local build trees.
            foreach ($candidate in @(
                (Join-Path $searchRoot $configName),
                (Join-Path $searchRoot "lib\\cmake\\spdlog\\$configName"),
                (Join-Path $searchRoot "cmake\\spdlog\\$configName"),
                (Join-Path $searchRoot "share\\spdlog\\$configName")
            )) {
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return (Split-Path -Parent (Resolve-Path -LiteralPath $candidate).Path)
                }
            }
        }

        $match = Get-ChildItem -LiteralPath $searchRoot -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $ConfigNames -contains $_.Name } |
            Select-Object -First 1
        if ($match) {
            return $match.Directory.FullName
        }
    }
    return $null
}

function Add-ExistingPrefix {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$List,
        [string]$Path
    )
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not $List.Contains($resolved)) {
        $List.Add($resolved)
    }
}

# CMake package discovery on Windows.
# Explicit -DCMAKE_PREFIX_PATH=... always wins. Otherwise merge the user's
# environment with common install prefixes used by spdlog, HDF5 and SoapySDR.
if (-not (Test-CMakeOptionDefined 'CMAKE_PREFIX_PATH')) {
    $prefixes = [System.Collections.Generic.List[string]]::new()

    if ($env:CMAKE_PREFIX_PATH) {
        foreach ($prefix in ($env:CMAKE_PREFIX_PATH -split ';')) {
            Add-ExistingPrefix $prefixes $prefix
        }
    }

    foreach ($prefix in @($env:SPDLOG_ROOT, $env:HDF5_ROOT, $env:SoapySDR_ROOT, $env:SOAPY_SDR_ROOT)) {
        Add-ExistingPrefix $prefixes $prefix
    }

    $programFilesRoots = @(
        $env:ProgramFiles,
        [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    ) | Where-Object { $_ } | Select-Object -Unique

    foreach ($programFiles in $programFilesRoots) {
        Add-ExistingPrefix $prefixes (Join-Path $programFiles 'spdlog')
        Add-ExistingPrefix $prefixes (Join-Path $programFiles 'SoapySDR')
        Add-ExistingPrefix $prefixes (Join-Path $programFiles 'PothosSDR')

        # The official HDF5 Windows installer normally uses
        # Program Files\HDF_Group\HDF5\<version>. Add each installed version.
        $hdfBase = Join-Path $programFiles 'HDF_Group\HDF5'
        if (Test-Path -LiteralPath $hdfBase -PathType Container) {
            Get-ChildItem -LiteralPath $hdfBase -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                Add-ExistingPrefix $prefixes $_.FullName
            }
        }
    }

    # Also support a conventional developer dependency tree and source/build
    # trees immediately beside Meteoris. This keeps local development simple.
    foreach ($prefix in @(
        'C:\deps\spdlog',
        'C:\deps\hdf5',
        'C:\deps\HDF5',
        'C:\deps\SoapySDR',
        'C:\deps\PothosSDR',
        (Join-Path $Root '..\spdlog\install'),
        (Join-Path $Root '..\spdlog\build'),
        (Join-Path $Root '..\SoapySDR\install'),
        (Join-Path $Root '..\SoapySDR\build')
    )) {
        Add-ExistingPrefix $prefixes $prefix
    }

    if ($prefixes.Count -gt 0) {
        $detectedPrefixPath = $prefixes -join ';'
        $argsListPrefix = "-DCMAKE_PREFIX_PATH=$detectedPrefixPath"
        Write-Host 'CMake dependency prefixes:'
        foreach ($prefix in $prefixes) {
            Write-Host "  $prefix"
        }
    }
}


# Prefer a valid installed spdlog header tree on Windows.  This deliberately
# bypasses spdlogConfig.cmake when possible: spdlog package files generated
# from a build tree are sometimes non-relocatable and can retain references to
# a source directory that has since been moved or deleted.
if (-not (Test-CMakeOptionDefined 'METEORIS_SPDLOG_INCLUDE_DIR') -and
    -not (Test-CMakeOptionDefined 'spdlog_DIR')) {
    $spdlogHeaderRoots = [System.Collections.Generic.List[string]]::new()

    if ($env:SPDLOG_ROOT) {
        Add-ExistingPrefix $spdlogHeaderRoots $env:SPDLOG_ROOT
    }
    if ($env:ProgramFiles) {
        Add-ExistingPrefix $spdlogHeaderRoots (Join-Path $env:ProgramFiles 'spdlog')
    }
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($programFilesX86) {
        Add-ExistingPrefix $spdlogHeaderRoots (Join-Path $programFilesX86 'spdlog')
    }
    Add-ExistingPrefix $spdlogHeaderRoots 'C:\deps\spdlog'
    Add-ExistingPrefix $spdlogHeaderRoots (Join-Path $Root '..\spdlog')

    foreach ($spdlogRoot in $spdlogHeaderRoots) {
        foreach ($includeDir in @(
            (Join-Path $spdlogRoot 'include'),
            $spdlogRoot
        )) {
            $header = Join-Path $includeDir 'spdlog\spdlog.h'
            if (Test-Path -LiteralPath $header -PathType Leaf) {
                $resolvedInclude = (Resolve-Path -LiteralPath $includeDir).Path
                Write-Host "spdlog headers: $resolvedInclude (header-only mode)"
                $argsListSpdlog = "-DMETEORIS_SPDLOG_INCLUDE_DIR=$resolvedInclude"
                break
            }
        }
        if ($argsListSpdlog) { break }
    }

    # If no usable header tree is present, fall back to normal CMake package
    # discovery.  This keeps Linux and custom Windows package installations
    # working exactly as before.
    if (-not $argsListSpdlog) {
        $spdlogSearchRoots = [System.Collections.Generic.List[string]]::new()
        Add-ExistingPrefix $spdlogSearchRoots $env:SPDLOG_ROOT
        if ($env:ProgramFiles) {
            Add-ExistingPrefix $spdlogSearchRoots (Join-Path $env:ProgramFiles 'spdlog')
        }
        if ($programFilesX86) {
            Add-ExistingPrefix $spdlogSearchRoots (Join-Path $programFilesX86 'spdlog')
        }
        Add-ExistingPrefix $spdlogSearchRoots 'C:\deps\spdlog'
        Add-ExistingPrefix $spdlogSearchRoots (Join-Path $Root '..\spdlog')

        $detectedSpdlogDir = Find-CMakePackageDir -SearchRoots $spdlogSearchRoots.ToArray() -ConfigNames @('spdlogConfig.cmake', 'spdlog-config.cmake', 'spdlog.cps')
        if ($detectedSpdlogDir) {
            Write-Host "spdlog package: $detectedSpdlogDir"
            $argsListSpdlog = "-Dspdlog_DIR=$detectedSpdlogDir"
        }
    }
}

$argsList = @('-S', $Root, '-B', $BuildDir,
              "-DCMAKE_BUILD_TYPE=$BuildType",
              '-DMETEORIS_INSTALL_RECOVER_HDF5=ON',
              '-DMETEORIS_INSTALL_CONFIG_TOOL=ON')

if ($argsListPrefix) {
    $argsList += $argsListPrefix
}

if ($argsListSpdlog) {
    $argsList += $argsListSpdlog
}

if ($Mode -eq 'full') {
    $argsList += @('-DMETEORIS_INSTALL_PLOT=ON', '-DMETEORIS_BUILD_SIM=ON')
} else {
    $argsList += @('-DMETEORIS_INSTALL_PLOT=OFF', '-DMETEORIS_BUILD_SIM=OFF')
}

if ($Generator) {
    $argsList += @('-G', $Generator)
    if ($Generator -match 'Visual Studio') {
        $argsList += @('-A', $Architecture)
    }
} elseif ($Architecture -eq 'Win32') {
    throw 'Win32 requires an explicit Visual Studio generator, for example -Generator "Visual Studio 17 2022".'
}

if ($env:VCPKG_ROOT -and -not ($CMakeOption -match 'CMAKE_TOOLCHAIN_FILE')) {
    $toolchain = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
    if (Test-Path $toolchain) {
        $argsList += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    }
}

$argsList += $CMakeOption
Write-Host "Source dir:  $Root"
Write-Host "Build mode:   $Mode"
Write-Host "Architecture: $Architecture"
Write-Host "Build dir:    $BuildDir"
& cmake @argsList
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $BuildDir --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Set-Content -Path $LastBuild -Value $BuildDir -NoNewline
Write-Host "`nBuild complete. install.ps1 will reuse: $BuildDir"
