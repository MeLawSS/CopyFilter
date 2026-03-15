<#
.SYNOPSIS
    Minifilter driver install/update script
.DESCRIPTION
	used for driver installation, update, uninstall and status management on target computer
.NOTES
	need administrator privilege to run
    support windows 10/11 and corresponding server version
.EXAMPLE
    # install(service and registry has been configured in inf file.)
    .\DriverManagement.ps1 -DriverName CopyFilter -InfPath .\CopyFilter.inf -DriverPath .\CopyFilter.sys -Action Install -Force
    # uninstalll
    .\DriverManagement.ps1 -DriverName CopyFilter -Action Uninstall
    # status check
    .\DriverManagement.ps1 -DriverName CopyFilter -Action Status
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [string]$DriverName = "CopyFilter",

    [Parameter(Mandatory=$false)]
    [string]$DriverPath = ".\CopyFilter.sys",

    [Parameter(Mandatory=$false)]
    [string]$InfPath = ".\CopyFilter.inf",

    [Parameter(Mandatory=$false)]
    [ValidateSet("Install", "Update", "Uninstall", "Status", "Start", "Stop", "Debug")]
    [string]$Action = "Install",

    [Parameter(Mandatory=$false)]
    [int]$Altitude = 370020,

    [Parameter(Mandatory=$false)]
    [switch]$Force,

    [Parameter(Mandatory=$false)]
    [switch]$RebootIfNeeded,

    [Parameter(Mandatory=$false)]
    [Alias("h")]
    [switch]$Help
)

if ($Help) {
    Write-Host @"

usage:
  .\DriverManagement.ps1 [params]

params:
  -DriverName   <string>   driver service name               (default: CopyFilter)
  -DriverPath   <string>   .sys file path               (default: .\CopyFilter.sys)
  -InfPath      <string>   .inf file path               (default: .\CopyFilter.inf)
  -Action       <string>   action to be executed（check list below）    (default: Install)
  -Altitude     <int>      Minifilter Altitude           (default: 370020)
  -Force                   force executing（overwrite installation）
  -RebootIfNeeded          reboot after enabling test-sign
  -Help / -h               show help info

action (-Action):
  Install      install driver（need to add -Force when already exists）
  Update       stop old version and install new one
  Uninstall    stop and uninstall driver
  Start        start existed driver
  Stop         stop running driver
  Status       show current status
  Debug        output debug info（fltmc, registry, signature...）

example:
  # first time installation（test-sign driver）
  .\DriverManagement.ps1 -Action Install -Force

  # specify path to install
  .\DriverManagement.ps1 -DriverName CopyFilter -DriverPath C:\pkg\CopyFilter.sys -InfPath C:\pkg\CopyFilter.inf -Action Install -Force

  # update
  .\DriverManagement.ps1 -Action Update

  # uninstall
  .\DriverManagement.ps1 -Action Uninstall

  # status check
  .\DriverManagement.ps1 -Action Status

  # output debug info
  .\DriverManagement.ps1 -Action Debug

"@ -ForegroundColor Cyan
    exit 0
}

$ErrorActionPreference = "Stop"

$colors = @{
    Success = "Green"
    Error   = "Red"
    Warning = "Yellow"
    Info    = "Cyan"
}

function Write-Log {
    param([string]$Message, [string]$Level = "Info")
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $color = $colors[$Level]
    Write-Host "[$timestamp] [$Level] $Message" -ForegroundColor $color
    $logFile = Join-Path $PSScriptRoot "minifilter_install.log"
    "[$timestamp] [$Level] $Message" | Out-File -FilePath $logFile -Append -Encoding UTF8
}

function Test-Admin {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# parse fltmc filters output, return hashtable or $null
function Get-MinifilterStatus {
    param([string]$Name)
    try {
        # fltmc filters columns: Filter Name | Num Instances | Altitude | Frame
        $line = (& fltmc filters 2>&1) | Where-Object {
            $_ -is [string] -and $_.Trim() -match "^$([regex]::Escape($Name))\s"
        } | Select-Object -First 1

        if ($line) {
            $parts = $line.Trim() -split '\s+'
            return @{
                Name         = $parts[0]
                NumInstances = $parts[1]
                Altitude     = $parts[2]
                Frame        = $parts[3]
                Running      = $true
            }
        }
        return $null
    }
    catch {
        return $null
    }
}

function Test-DriverFiles {
    param([string]$SysPath, [string]$InfPath)
    $valid = $true

    if (-not (Test-Path $SysPath)) {
        Write-Log "SYS file not exists: $SysPath" "Error"
        $valid = $false
    }
    if (-not (Test-Path $InfPath)) {
        Write-Log "INF file not exists: $InfPath" "Error"
        $valid = $false
    }

    if ($valid) {
        try {
            $sig = Get-AuthenticodeSignature $SysPath
            if ($sig.Status -eq "NotSigned") {
                if ($Force) {
                    Write-Log "driver is not signed, continue with -Force mode（need test-sign enabled）" "Warning"
                } else {
                    Write-Log "driver is not signed, please add -Force under test mode" "Warning"
                    $valid = $false
                }
            } elseif ($sig.Status -notin @("Valid", "NotSigned")) {
                Write-Log "sign status error: $($sig.Status)" "Warning"
            }
        }
        catch {
            Write-Log "failed to verify signature: $_" "Warning"
        }
    }
    return $valid
}

function Enable-TestSigning {
    Write-Log "Checking test signing mode..." "Info"
    $bcdedit = & bcdedit /enum 2>&1 | Out-String
    if ($bcdedit -match "testsigning\s+Yes") {
        Write-Log "Test signing mode is enabled" "Success"
        return $true
    }

    Write-Log "Enabling test signing mode..." "Warning"
    & bcdedit /set testsigning on 2>&1 | Out-Null

    if ($LASTEXITCODE -eq 0) {
        Write-Log "Test signing mode enabled, reboot required to take effect" "Success"
        if ($RebootIfNeeded) {
            Write-Log "System will reboot in 10 seconds..." "Warning"
            Start-Sleep -Seconds 10
            Restart-Computer -Force
        }
        return $false
    } else {
        throw "Failed to enable test signing mode"
    }
}

function Stop-Minifilter {
    param([string]$Name)
    Write-Log "Stopping Minifilter: $Name" "Info"

    $status = Get-MinifilterStatus -Name $Name
    if (-not $status) {
        Write-Log "Minifilter is not running in filter manager" "Info"
    } else {
        $result = & fltmc unload $Name 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Log "fltmc unload succeeded" "Success"
        } else {
            Write-Log "fltmc unload failed (${result}), continuing with sc stop" "Warning"
        }
    }

    # Stop service (regardless of fltmc result)
    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -ne "Stopped") {
        & sc.exe stop $Name 2>&1 | Out-Null
        Start-Sleep -Seconds 2
        Write-Log "Service stopped" "Success"
    }
    return $true
}

function Start-Minifilter {
    param([string]$Name)
    Write-Log "Starting Minifilter: $Name" "Info"

    # Use sc.exe start, supports kernel/filesystem drivers
    $result = & sc.exe start $Name 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Log "Driver started" "Success"
    } else {
        # sc start returns 1056 for already-running service, which is normal
        if ($result -match "1056") {
            Write-Log "Driver is already running" "Info"
        } else {
            throw "sc start failed: $result"
        }
    }

    # Verify visibility in fltmc
    Start-Sleep -Seconds 1
    $status = Get-MinifilterStatus -Name $Name
    if ($status) {
        Write-Log "Running confirmed: Name=$($status.Name), Instances=$($status.NumInstances), Altitude=$($status.Altitude)" "Success"
    } else {
        Write-Log "Driver started, but not yet visible in fltmc (no mounted volumes)" "Warning"
    }
    return $true
}

function Install-MinifilterDriver {
    param(
        [string]$Name,
        [string]$SysPath,
        [string]$InfPath,
        [int]$Altitude
    )
    Write-Log "Installing Minifilter: $Name" "Info"

    $existing = Get-MinifilterStatus -Name $Name
    $svcExists = (Get-Service -Name $Name -ErrorAction SilentlyContinue) -ne $null

    if (($existing -or $svcExists) -and -not $Force) {
        throw "Minifilter or service already exists, use -Force to reinstall or use Update action"
    }
    if ($existing -or $svcExists) {
        Uninstall-MinifilterDriver -Name $Name
        Start-Sleep -Seconds 2
    }

    # Prefer pnputil + INF (INF handles service creation and registry configuration)
    Write-Log "Installing INF via pnputil: $InfPath" "Info"
    $installResult = & pnputil.exe /add-driver $InfPath /install /force 2>&1
    Write-Log "$installResult" "Info"

    if ($LASTEXITCODE -ne 0) {
        # Fallback: manual service registration and registry configuration
        Write-Log "pnputil failed, falling back to manual registration..." "Warning"

        $destSys = "$env:SystemRoot\System32\drivers\$Name.sys"
        Write-Log "Copying driver file to: $destSys" "Info"
        Copy-Item -Path $SysPath -Destination $destSys -Force

        # sc.exe create supports filesys type (New-Service does not)
        $scResult = & sc.exe create $Name `
            type= filesys `
            start= demand `
            error= normal `
            binPath= "\SystemRoot\System32\drivers\$Name.sys" `
            DisplayName= "$Name Minifilter Driver" `
            depend= FltMgr 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "sc.exe create failed: $scResult"
        }

        # Set Minifilter instance registry key (altitude)
        $regBase = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
        $regInstances = "$regBase\Instances"
        New-Item -Path $regInstances -Force | Out-Null
        Set-ItemProperty -Path $regInstances -Name "DefaultInstance" -Value "$Name Instance" -Force
        $regInst = "$regInstances\$Name Instance"
        New-Item -Path $regInst -Force | Out-Null
        Set-ItemProperty -Path $regInst -Name "Altitude" -Value ([string]$Altitude) -Force
        Set-ItemProperty -Path $regInst -Name "Flags"    -Value 0 -Type DWord -Force
        Write-Log "Manual registration complete, altitude: $Altitude" "Success"
    }

    Start-Minifilter -Name $Name
    return $true
}

function Uninstall-MinifilterDriver {
    param([string]$Name, [switch]$KeepFiles)
    Write-Log "Uninstalling Minifilter: $Name" "Info"

    Stop-Minifilter -Name $Name

    # Delete service
    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($svc) {
        Write-Log "Deleting service..." "Info"
        & sc.exe delete $Name 2>&1 | Out-Null
        Start-Sleep -Seconds 2
    }

    # Clean up registry (pnputil uninstall usually handles this, kept as fallback)
    $regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
    if (Test-Path $regPath) {
        Write-Log "Cleaning up registry..." "Info"
        Remove-Item -Path $regPath -Recurse -Force -ErrorAction SilentlyContinue
    }

    # Uninstall OEM INF (parse pnputil /enum-drivers output line by line)
    Write-Log "Searching for installed OEM INF..." "Info"
    $oemOutput = & pnputil.exe /enum-drivers 2>&1
    $oemName   = $null
    $matched   = $false
    foreach ($line in $oemOutput) {
        if ($line -match "Published Name\s*:\s*(oem\d+\.inf)") {
            $oemName = $Matches[1]
            $matched = $false
        }
        if ($line -match "Original Name\s*:\s*$([regex]::Escape($Name))\.inf") {
            $matched = $true
        }
        if ($matched -and $oemName) {
            Write-Log "Uninstalling OEM INF: $oemName" "Info"
            & pnputil.exe /delete-driver $oemName /uninstall /force 2>&1 | Out-Null
            $matched = $false
            $oemName = $null
        }
    }

    if (-not $KeepFiles) {
        $driverFile = "$env:SystemRoot\System32\drivers\$Name.sys"
        if (Test-Path $driverFile) {
            Write-Log "Deleting driver file: $driverFile" "Info"
            Remove-Item -Path $driverFile -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Log "Uninstall complete" "Success"
    return $true
}

function Update-MinifilterDriver {
    param(
        [string]$Name,
        [string]$SysPath,
        [string]$InfPath,
        [int]$Altitude
    )
    Write-Log "Updating Minifilter: $Name" "Info"

    # Uninstall old version (keep files to avoid lock issues)
    Uninstall-MinifilterDriver -Name $Name -KeepFiles
    Start-Sleep -Seconds 2

    Install-MinifilterDriver -Name $Name -SysPath $SysPath -InfPath $InfPath -Altitude $Altitude
    Write-Log "Update complete" "Success"
    return $true
}

function Show-DebugInfo {
    param([string]$Name)
    Write-Log "========== Minifilter Debug Info ==========" "Info"

    Write-Log "--- Test Signing Status ---" "Info"
    & bcdedit /enum 2>&1 | Select-String "testsigning" | ForEach-Object { Write-Log $_ "Info" }

    Write-Log "--- All Minifilters ---" "Info"
    & fltmc filters 2>&1 | ForEach-Object { Write-Log $_ "Info" }

    Write-Log "--- Minifilter Instances ---" "Info"
    & fltmc instances 2>&1 | ForEach-Object { Write-Log $_ "Info" }

    if ($Name) {
        Write-Log "--- $Name Service Info ---" "Info"
        $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if ($svc) {
            Write-Log "Service status: $($svc.Status), Start type: $($svc.StartType)" "Info"
        } else {
            Write-Log "Service does not exist" "Warning"
        }

        Write-Log "--- $Name Registry Configuration ---" "Info"
        $regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
        if (Test-Path $regPath) {
            Get-ChildItem -Path $regPath -Recurse -ErrorAction SilentlyContinue |
                ForEach-Object {
                    $keyPath = $_.PSPath -replace "Microsoft.PowerShell.Core\\Registry::", ""
                    Write-Log "[$keyPath]" "Info"
                    Get-ItemProperty -Path $_.PSPath -ErrorAction SilentlyContinue |
                        Get-Member -MemberType NoteProperty |
                        Where-Object { $_.Name -notlike "PS*" } |
                        ForEach-Object {
                            $val = (Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue).($_.Name)
                            Write-Log "  $($_.Name) = $val" "Info"
                        }
                }
        } else {
            Write-Log "Registry key does not exist: $regPath" "Warning"
        }

        Write-Log "--- $Name Driver File ---" "Info"
        $driverFile = "$env:SystemRoot\System32\drivers\$Name.sys"
        if (Test-Path $driverFile) {
            $fi = Get-Item $driverFile
            Write-Log "Path: $($fi.FullName)" "Info"
            Write-Log "Size: $($fi.Length) bytes" "Info"
            Write-Log "Modified: $($fi.LastWriteTime)" "Info"
            $sig = Get-AuthenticodeSignature $driverFile
            Write-Log "Signature status: $($sig.Status)" "Info"
        } else {
            Write-Log "Driver file not found: $driverFile" "Warning"
        }
    }

    Write-Log "--- Installed FSFilter Drivers (OEM INF) ---" "Info"
    & pnputil.exe /enum-drivers 2>&1 |
        Select-String -Pattern "Published Name|Original Name|Class Name" |
        ForEach-Object { Write-Log $_ "Info" }

    Write-Log "========================================" "Info"
}

# ============ Main ============

Write-Log "Minifilter management script started" "Info"
Write-Log "Action: $Action, Driver: $DriverName" "Info"

if (-not (Test-Admin)) {
    throw "This script requires administrator privileges"
}

# Resolve paths (convert to absolute path strings)
$resolvedDriver = Resolve-Path $DriverPath -ErrorAction SilentlyContinue
$resolvedInf    = Resolve-Path $InfPath    -ErrorAction SilentlyContinue
$DriverPath = if ($resolvedDriver) { $resolvedDriver.Path } else { Join-Path $PSScriptRoot "$DriverName.sys" }
$InfPath    = if ($resolvedInf)    { $resolvedInf.Path    } else { Join-Path $PSScriptRoot "$DriverName.inf" }

try {
    switch ($Action) {
        "Install" {
            $sig = Get-AuthenticodeSignature $DriverPath -ErrorAction SilentlyContinue
            if ($sig -and $sig.Status -eq "NotSigned") {
                $ready = Enable-TestSigning
                if (-not $ready) { exit 0 }   # reboot required before installing
            }
            if (-not (Test-DriverFiles -SysPath $DriverPath -InfPath $InfPath)) {
                if (-not $Force) { exit 1 }
            }
            Install-MinifilterDriver -Name $DriverName -SysPath $DriverPath -InfPath $InfPath -Altitude $Altitude
        }

        "Update" {
            if (-not (Test-DriverFiles -SysPath $DriverPath -InfPath $InfPath)) {
                throw "Driver file validation failed"
            }
            Update-MinifilterDriver -Name $DriverName -SysPath $DriverPath -InfPath $InfPath -Altitude $Altitude
        }

        "Uninstall" {
            Uninstall-MinifilterDriver -Name $DriverName
        }

        "Start" {
            Start-Minifilter -Name $DriverName
        }

        "Stop" {
            Stop-Minifilter -Name $DriverName
        }

        "Status" {
            $status = Get-MinifilterStatus -Name $DriverName
            if ($status) {
                Write-Log "Minifilter status: Running" "Success"
                Write-Log "  Name         : $($status.Name)"         "Info"
                Write-Log "  Instances    : $($status.NumInstances)" "Info"
                Write-Log "  Altitude     : $($status.Altitude)"     "Info"
                Write-Log "  Frame        : $($status.Frame)"        "Info"
            } else {
                $svc = Get-Service -Name $DriverName -ErrorAction SilentlyContinue
                if ($svc) {
                    Write-Log "Service exists but not running in filter manager, service status: $($svc.Status)" "Warning"
                } else {
                    Write-Log "Minifilter not installed or not running" "Warning"
                }
            }
        }

        "Debug" {
            Show-DebugInfo -Name $DriverName
        }
    }
}
catch {
    Write-Log "Operation failed: $_" "Error"
    exit 1
}
