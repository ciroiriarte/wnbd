$ErrorActionPreference = "Continue"
Get-Process qemu-storage-daemon -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
