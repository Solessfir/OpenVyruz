@echo off
setlocal

rem Usage: GenerateProjectFiles.bat [vs2026|vs2022]
set "ROOT=%~dp0"
set "ACTION=%~1"
if not defined ACTION set "ACTION=vs2026"

set "PREMAKE_DIR=%ROOT%External\Premake\Windows"
set "PREMAKE=%PREMAKE_DIR%\premake5.exe"
if not exist "%PREMAKE%" (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference = 'Stop';" ^
        "$Zip = Join-Path $env:TEMP 'premake-5.0.0-beta8-windows.zip';" ^
        "Invoke-WebRequest 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-windows.zip' -OutFile $Zip;" ^
        "$Stream = [IO.File]::OpenRead($Zip); $Hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($Stream)).Replace('-', ''); $Stream.Close();" ^
        "if ($Hash -ne 'E64CE2ED8778E0098F63674CCA61FE33941B5F0C8D9A4AFD651152BDEA3758AB') { throw 'Premake archive hash mismatch' };" ^
        "Expand-Archive $Zip -DestinationPath '%PREMAKE_DIR%' -Force; Remove-Item $Zip" || exit /b 1
)

if not defined VULKAN_SDK (
    echo VULKAN_SDK is not set. Install the Vulkan SDK from https://vulkan.lunarg.com, or set VULKAN_SDK to its folder.
    exit /b 1
)

"%PREMAKE%" --file="%ROOT%premake5.lua" %ACTION% || exit /b 1
