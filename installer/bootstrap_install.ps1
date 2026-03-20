param(
    [Parameter(Mandatory = $true)]
    [string]$Repo,
    [string]$Tag = "latest",
    [string]$AssetName = "",
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Sicht"),
    [switch]$SystemWide,
    [switch]$NoPath,
    [switch]$NoEnv,
    [switch]$NoFileAssociation,
    [switch]$CleanInstall,
    [string]$GitHubToken = ""
)

$ErrorActionPreference = "Stop"

function Write-Info($msg) { Write-Output "[info] $msg" }
function Write-Ok($msg) { Write-Output "[ok] $msg" }
function Write-Fail($msg) { Write-Output "[fail] $msg" }

function Test-PathInList {
    param([string]$Needle, [string]$List)
    if (-not $List) { return $false }
    $needleNorm = $Needle.Trim().ToLowerInvariant()
    foreach ($entry in ($List -split ";")) {
        if ($entry.Trim().ToLowerInvariant() -eq $needleNorm) { return $true }
    }
    return $false
}

function Get-ReleaseInfo {
    param([string]$Repo, [string]$Tag, [string]$Token)

    $headers = @{ "User-Agent" = "SichtInstaller" }
    if ($Token -and $Token.Trim().Length -gt 0) {
        $headers["Authorization"] = "token $Token"
    }

    if ($Tag -eq "latest" -or [string]::IsNullOrWhiteSpace($Tag)) {
        $url = "https://api.github.com/repos/$Repo/releases/latest"
    } else {
        $url = "https://api.github.com/repos/$Repo/releases/tags/$Tag"
    }

    Write-Info "Fetching release info: $url"
    return Invoke-RestMethod -Uri $url -Headers $headers
}

function Find-ZipAsset {
    param($Assets, [string]$AssetName)

    if ($AssetName -and $AssetName.Trim().Length -gt 0) {
        foreach ($a in $Assets) {
            if ($a.name -eq $AssetName) { return $a }
        }
        return $null
    }

    foreach ($a in $Assets) {
        if ($a.name -like "*.zip") { return $a }
    }
    return $null
}

function Get-PayloadRoot {
    param([string]$ExtractDir)

    $items = Get-ChildItem -Path $ExtractDir -Force
    $dirs = $items | Where-Object { $_.PSIsContainer }
    $files = $items | Where-Object { -not $_.PSIsContainer }

    $root = $ExtractDir
    if ($dirs.Count -eq 1 -and $files.Count -eq 0) {
        $root = $dirs[0].FullName
    }

    $binExe = Join-Path $root "bin\sicht.exe"
    if (Test-Path $binExe) { return $root }

    $exe = Join-Path $root "sicht.exe"
    if (Test-Path $exe) { return $root }

    $found = Get-ChildItem -Path $root -Recurse -Filter "sicht.exe" | Select-Object -First 1
    if ($found) {
        $parent = Split-Path -Parent $found.FullName
        if ((Split-Path -Leaf $parent) -eq "bin") {
            return (Split-Path -Parent $parent)
        }
        return $parent
    }

    return $root
}

# Enforce TLS 1.2+ for GitHub
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13

if ($SystemWide) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
    if (-not $admin) {
        throw "SystemWide install requested, but the script is not running as Administrator."
    }
}

$release = $null
try {
    $release = Get-ReleaseInfo -Repo $Repo -Tag $Tag -Token $GitHubToken
} catch {
    Write-Fail "Release lookup failed: $($_.Exception.Message)"
    throw
}

if (-not $release) {
    throw "No release info returned."
}

$asset = Find-ZipAsset -Assets $release.assets -AssetName $AssetName
if (-not $asset) {
    $zipball = $release.zipball_url
    if (-not $zipball) { throw "No zip asset or zipball available for this release." }
    Write-Info "No zip asset selected; falling back to release zipball."
    $downloadUrl = $zipball
    $assetName = "release.zip"
} else {
    $downloadUrl = $asset.browser_download_url
    $assetName = $asset.name
}

if (-not $downloadUrl) { throw "No download URL resolved." }

$tempRoot = Join-Path $env:TEMP "sicht_install"
if (Test-Path $tempRoot) { Remove-Item -Recurse -Force $tempRoot }
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

$zipPath = Join-Path $tempRoot $assetName
Write-Info "Downloading $downloadUrl"
Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -UseBasicParsing

$extractDir = Join-Path $tempRoot "extract"
New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
Write-Info "Extracting $assetName"
Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

$payloadRoot = Get-PayloadRoot -ExtractDir $extractDir
Write-Info "Payload root: $payloadRoot"

if ($CleanInstall -and (Test-Path $InstallDir)) {
    Write-Info "Removing existing install at $InstallDir"
    Remove-Item -Recurse -Force $InstallDir
}

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item -Path (Join-Path $payloadRoot "*") -Destination $InstallDir -Recurse -Force
Write-Ok "Installed to $InstallDir"

$envKey = if ($SystemWide) { "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" } else { "HKCU:\Environment" }
$envScopeLabel = if ($SystemWide) { "system" } else { "user" }

$binPath = Join-Path $InstallDir "bin"
if (-not $NoPath) {
    $currentPath = (Get-ItemProperty -Path $envKey -Name Path -ErrorAction SilentlyContinue).Path
    if (-not (Test-PathInList -Needle $binPath -List $currentPath)) {
        $newPath = if ([string]::IsNullOrWhiteSpace($currentPath)) { $binPath } else { "$currentPath;$binPath" }
        Set-ItemProperty -Path $envKey -Name Path -Value $newPath
        $env:Path = "$env:Path;$binPath"
        Write-Ok "Added to $envScopeLabel PATH: $binPath"
    } else {
        Write-Info "$envScopeLabel PATH already contains: $binPath"
    }
}

if (-not $NoEnv) {
    Set-ItemProperty -Path $envKey -Name SICHT_RUNTIME_ROOT -Value $InstallDir
    Set-ItemProperty -Path $envKey -Name SICHT_LIB_ROOT -Value $InstallDir
    Write-Ok "Configured SICHT_RUNTIME_ROOT and SICHT_LIB_ROOT"
}

if (-not $NoFileAssociation) {
    $registerScript = Join-Path $InstallDir "register_si_filetype.ps1"
    if (-not (Test-Path $registerScript)) {
        $registerScript = Join-Path $InstallDir "installer\register_si_filetype.ps1"
    }
    if (Test-Path $registerScript) {
        Write-Info "Registering .si file association"
        & $registerScript -ProjectRoot $InstallDir | Out-Null
        Write-Ok "File association registered"
    } else {
        Write-Info "No register_si_filetype.ps1 found; skipping file association."
    }
}

Write-Ok "Install complete"
