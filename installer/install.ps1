param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot ".."),
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Sicht"),
    [switch]$NoPath,
    [switch]$NoFileAssociation,
    [switch]$NoEnv,
    [switch]$SystemWide
)

$ErrorActionPreference = "Stop"

function Test-PathInList {
    param(
        [string]$Needle,
        [string]$List
    )
    if (-not $List) {
        return $false
    }
    $needleNorm = $Needle.Trim().ToLowerInvariant()
    foreach ($entry in ($List -split ";")) {
        if ($entry.Trim().ToLowerInvariant() -eq $needleNorm) {
            return $true
        }
    }
    return $false
}

function Try-SetEnvValue {
    param(
        [string]$EnvKey,
        [string]$Name,
        [string]$Value,
        [string]$ScopeLabel
    )
    $ok = $true
    try {
        Set-ItemProperty -Path $EnvKey -Name $Name -Value $Value -ErrorAction Stop
    } catch {
        $ok = $false
        Write-Output "Warning: Could not set $Name in $ScopeLabel environment (insufficient permissions). Update manually or run PowerShell as admin."
    }
    return $ok
}

function Find-BundledSysroot {
    param(
        [string]$LlvmRoot
    )

    if (-not $LlvmRoot -or -not (Test-Path $LlvmRoot)) {
        return $null
    }

    $sysrootCandidates = @(
        "mingw",
        "llvm-mingw",
        "sysroot",
        "x86_64-w64-mingw32",
        "LLVM\mingw",
        "LLVM\llvm-mingw",
        "LLVM\sysroot",
        "LLVM\x86_64-w64-mingw32"
    )
    $headerProbes = @(
        "usr\include\stdio.h",
        "include\stdio.h",
        "x86_64-w64-mingw32\include\stdio.h"
    )

    foreach ($candidate in $sysrootCandidates) {
        $root = Join-Path $LlvmRoot $candidate
        if (-not (Test-Path $root)) {
            continue
        }
        foreach ($probe in $headerProbes) {
            if (Test-Path (Join-Path $root $probe)) {
                return $root
            }
        }
    }

    return $null
}

$src = (Resolve-Path $SourceRoot).Path

if ($SystemWide -and -not $PSBoundParameters.ContainsKey("InstallDir")) {
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    if ([string]::IsNullOrWhiteSpace($programFiles)) {
        $programFiles = $env:ProgramFiles
    }
    if ([string]::IsNullOrWhiteSpace($programFiles)) {
        $programFiles = "C:\Program Files"
    }
    $InstallDir = Join-Path $programFiles "Sicht"
}

$target = [System.IO.Path]::GetFullPath($InstallDir)

$required = @("bin\sicht.exe")

foreach ($name in $required) {
    $p = Join-Path $src $name
    if (-not (Test-Path $p)) {
        throw "Missing required file: $p"
    }
}

$registerScriptSource = Join-Path $src "installer\register_si_filetype.ps1"
if (-not (Test-Path $registerScriptSource)) {
    throw "Missing required file: $registerScriptSource"
}

New-Item -ItemType Directory -Path $target -Force | Out-Null

$copyFiles = @(
    "bin\sicht.exe",
    "image.ico",
    "README.md",
    "README.windows.md"
)

foreach ($name in $copyFiles) {
    $from = Join-Path $src $name
    if (Test-Path $from) {
        $dest = Join-Path $target $name
        $destDir = Split-Path -Parent $dest
        if ($destDir -and -not (Test-Path $destDir)) {
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
        }
        Copy-Item -Path $from -Destination $dest -Force
    }
}

$windowsReadme = Join-Path $target "README.windows.md"
if (Test-Path $windowsReadme) {
    Copy-Item -Path $windowsReadme -Destination (Join-Path $target "README.md") -Force
}

$exampleSrc = Join-Path $src "examples\example.si"
if (Test-Path $exampleSrc) {
    Copy-Item -Path $exampleSrc -Destination (Join-Path $target "example.si") -Force
}

$licenseSrc = Join-Path $src "docs\License.txt"
if (Test-Path $licenseSrc) {
    Copy-Item -Path $licenseSrc -Destination (Join-Path $target "License.txt") -Force
}

Copy-Item -Path $registerScriptSource -Destination (Join-Path $target "register_si_filetype.ps1") -Force

$docsSrc = Join-Path $src "docs\Sicht Documentation.odt"
if (Test-Path $docsSrc) {
    $docsDstDir = Join-Path $target "docs"
    New-Item -ItemType Directory -Path $docsDstDir -Force | Out-Null
    Copy-Item -Path $docsSrc -Destination (Join-Path $docsDstDir "Sicht Documentation.odt") -Force
}

$runtimeDirs = @("src", "libs")
foreach ($dirName in $runtimeDirs) {
    $fromDir = Join-Path $src $dirName
    if (Test-Path $fromDir) {
        $toDir = Join-Path $target $dirName
        if (Test-Path $toDir) {
            Remove-Item -Recurse -Force $toDir
        }
        Copy-Item -Path $fromDir -Destination $toDir -Recurse -Force
    }
}
Write-Output "Installed runtime pack (src/, libs/) for LLVM native compile mode and stdlib loading."

$envKey = if ($SystemWide) { "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" } else { "HKCU:\Environment" }
$envScopeLabel = if ($SystemWide) { "system" } else { "user" }

$binTarget = Join-Path $target "bin"
if (-not $NoPath -and -not $NoEnv) {
    $currentPath = (Get-ItemProperty -Path $envKey -Name Path -ErrorAction SilentlyContinue).Path
    if (-not (Test-PathInList -Needle $binTarget -List $currentPath)) {
        $newPath = if ([string]::IsNullOrWhiteSpace($currentPath)) { $binTarget } else { "$currentPath;$binTarget" }
        $pathUpdated = Try-SetEnvValue -EnvKey $envKey -Name Path -Value $newPath -ScopeLabel $envScopeLabel
        $env:Path = "$env:Path;$binTarget"
        if ($pathUpdated) {
            Write-Output "Added to $envScopeLabel PATH: $binTarget"
        } else {
            Write-Output "PATH updated for current session only: $binTarget"
        }
    } else {
        Write-Output "$envScopeLabel PATH already contains: $binTarget"
    }
} elseif ($NoEnv) {
    Write-Output "Skipping PATH update (NoEnv specified)."
}

if (-not $NoEnv) {
    $currentRuntimeRoot = (Get-ItemProperty -Path $envKey -Name SICHT_RUNTIME_ROOT -ErrorAction SilentlyContinue).SICHT_RUNTIME_ROOT
    if ($currentRuntimeRoot -ne $target) {
        $ok = Try-SetEnvValue -EnvKey $envKey -Name SICHT_RUNTIME_ROOT -Value $target -ScopeLabel $envScopeLabel
        if ($ok) {
            Write-Output "Configured SICHT_RUNTIME_ROOT: $target"
        }
    } else {
        Write-Output "SICHT_RUNTIME_ROOT already set to: $target"
    }
    $env:SICHT_RUNTIME_ROOT = $target
} else {
    Write-Output "Skipping SICHT_RUNTIME_ROOT (NoEnv specified)."
}

$libsPath = Join-Path $target "libs"
if (Test-Path $libsPath) {
    if (-not $NoEnv) {
        $currentLibRoot = (Get-ItemProperty -Path $envKey -Name SICHT_LIB_ROOT -ErrorAction SilentlyContinue).SICHT_LIB_ROOT
        if ($currentLibRoot -ne $target) {
            $ok = Try-SetEnvValue -EnvKey $envKey -Name SICHT_LIB_ROOT -Value $target -ScopeLabel $envScopeLabel
            if ($ok) {
                Write-Output "Configured SICHT_LIB_ROOT: $target"
            }
        } else {
            Write-Output "SICHT_LIB_ROOT already set to: $target"
        }
        $env:SICHT_LIB_ROOT = $target
    } else {
        Write-Output "Skipping SICHT_LIB_ROOT (NoEnv specified)."
    }
}

$bundledLlvmDir = Join-Path $src "installer\llvm"
$bundledClangSource = $null
$bundledClangRelative = $null
$bundledCandidates = @(
    "bin\clang.exe",
    "LLVM\bin\clang.exe"
)
foreach ($candidate in $bundledCandidates) {
    $candidatePath = Join-Path $bundledLlvmDir $candidate
    if (Test-Path $candidatePath) {
        $bundledClangSource = $candidatePath
        $bundledClangRelative = $candidate
        break
    }
}
$llvmClangTarget = $null
$llvmBinTargetDir = $null
$llvmSysrootTarget = $null

if ($bundledClangSource) {
    $targetLlvmDir = Join-Path $target "llvm"
    if (Test-Path $targetLlvmDir) {
        Remove-Item -Recurse -Force $targetLlvmDir
    }
    Copy-Item -Path $bundledLlvmDir -Destination $targetLlvmDir -Recurse -Force
    $llvmClangTarget = Join-Path $targetLlvmDir $bundledClangRelative
    $llvmBinTargetDir = Split-Path -Parent $llvmClangTarget
    $llvmSysrootTarget = Find-BundledSysroot -LlvmRoot $targetLlvmDir
    Write-Output "Installed bundled LLVM toolchain to: $targetLlvmDir"
} else {
    $clangCmd = Get-Command clang -ErrorAction SilentlyContinue
    if ($clangCmd -and $clangCmd.Source) {
        $llvmClangTarget = $clangCmd.Source
        $llvmBinTargetDir = Split-Path -Parent $llvmClangTarget
        Write-Output "Using system clang: $llvmClangTarget"
    }
}

if ($llvmClangTarget) {
    if (-not $NoEnv) {
        $ok = Try-SetEnvValue -EnvKey $envKey -Name SICHT_LLVM_CLANG -Value $llvmClangTarget -ScopeLabel $envScopeLabel
        $env:SICHT_LLVM_CLANG = $llvmClangTarget
        if ($ok) {
            Write-Output "Configured SICHT_LLVM_CLANG: $llvmClangTarget"
        }

        if ($llvmSysrootTarget) {
            $ok = Try-SetEnvValue -EnvKey $envKey -Name SICHT_LLVM_SYSROOT -Value $llvmSysrootTarget -ScopeLabel $envScopeLabel
            $env:SICHT_LLVM_SYSROOT = $llvmSysrootTarget
            if ($ok) {
                Write-Output "Configured SICHT_LLVM_SYSROOT: $llvmSysrootTarget"
            }
        } elseif ($bundledClangSource) {
            try {
                Remove-ItemProperty -Path $envKey -Name SICHT_LLVM_SYSROOT -ErrorAction Stop
            } catch {
            }
            Remove-Item Env:SICHT_LLVM_SYSROOT -ErrorAction SilentlyContinue
            Write-Output "No bundled sysroot detected. Native LLVM builds may still rely on host C toolchain."
        }
    } else {
        Write-Output "Skipping LLVM env vars (NoEnv specified)."
    }

    if ($llvmBinTargetDir -and (Test-Path $llvmBinTargetDir) -and (-not $NoPath) -and (-not $NoEnv)) {
        $currentPath = (Get-ItemProperty -Path $envKey -Name Path -ErrorAction SilentlyContinue).Path
        if (-not (Test-PathInList -Needle $llvmBinTargetDir -List $currentPath)) {
            $newPath = if ([string]::IsNullOrWhiteSpace($currentPath)) { $llvmBinTargetDir } else { "$currentPath;$llvmBinTargetDir" }
            $pathUpdated = Try-SetEnvValue -EnvKey $envKey -Name Path -Value $newPath -ScopeLabel $envScopeLabel
            $env:Path = "$env:Path;$llvmBinTargetDir"
            if ($pathUpdated) {
                Write-Output "Added LLVM bin to $envScopeLabel PATH: $llvmBinTargetDir"
            } else {
                Write-Output "LLVM bin PATH updated for current session only: $llvmBinTargetDir"
            }
        } else {
            Write-Output "$envScopeLabel PATH already contains LLVM bin: $llvmBinTargetDir"
        }
    } elseif ($NoEnv) {
        Write-Output "Skipping LLVM PATH update (NoEnv specified)."
    }
} else {
    Write-Output "LLVM clang not found. `sicht compile --exe` will fail until clang is installed."
    Write-Output "Set SICHT_LLVM_CLANG (and optionally SICHT_LLVM_SYSROOT) after installing LLVM/clang."
}

if (-not $NoFileAssociation) {
    $assocScript = Join-Path $target "register_si_filetype.ps1"
    $assocHive = if ($SystemWide) { "HKLM" } else { "HKCU" }
    & $assocScript -ProjectRoot $target -RootHive $assocHive
}

Write-Output ""
Write-Output "Sicht installed to: $target"
if (Test-Path (Join-Path $target "example.si")) {
    Write-Output "Run examples with: `"$target\bin\sicht.exe`" run `"$target\example.si`""
} else {
    Write-Output "Run with: `"$target\bin\sicht.exe`" run `"<program>.si`""
}
Write-Output "If PATH was updated, open a new terminal to use: sicht.exe"
