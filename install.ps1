[CmdletBinding(DefaultParameterSetName='Local')]
param(
    [Parameter(ParameterSetName='Local')]
    [switch]$Local,
    [Parameter(ParameterSetName='System', Mandatory=$true)]
    [switch]$System,
    [string]$Prefix = '',
    [string]$BuildDir = '',
    [string]$BuildType = 'Release',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$LastBuild = Join-Path $Root '.meteoris-last-build-windows'

if (-not $Prefix) {
    if ($System) {
        $Prefix = Join-Path $env:ProgramFiles 'Meteoris'
    } else {
        $Prefix = Join-Path $env:LOCALAPPDATA 'Meteoris'
    }
}

if (-not $BuildDir) {
    if (Test-Path $LastBuild) {
        $BuildDir = (Get-Content $LastBuild -Raw).Trim()
        Write-Host "Reusing last successful build: $BuildDir"
    } else {
        & (Join-Path $Root 'build.ps1') -Mode full -BuildType $BuildType
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $BuildDir = (Get-Content $LastBuild -Raw).Trim()
    }
} elseif (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $Root $BuildDir
}

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    throw "Build directory is not configured: $BuildDir"
}

if ($Rebuild) {
    & cmake --build $BuildDir --config $BuildType --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& cmake --install $BuildDir --config $BuildType --prefix $Prefix
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Make installed commands discoverable by name.  System installs update the
# machine PATH; local installs update the current user's PATH.  Also update
# this PowerShell process so the command is available immediately.
$BinDir = Join-Path $Prefix 'bin'
$PathTarget = if ($System) { 'Machine' } else { 'User' }

function Add-ToEnvironmentPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [ValidateSet('User', 'Machine')]
        [string]$Target
    )

    $fullDirectory = [IO.Path]::GetFullPath($Directory).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $current = [Environment]::GetEnvironmentVariable('Path', $Target)
    $entries = @()
    if ($current) {
        $entries = @($current -split ';' | Where-Object { $_ -and $_.Trim() })
    }

    $alreadyPresent = $false
    foreach ($entry in $entries) {
        try {
            $normalizedEntry = [IO.Path]::GetFullPath($entry.Trim()).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
            if ($normalizedEntry -ieq $fullDirectory) {
                $alreadyPresent = $true
                break
            }
        } catch {
            if ($entry.Trim().TrimEnd('\', '/') -ieq $fullDirectory) {
                $alreadyPresent = $true
                break
            }
        }
    }

    if (-not $alreadyPresent) {
        $newPath = if ($current -and $current.Trim()) { "$current;$fullDirectory" } else { $fullDirectory }
        [Environment]::SetEnvironmentVariable('Path', $newPath, $Target)
        Write-Host "Added to $Target PATH: $fullDirectory"
    } else {
        Write-Host "Already in $Target PATH: $fullDirectory"
    }

    $processEntries = @($env:Path -split ';' | Where-Object { $_ -and $_.Trim() })
    $processHasDir = $false
    foreach ($entry in $processEntries) {
        try {
            if ([IO.Path]::GetFullPath($entry.Trim()).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) -ieq $fullDirectory) {
                $processHasDir = $true
                break
            }
        } catch { }
    }
    if (-not $processHasDir) {
        $env:Path = "$env:Path;$fullDirectory"
    }
}

Add-ToEnvironmentPath -Directory $BinDir -Target $PathTarget

Write-Host "`nInstalled from: $BuildDir"
Write-Host "Install prefix: $Prefix"
Write-Host "  $Prefix\bin\meteoris.exe"
Write-Host "  $Prefix\bin\meteoris_config.cmd"
Write-Host "  $Prefix\bin\meteoris_recover_hdf5.cmd"
