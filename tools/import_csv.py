#!/usr/bin/env python3
"""Test-only CSV importer for SDL30 Collector.

Usage:
    python3 tools/import_csv.py <csv_file> <job_name> [device_ip]

Example:
    python3 tools/import_csv.py ~/Downloads/18001_section1.csv 18001 192.168.4.1
"""

import sys
import urllib.request

DEVICE_IP = "192.168.4.1"

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    csv_path  = sys.argv[1]
    job_name  = sys.argv[2]
    device_ip = sys.argv[3] if len(sys.argv) > 3 else DEVICE_IP

    with open(csv_path, "rb") as f:
        body = f.read()

    url = f"http://{device_ip}/api/import?job={job_name}"
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "text/csv"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        print(resp.read().decode())

if __name__ == "__main__":
    main()
