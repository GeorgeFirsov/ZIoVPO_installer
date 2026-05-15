param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$SkipBuild,
    [string]$InnoSetupCompiler = ""
)

$ErrorActionPreference = "Stop"

$InstallerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $InstallerDir
$ApplicationDir = Join-Path $RootDir "Application"
$ServiceDir = Join-Path $RootDir "Service"
$PayloadDir = Join-Path $InstallerDir "payload"
$OutputDir = Join-Path $InstallerDir "output"

$ApplicationProject = Join-Path $ApplicationDir "Application.vcxproj"
$ServiceProject = Join-Path $ServiceDir "Service.vcxproj"

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($path -and (Test-Path $path)) {
            return $path
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "MSBuild.exe not found. Open Developer PowerShell for Visual Studio or install Visual Studio Build Tools."
}

function Find-ISCC {
    param([string]$ExplicitPath)

    if ($ExplicitPath -and (Test-Path $ExplicitPath)) {
        return $ExplicitPath
    }

    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 5\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "ISCC.exe not found. Install Inno Setup or pass -InnoSetupCompiler 'C:\Path\To\ISCC.exe'."
}

function Build-Project {
    param([string]$ProjectPath)

    if (!(Test-Path $ProjectPath)) {
        throw "Project file not found: $ProjectPath"
    }

    $msbuild = Find-MSBuild
    Write-Host "Building $ProjectPath ($Platform|$Configuration)..."
    & $msbuild $ProjectPath /m /p:Configuration=$Configuration /p:Platform=$Platform
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed: $ProjectPath"
    }
}

function Find-BuiltExe {
    param(
        [string]$ProjectDir,
        [string[]]$ExeNames
    )

    $all = @()
    foreach ($exeName in $ExeNames) {
        $all += Get-ChildItem -Path $ProjectDir -Recurse -File -Filter $exeName -ErrorAction SilentlyContinue
    }

    if ($all.Count -eq 0) {
        throw "Built exe not found in $ProjectDir. Expected: $($ExeNames -join ', ')"
    }

    $preferred = $all | Where-Object {
        $_.FullName -like "*$Platform*$Configuration*" -or
        $_.FullName -like "*$Configuration*$Platform*" -or
        $_.FullName -like "*$Platform\$Configuration*" -or
        $_.FullName -like "*$Configuration\$Platform*"
    } | Sort-Object LastWriteTime -Descending | Select-Object -First 1

    if ($preferred) {
        return $preferred
    }

    return ($all | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
}

function Copy-OutputDirectory {
    param(
        [System.IO.FileInfo]$ExeFile,
        [string]$DestinationDir
    )

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    $sourceDir = $ExeFile.Directory.FullName

    Get-ChildItem -Path $sourceDir -File | Where-Object {
        $_.Extension -in @('.exe', '.dll', '.json', '.config', '.xml', '.dat', '.bin', '.pem', '.ico', '.png')
    } | ForEach-Object {
        Copy-Item $_.FullName -Destination (Join-Path $DestinationDir $_.Name) -Force
    }
}

if (!$SkipBuild) {
    Build-Project $ServiceProject
    Build-Project $ApplicationProject
}

if (Test-Path $PayloadDir) {
    Remove-Item $PayloadDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $PayloadDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$servicePayload = Join-Path $PayloadDir "Service"
$appPayload = Join-Path $PayloadDir "Application"
$redistPayload = Join-Path $PayloadDir "redist"

$serviceExe = Find-BuiltExe -ProjectDir $ServiceDir -ExeNames @('Service.exe', 'ZIoVPO_service.exe')
$appExe = Find-BuiltExe -ProjectDir $ApplicationDir -ExeNames @('Application.exe')

Copy-OutputDirectory -ExeFile $serviceExe -DestinationDir $servicePayload
Copy-OutputDirectory -ExeFile $appExe -DestinationDir $appPayload

# The installed service executable has a stable name, even if Visual Studio produced Service.exe.
Copy-Item $serviceExe.FullName -Destination (Join-Path $servicePayload "ZIoVPO_service.exe") -Force
Copy-Item $appExe.FullName -Destination (Join-Path $appPayload "Application.exe") -Force

$publicKey = Join-Path $ServiceDir "signature_public.pem"
if (Test-Path $publicKey) {
    Copy-Item $publicKey -Destination (Join-Path $servicePayload "signature_public.pem") -Force
} else {
    throw "signature_public.pem not found in Service directory."
}

$defaultDb = Join-Path $ServiceDir "DefaultAvDb"
if (Test-Path $defaultDb) {
    Copy-Item $defaultDb -Destination (Join-Path $servicePayload "DefaultAvDb") -Recurse -Force
} else {
    throw "DefaultAvDb directory not found in Service directory."
}

$redistSource = Join-Path $InstallerDir "redist\VC_redist.x64.exe"
if (Test-Path $redistSource) {
    New-Item -ItemType Directory -Force -Path $redistPayload | Out-Null
    Copy-Item $redistSource -Destination (Join-Path $redistPayload "VC_redist.x64.exe") -Force
} else {
    Write-Warning "VC_redist.x64.exe not found in Installer\redist. The installer will be built without VC++ Runtime redistributable."
}

$iscc = Find-ISCC -ExplicitPath $InnoSetupCompiler
$script = Join-Path $InstallerDir "installer.iss"

Write-Host "Compiling installer..."
& $iscc $script
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compiler failed."
}

Write-Host "Done. Installer output:"
Get-ChildItem -Path $OutputDir -Filter "*.exe" | ForEach-Object { Write-Host $_.FullName }
