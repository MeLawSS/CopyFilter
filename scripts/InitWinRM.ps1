# Run as administrator
winrm quickconfig -force

# Add target IP to trusted hosts list
Set-Item WSMan:\localhost\Client\TrustedHosts -Value "192.168.61.129" -Force

# Or add multiple hosts (comma-separated)
Set-Item WSMan:\localhost\Client\TrustedHosts -Value "192.168.61.129,192.168.61.130" -Force

# View current setting
Get-Item WSMan:\localhost\Client\TrustedHosts