#!/usr/bin/env python3
"""Extracts a fresh SoundCloud client_id from the public web player.

The key rotates over time and is not secret in the web app, but you need it for
api-v2. Usage:
    python3 tools/get_client_id.py            # print candidates + test
    python3 tools/get_client_id.py --write /path/to/config.cfg
"""
import re
import subprocess
import sys

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36"
CURL = ["curl", "-sSL", "--max-time", "30", "-A", UA]


def curl(url):
    return subprocess.run(CURL + [url], capture_output=True, text=True).stdout


def candidates():
    html = curl("https://soundcloud.com/")
    assets = sorted(set(re.findall(r'https://a-v2\.sndcdn\.com/assets/[^"\']+\.js', html)))
    found = []
    for url in assets:
        js = curl(url)
        for pat in (r'client_id:"([A-Za-z0-9]{20,40})"',
                    r'clientId:"([A-Za-z0-9]{20,40})"',
                    r'"client_id":"([A-Za-z0-9]{20,40})"'):
            found += re.findall(pat, js)
    out = []
    for c in found:
        if c not in out:
            out.append(c)
    return out


def works(cid):
    url = ("https://api-v2.soundcloud.com/search/tracks?q=test&limit=1&client_id=" + cid)
    r = subprocess.run(["curl", "-sS", "--max-time", "20", "-o", "/dev/null",
                        "-w", "%{http_code}", url], capture_output=True, text=True)
    return r.stdout.strip() == "200"


def main():
    args = sys.argv[1:]
    write_path = None
    if "--write" in args:
        write_path = args[args.index("--write") + 1]
    found = candidates()
    if not found:
        print("no client_id found (page layout changed?)", file=sys.stderr)
        return 1
    good = [c for c in found if works(c)]
    for c in found:
        print(("OK   " if c in good else "bad  ") + c)
    if not good:
        print("none of the candidates work", file=sys.stderr)
        return 1
    cid = good[0]
    print("\nuse:", cid)
    if write_path:
        with open(write_path) as f:
            data = f.read()
        if re.search(r"^client_id=.*$", data, re.M):
            data = re.sub(r"^client_id=.*$", "client_id=" + cid, data, flags=re.M)
        else:
            data += "client_id=" + cid + "\n"
        with open(write_path, "w") as f:
            f.write(data)
        print("written to", write_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
