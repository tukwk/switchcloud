#!/bin/sh
# Starts the LAN SOCKS5 proxy on this PC and (if MTP is mounted) updates the
# proxy= line in the Switch config automatically.
set -e
cd "$(dirname "$0")/.."
PORT="${1:-10808}"
mkdir -p build
pkill -f 'lan_socks5.py' 2>/dev/null || true
sleep 0.2
setsid nohup python3 tools/lan_socks5.py 0.0.0.0 "$PORT" > build/socks5.log 2>&1 < /dev/null &
sleep 1

IP=$(ip -4 addr show scope global 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 \
     | grep -E '^(192\.168\.|10\.|172\.)' | grep -v '^10\.123\.' | head -1)
[ -z "$IP" ] && IP="<IP-ПК>"

M=$(ls -d /run/user/"$(id -u)"/gvfs/mtp:* 2>/dev/null | head -1)
if [ -n "$M" ] && [ -f "$M/1: SD Card/switch/scswitch/config.cfg" ]; then
    python3 - "$M/1: SD Card/switch/scswitch/config.cfg" "socks5://$IP:$PORT" <<'PY'
import re, sys
path, proxy = sys.argv[1], sys.argv[2]
data = open(path).read()
if re.search(r'^proxy=.*$', data, re.M):
    data = re.sub(r'^proxy=.*$', 'proxy=' + proxy, data, flags=re.M)
else:
    data += 'proxy=' + proxy + '\n'
open(path, 'w').write(data)
print('config updated:', proxy)
PY
fi

echo "SOCKS5 запущен. В настройках switchcloud:"
echo "  socks5://${IP}:${PORT}"
echo "лог: build/socks5.log"
