""" This script takes a list of addresses and ranges and generates a list of ranges with the number of accesses in each range.

    The files are currently hardcoded at the bottom of the file for convenience.

    To generate the correct input format from the logs consider the following commands (where rg is ripgrep):

    rg 'psp_misc' qemu_compact.log | awk '{ split($3, a, "="); print a[2] }'
    rg 'psp_x86_misc' qemu_compact.log | awk '{ split($2, a, "="); print a[2] }'
"""

from collections import defaultdict
from pathlib import Path
from sys import argv


def parse_address(addr):
    """Parse a single address or range and return a list of individual addresses."""
    if "-" in addr:
        start, end = addr.split("-")
        return list(range(int(start, 16), int(end, 16) + 1))
    else:
        return [int(addr, 16)]


def generate_ranges(raw_addresses):
    """Generate ranges from a list of addresses."""
    if not raw_addresses:
        return []

    addresses = defaultdict(int)
    for addr in raw_addresses:
        addresses[addr] += 1
    # Sort and remove duplicates
    addresses = sorted(addresses.items(), key=lambda x: x[0])

    ranges = []
    range_start = addresses[0][0]
    prev_address = addresses[0][0]
    range_count = addresses[0][1]

    for addr, count in addresses[1:]:
        if addr not in [prev_address + i for i in range(1, 32)]:
            # End the current range and start a new one
            if range_start == prev_address:
                ranges.append((f"0x{range_start:x}", range_count))
            else:
                ranges.append((f"0x{range_start:x}-0x{prev_address:x}", range_count))
            range_start = addr
            range_count = count
        prev_address = addr
        range_count += count

    # Add the final range
    if range_start == prev_address:
        ranges.append((f"0x{range_start:x}", range_count))
    else:
        ranges.append((f"0x{range_start:x}-0x{prev_address:x}", range_count))

    return ranges


def process_addresses(addresses):
    all_addresses = []
    for addr in addresses:
        all_addresses.extend(parse_address(addr))

    return generate_ranges(all_addresses)


def main():
    file = "relevant_addresses.csv"
    if len(argv) == 2 and argv[1] == "psp":
        file = "psp_addrs.csv"
    with open(Path(__file__).parent / file, "r") as f:
        addresses = f.read().splitlines()
        print(len(addresses))
        ranges = process_addresses(addresses)
        print(*sorted(ranges), sep="\n")


if __name__ == "__main__":
    main()
