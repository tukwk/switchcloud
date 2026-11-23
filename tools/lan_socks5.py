#!/usr/bin/env python3
"""Tiny LAN SOCKS5 proxy (CONNECT only, no auth).

Runs on the PC so the Switch can use the PC's network route (e.g. an active
VPN) as a bypass. Only expose it on your local network.

Usage:
    python3 tools/lan_socks5.py [bind] [port]
"""
import select
import socket
import struct
import sys
import threading

BIND = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 10808


def recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("eof")
        buf += chunk
    return buf


def read_addr(sock, atyp):
    if atyp == 1:
        return socket.inet_ntoa(recvn(sock, 4))
    if atyp == 3:
        length = recvn(sock, 1)[0]
        return recvn(sock, length).decode("utf-8", "replace")
    if atyp == 4:
        return socket.inet_ntop(socket.AF_INET6, recvn(sock, 16))
    raise ValueError("bad atyp")


def relay(a, b):
    socks = [a, b]
    while True:
        r, _, _ = select.select(socks, [], [], 120)
        if not r:
            return
        for s in r:
            data = s.recv(65536)
            if not data:
                return
            other = b if s is a else a
            other.sendall(data)


def handle(client, peer):
    try:
        ver, nmethods = recvn(client, 2)
        if ver != 5:
            return
        methods = recvn(client, nmethods)
        if 0 not in methods:
            client.sendall(b"\x05\xff")
            return
        client.sendall(b"\x05\x00")

        ver, cmd, rsv, atyp = recvn(client, 4)
        if cmd != 1:
            client.sendall(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")
            return
        host = read_addr(client, atyp)
        port = struct.unpack("!H", recvn(client, 2))[0]

        try:
            remote = socket.create_connection((host, port), timeout=20)
        except Exception as exc:
            print("fail %s:%d <- %s: %s" % (host, port, peer, exc), flush=True)
            client.sendall(b"\x05\x04\x00\x01\x00\x00\x00\x00\x00\x00")
            return

        local = remote.getsockname()
        reply = b"\x05\x00\x00\x01" + socket.inet_aton(local[0] if local[0].count(".") == 3 else "0.0.0.0")
        client.sendall(reply + struct.pack("!H", local[1]))
        print("ok   %s:%d <- %s" % (host, port, peer), flush=True)

        remote.settimeout(None)
        client.settimeout(None)
        relay(client, remote)
        remote.close()
    except Exception as exc:
        print("err  %s: %s" % (peer, exc), flush=True)
    finally:
        try:
            client.close()
        except OSError:
            pass


def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((BIND, PORT))
    server.listen(64)
    print("SOCKS5 listening on %s:%d" % (BIND, PORT), flush=True)
    while True:
        client, peer = server.accept()
        threading.Thread(target=handle, args=(client, peer[0]), daemon=True).start()


if __name__ == "__main__":
    main()
