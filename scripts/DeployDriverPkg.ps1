param(
    [string]$TargetIP = "",
    [string]$Username = "",
    [string]$PlainPwd = "",
    [switch]$Save
)

# ---------- Read configuration file ----------
$configPath = Join-Path $PSScriptRoot "..\target.cfg"

function Read-TargetConfig([string]$Path) {
    $cfg = @{}
    if (Test-Path $Path) {
        Get-Content $Path | ForEach-Object {
            if ($_ -match '^\s*([^#=]+?)\s*=\s*(.*?)\s*$') {
                $cfg[$Matches[1]] = $Matches[2]
            }
        }
    }
    return $cfg
}

$cfg = Read-TargetConfig $configPath
if (-not $TargetIP) { $TargetIP = $cfg['TargetIP'] }
if (-not $Username)  { $Username  = $cfg['Username']  }
if (-not $PlainPwd)  { $PlainPwd  = $cfg['Password']  }

if (-not $TargetIP -or -not $Username -or -not $PlainPwd) {
    Write-Error "Missing required parameters (TargetIP/Username/PlainPwd), please provide via command line or configure in target.cfg."
    exit 1
}

# ---------- Save configuration (-Save switch) ----------
if ($Save) {
    @"
# Target machine configuration
# Auto-written by DeployDriverPkg.ps1 -Save, can also be edited manually
# Priority: command-line args > this file > error and exit
TargetIP=$TargetIP
Username=$Username
Password=$PlainPwd
"@ | Set-Content $configPath -Encoding UTF8
    Write-Host "Configuration saved to $configPath"
}

# 1. Credentials
$secPwd = ConvertTo-SecureString $PlainPwd -AsPlainText -Force
$cred   = [pscredential]::new("$TargetIP\$Username", $secPwd)

# 2. Local build output directory (script is in scripts\, go up one level then into x64\Debug)
$buildDir = Join-Path $PSScriptRoot "..\x64\Debug\CopyFilter"
$cerFilePath = Join-Path $buildDir "..\CopyFilter.cer"
$files    = @("CopyFilter.sys", "CopyFilter.inf", "CopyFilter.cat", "CopyFilter.cer")

Copy-Item -Path $cerFilePath -Destination $buildDir

# 3. Establish session
try {
    $session = New-PSSession -ComputerName $TargetIP -Credential $cred -ErrorAction Stop
    Write-Host "Successfully connected to $TargetIP"
} catch {
    Write-Error "Connection failed: $_"
    exit 1
}

# 4. Ensure remote directory exists
$remoteDir = "C:\DriverTest\Drivers"
Invoke-Command -Session $session -ScriptBlock {
    param($dir)
    if (-not (Test-Path $dir)) { New-Item -Path $dir -ItemType Directory -Force | Out-Null }
} -ArgumentList $remoteDir

# 5. Copy driver files to remote one by one
foreach ($file in $files) {
    $localPath  = Join-Path $buildDir $file
    $remotePath = Join-Path $remoteDir $file
    if (-not (Test-Path $localPath)) {
        Write-Warning "Local file not found, skipping: $localPath"
        continue
    }
    Write-Host "Copying $file -> $remoteDir"
    Copy-Item -Path $localPath -Destination $remotePath -ToSession $session -Force
}

# 6. Copy DriverManagement.ps1 to remote
$mgmtScript     = Join-Path $PSScriptRoot "DriverManagement.ps1"
$mgmtRemotePath = Join-Path $remoteDir "DriverManagement.ps1"
Write-Host "Copying DriverManagement.ps1 -> $remoteDir"
Copy-Item -Path $mgmtScript -Destination $mgmtRemotePath -ToSession $session -Force

# 7. Register test signing certificate on remote (Root + TrustedPublisher)
Write-Host "Registering certificate on target machine..."
Invoke-Command -Session $session -ScriptBlock {
    param($dir)
    $cerPath = Join-Path $dir "CopyFilter.cer"
    if (-not (Test-Path $cerPath)) {
        Write-Warning "Certificate file not found, skipping registration: $cerPath"
        return
    }
    $stores = @("Root", "TrustedPublisher")
    foreach ($storeName in $stores) {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, "LocalMachine")
        $store.Open("ReadWrite")
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cerPath)
        if (-not $store.Certificates.Find("FindByThumbprint", $cert.Thumbprint, $false).Count) {
            $store.Add($cert)
            Write-Host "  Certificate imported to $storeName (Thumbprint: $($cert.Thumbprint))"
        } else {
            Write-Host "  Certificate already exists in $storeName, skipping"
        }
        $store.Close()
    }

    Set-ExecutionPolicy RemoteSigned -Scope LocalMachine
} -ArgumentList $remoteDir

# 8. Execute installation on remote (set working directory to remoteDir so script can find driver files)
Write-Host "Executing driver installation on remote..."
$installResult = Invoke-Command -Session $session -ScriptBlock {
    param($dir)
    Set-Location $dir
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$dir\DriverManagement.ps1" -Action Install -Force
} -ArgumentList $remoteDir

$installResult | ForEach-Object { Write-Host $_ }

# 9. Close session
Remove-PSSession $session
Write-Host "Deployment complete."
