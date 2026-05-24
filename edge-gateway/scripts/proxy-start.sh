#!/bin/bash
# EdgeVib — PC-side SSH tunnel startup for Orange Pi internet sharing
# Usage: bash edge-gateway/scripts/proxy-start.sh
#
# Architecture:
#   Orange Pi → SSH reverse tunnel → PC system proxy (127.0.0.1:7897) → internet
#   Orange Pi DNS → UDP:53 → Python relay → TCP:5353 → SSH tunnel → 8.8.8.8:53

set -e
ORANGE_IP="${1:-192.168.1.1}"
ORANGE_USER="${2:-root}"
PROXY_PORT="${3:-18888}"
SYSTEM_PROXY_PORT="${4:-7897}"

echo "=== EdgeVib SSH Tunnel ==="

# Kill existing SSH tunnels
taskkill //F //IM ssh.exe 2>/dev/null || true
sleep 1

# Create SSH reverse tunnels:
#   1. Orange Pi:18888  → PC system proxy :7897 (HTTP/HTTPS)
#   2. Orange Pi:5353   → 8.8.8.8:53 (DNS TCP relay)
echo "Creating SSH tunnels to ${ORANGE_IP}..."
echo "  HTTP proxy:  ${ORANGE_IP}:${PROXY_PORT} -> PC:${SYSTEM_PROXY_PORT}"
echo "  DNS relay:   ${ORANGE_IP}:5353 -> 8.8.8.8:53"

ssh -o StrictHostKeyChecking=no \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -R ${PROXY_PORT}:localhost:${SYSTEM_PROXY_PORT} \
    -R 5353:8.8.8.8:53 \
    -f -N ${ORANGE_USER}@${ORANGE_IP}

echo ""
echo "=== SSH tunnel ready ==="
echo "Orange Pi proxy: http://localhost:${PROXY_PORT}"
echo "Test: ssh ${ORANGE_USER}@${ORANGE_IP} 'curl -x http://localhost:${PROXY_PORT} https://github.com'"
echo ""
echo "To start DNS relay on Orange Pi (if not already running):"
echo "  ssh root@${ORANGE_IP} 'systemctl start dns-relay'"
