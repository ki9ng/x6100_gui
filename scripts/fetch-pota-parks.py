#!/usr/bin/env python3
"""
Fetch all POTA parks worldwide, save as a packed binary suitable for shipping
on the X6100 SD card and/or in-radio updating.

Usage:
    ./fetch-pota-parks.py --output pota-parks.bin
    ./fetch-pota-parks.py --output pota-parks.bin --json raw.json --csv parks.csv

Binary format (little-endian):
    header  16 bytes: magic "PARK" | version u32 | count u32 | epoch u32
    entry   36 bytes: ref 11 chars | lat i32 (microdegrees) | lon i32 (microdegrees) | name 17 chars

Reference length is capped at 10 chars + null. Name is truncated to 16 chars + null.
Full name can be fetched from api.pota.app/park/<ref> when needed.
"""

import argparse
import gzip
import json
import os
import struct
import sys
import time
import urllib.request

API = "https://api.pota.app"
HEADER_FMT = "<4sIII"           # magic, version, count, epoch
ENTRY_FMT  = "<11sii17s"        # ref, lat_udeg, lon_udeg, name
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)   # 36
VERSION    = 1

def fetch_json(url, timeout=20):
    req = urllib.request.Request(url, headers={"Accept-Encoding": "gzip"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        if resp.headers.get("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
        return json.loads(raw)

def fetch_all_parks(verbose=True):
    # /locations?program=US returns all 3770 subdivisions worldwide
    # (the query param appears to be ignored server-side)
    subs = fetch_json(f"{API}/locations?program=US")
    all_locs = [s["locationDesc"] for s in subs if s.get("locationDesc")]
    if verbose:
        print(f"Fetching parks for {len(all_locs)} subdivisions…", file=sys.stderr)

    parks = []
    failures = []
    t0 = time.time()

    for i, loc in enumerate(all_locs):
        if verbose and i % 100 == 0:
            rate = i / max(time.time() - t0, 0.1)
            print(f"  {i}/{len(all_locs)}  {int(time.time()-t0)}s  {rate:.1f}/s  "
                  f"{len(parks):,} parks so far",
                  file=sys.stderr)
        try:
            parks.extend(fetch_json(f"{API}/locations/{loc}"))
        except Exception as e:
            failures.append((loc, str(e)))
            continue

    if verbose:
        print(f"Done in {int(time.time()-t0)}s — {len(parks):,} parks, "
              f"{len(failures)} failed subdivisions",
              file=sys.stderr)

    return parks, failures

def pack(parks, out_path):
    """Write a packed binary file sorted by reference for deterministic output."""
    parks = sorted(
        (p for p in parks if p.get("reference")),
        key=lambda p: p["reference"]
    )

    with open(out_path, "wb") as f:
        f.write(struct.pack(HEADER_FMT, b"PARK", VERSION, len(parks), int(time.time())))
        for p in parks:
            ref  = (p.get("reference") or "")[:10].encode("ascii", "replace").ljust(11, b"\0")[:11]
            lat  = int(round((p.get("latitude")  or 0.0) * 1_000_000))
            lon  = int(round((p.get("longitude") or 0.0) * 1_000_000))
            name = (p.get("name") or "")[:16].encode("utf-8", "replace").ljust(17, b"\0")[:17]
            f.write(struct.pack(ENTRY_FMT, ref, lat, lon, name))

def write_csv(parks, out_path):
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("ref,lat,lon,locationDesc,name\n")
        for p in parks:
            name = (p.get("name") or "").replace('"', '""')
            f.write(f'{p.get("reference","")},'
                    f'{p.get("latitude", "")},'
                    f'{p.get("longitude", "")},'
                    f'{p.get("locationDesc", "")},'
                    f'"{name}"\n')

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output", "-o", required=True, help="packed binary output path")
    ap.add_argument("--json",  help="also write raw JSON to this path")
    ap.add_argument("--csv",   help="also write minimal CSV to this path")
    ap.add_argument("--quiet", "-q", action="store_true", help="suppress progress")
    args = ap.parse_args()

    parks, failures = fetch_all_parks(verbose=not args.quiet)

    pack(parks, args.output)
    size = os.path.getsize(args.output)
    print(f"Wrote {args.output}: {size:,} bytes ({size/1024/1024:.2f} MB, {len(parks):,} parks)",
          file=sys.stderr)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(parks, f, separators=(",", ":"))
        print(f"Wrote {args.json}: {os.path.getsize(args.json):,} bytes", file=sys.stderr)

    if args.csv:
        write_csv(parks, args.csv)
        print(f"Wrote {args.csv}: {os.path.getsize(args.csv):,} bytes", file=sys.stderr)

    if failures:
        print(f"\nFailed subdivisions ({len(failures)}):", file=sys.stderr)
        for loc, err in failures[:20]:
            print(f"  {loc}: {err}", file=sys.stderr)
        if len(failures) > 20:
            print(f"  … and {len(failures)-20} more", file=sys.stderr)

if __name__ == "__main__":
    main()
