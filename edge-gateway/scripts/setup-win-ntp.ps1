# EdgeVib P3: Enable Windows NTP Server
# 用途: 让 PC 成为 Orange Pi chrony 的上游 NTP 时间源
# 运行: 管理员 PowerShell
# 一次运行, 永久生效 (注册表持久化)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File setup-win-ntp.ps1

Write-Host "EdgeVib P3: Enabling Windows NTP Server..."

# 1. 启用 NTP Server 组件
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\NtpServer" -Name "Enabled" -Value 1
Write-Host "  [OK] NtpServer provider enabled"

# 2. 宣布此机器为可靠时间源 (0x05 = 始终提供时间, 0x0A = 查询上层后提供)
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\Config" -Name "AnnounceFlags" -Value 5
Write-Host "  [OK] AnnounceFlags set to 5 (reliable time source)"

# 3. 设置 w32time 自动启动 + 启动服务
Set-Service w32time -StartupType Automatic
Write-Host "  [OK] w32time startup type set to Automatic"
Restart-Service w32time
Write-Host "  [OK] w32time service restarted"

# 4. 开放 Windows 防火墙 UDP 123 端口 (NTP Server)
New-NetFirewallRule -DisplayName "EdgeVib NTP Server (UDP 123)" -Direction Inbound -Protocol UDP -LocalPort 123 -Action Allow -ErrorAction SilentlyContinue
Write-Host "  [OK] Firewall rule added (UDP 123)"

# 5. 强制立即与公网 NTP 同步
w32tm /resync
Write-Host "  [OK] Forced NTP resync triggered"

# 6. 验证
Write-Host ""
Write-Host "=== Verification ==="
w32tm /query /status
Write-Host ""
Write-Host "NTP Server port 123 should now accept incoming requests from Orange Pi (192.168.1.1)."
Write-Host "Test from Orange Pi:  chronyc sources | grep 192.168.1.100"
