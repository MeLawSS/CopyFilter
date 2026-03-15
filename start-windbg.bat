@echo off
setlocal enabledelayedexpansion

set TARGET_IP=%~1
set USERNAME=%~2
set PASSWORD=%~3
set KD_KEY=%~4
set KD_PORT=%~5
set DEBUG_PRODUCT_DIR=%~dp0x64\Debug

:: ---------- fill blank parameters from target.cfg ----------
set CONFIG=%~dp0target.cfg
if exist "%CONFIG%" (
    for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%CONFIG%") do (
        set _KEY=%%a
        set _VAL=%%b
        if /i "!_KEY: =!"=="TargetIP" if "!TARGET_IP!"=="" set TARGET_IP=!_VAL!
        if /i "!_KEY: =!"=="Username" if "!USERNAME!"==""  set USERNAME=!_VAL!
        if /i "!_KEY: =!"=="Password" if "!PASSWORD!"==""  set PASSWORD=!_VAL!
    )
)

:: WinDbg parameters still use hardcoded fallbacks (not target machine params; not stored in target.cfg)
if "%KD_KEY%"==""  set KD_KEY=28y0u9eg45gq3.3iwebbrirygq6.2a3i6j2fc276h.28phgo9tmo0kw
if "%KD_PORT%"=="" set KD_PORT=50000

echo Starting WinDbg kernel debug session (port=%KD_PORT%)...
start "" windbg -k net:port=%KD_PORT%,key=%KD_KEY% -y "SRV*C:\Symbols*http://msdl.microsoft.com/download/symbols;%DEBUG_PRODUCT_DIR%" -i "%DEBUG_PRODUCT_DIR%" -srcpath "%~dp0"

echo Rebooting target machine %TARGET_IP%...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "$pw = ConvertTo-SecureString '%PASSWORD%' -AsPlainText -Force;" ^
    "$cred = [pscredential]::new('%TARGET_IP%\%USERNAME%', $pw);" ^
    "Invoke-Command -ComputerName '%TARGET_IP%' -Credential $cred -ScriptBlock { Restart-Computer -Force }"

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Remote reboot failed. Check WinRM connection or credentials.
    exit /b 1
)

echo Reboot command sent to target machine. Waiting for kernel debug connection...
echo Press any key to exit...
pause >nul
