# Installs the pinned Vulkan SDK into External\VulkanSDK on first use and prints its folder, so a build needs no SDK installed system-wide.
$ErrorActionPreference = 'Stop'

$Version = '1.4.363.0'
$Url = "https://sdk.lunarg.com/sdk/download/$Version/windows/vulkansdk-windows-X64-$Version.exe"
$Sha256 = '94A82D378F7A5E3E54C9DB7D2FB7016AF136E14AC0A18DBF0F2F67A36352D141'

$Root = Join-Path (Split-Path -Parent $PSScriptRoot) "External\VulkanSDK"
$InstallDirectory = Join-Path $Root $Version
$Required = @('Include\vulkan\vulkan.h', 'Lib\vulkan-1.lib', 'Bin\slangc.exe')
# .NET hashing, since Get-FileHash is missing when Windows PowerShell starts from PowerShell 7.
$HashOf = { param($File) $Stream = [IO.File]::OpenRead($File); try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($Stream)).Replace('-', '') } finally { $Stream.Close() } }
$IsInstalled = { param($Directory) -not ($Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Directory $_) -PathType Leaf) }) }

if (-not (& $IsInstalled $InstallDirectory)) {
    if (Test-Path -LiteralPath $InstallDirectory) {
        throw "The Vulkan SDK folder exists but is incomplete: $InstallDirectory"
    }

    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $Installer = Join-Path $Root "vulkansdk-windows-X64-$Version.exe"
    if (-not (Test-Path -LiteralPath $Installer) -or (& $HashOf $Installer) -ne $Sha256) {
        [Console]::Error.WriteLine("Downloading the Vulkan SDK $Version...")
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest $Url -OutFile $Installer
        if ((& $HashOf $Installer) -ne $Sha256) {
            Remove-Item -LiteralPath $Installer -Force
            throw 'Vulkan SDK installer hash mismatch'
        }
    }

    # copy_only unpacks the files without touching the registry or the environment; a temporary folder keeps an interrupted install from looking complete.
    $Temporary = Join-Path $Root ".$Version.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        & $Installer --root $Temporary --accept-licenses --default-answer --confirm-command install copy_only=1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "The Vulkan SDK installer exited with code $LASTEXITCODE."
        }

        if (-not (& $IsInstalled $Temporary)) {
            throw 'The Vulkan SDK installer finished without the headers, loader library or slangc.'
        }

        Move-Item -LiteralPath $Temporary -Destination $InstallDirectory
    }
    finally {
        if (Test-Path -LiteralPath $Temporary) {
            Remove-Item -LiteralPath $Temporary -Recurse -Force
        }
    }

    Remove-Item -LiteralPath $Installer -Force
}

Write-Output $InstallDirectory
