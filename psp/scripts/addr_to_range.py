from pathlib import Path
from sys import argv
import json
with (Path.home() / "uni" / "thesis" / "content" / "3-analysis" /"tables" / 'memory_layout.json').open() as f:
    memory_layout = json.load(f)
def addr_to_range(addr):
    for category in memory_layout:
        for entry in category['entries']:
            if entry['begin'] <= addr < entry['end']:
                return (category["type"],entry['description'])
    return None, None

def main():
    print(addr_to_range(int(argv[1],0)))

if __name__ == "__main__":
    main()
