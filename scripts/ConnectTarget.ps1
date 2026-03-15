# Only needed on first run; afterwards you can double-click to connect directly

param(
    [string]$TargetIP   = "192.168.132.129",
    [string]$Username   = "melo",
    [string]$PlainPwd   = "010929"          # for production use, replace with credential read from a file for better security
)

# 1. Confirm the WinRM service is installed and running
if (-not (Get-Service winrm -ErrorAction SilentlyContinue)) {
    Write-Host "WinRM service is not installed. Please run: winrm quickconfig"
    exit 1
}
if ((Get-Service winrm).Status -ne 'Running') {
    Start-Service winrm
}

# 2. Add the target IP to TrustedHosts (only needed on first run)
$trusted = (Get-Item WSMan:\localhost\Client\TrustedHosts).Value
if ($trusted -notlike "*$TargetIP*") {
    Write-Host "First run: adding $TargetIP to TrustedHosts ..."
    Set-Item WSMan:\localhost\Client\TrustedHosts $TargetIP -Force
}

# 3. Build credential object
$fullUser = "$TargetIP\$Username"
$secPwd   = ConvertTo-SecureString $PlainPwd -AsPlainText -Force
$cred     = New-Object System.Management.Automation.PSCredential($fullUser, $secPwd)

# 4. Open remote session
Write-Host "Connecting to $TargetIP ..."
Enter-PSSession -ComputerName $TargetIP -Credential $cred
