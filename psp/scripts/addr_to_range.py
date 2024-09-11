from pathlib import Path
from sys import argv
import json

with (
    Path.home() / "uni" / "thesis" / "assets" / "tables" / "memory_layout.json"
).open() as f:
    memory_layout = json.load(f)


def addr_to_range(addr):
    for category in memory_layout:
        for entry in category["entries"]:
            if int(entry["begin"], 0) <= addr < int(entry["end"], 0):
                return (category["type"], entry["description"])
    return None, None


def main():
    print(addr_to_range(int(argv[1], 0)))


if __name__ == "__main__":
    main()
