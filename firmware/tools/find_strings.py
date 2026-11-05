import re
import sys

PATH = r".\waveshare-repo\firmware\ESP32-S3-Touch-AMOLED-1.75C_FactoryOnly_20260114.bin"
START = 0x110000
END = 0xA10000

with open(PATH, "rb") as f:
    data = f.read()[START:END]

pat = re.compile(rb"[\x20-\x7e]{3,64}")
hits = []
for m in pat.finditer(data):
    s = m.group().decode("ascii")
    hits.append((START + m.start(), s))

print(f"strings: {len(hits)}")

keywords = ("ssid", "pswd", "pass", "wlan", "wifi", "nvs", "cred", "station")

print("\n=== strings mentioning wifi/ssid/password (len<=40) ===")
seen = set()
for off, s in hits:
    low = s.lower()
    if len(s) <= 40 and any(k in low for k in keywords) and s not in seen:
        seen.add(s)
        print(f"  0x{off:07X}  [{s}]")

print("\n=== short lowercase identifiers (possible NVS keys/namespaces) ===")
keys = set()
for off, s in hits:
    if re.fullmatch(r"[a-z][a-z0-9_.\-]{2,17}", s):
        keys.add(s)
print("  " + "  ".join(sorted(keys)))
