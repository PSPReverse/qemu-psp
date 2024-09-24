## This script can be used to parse the pdftotext output from a PPR NRB document
# It will attempt to parse everything that looks like a field header
# However, the header format is not strictly defined, so it may not catch everything
# Also since it can't differentiate which Address Space (SMN, PCICFG, USB)
# something is mapped on, there might be overlap and false positives

# AS SUCH IT SHOULD NOT BE USED WITHOUT FURTHER IMPROVEMENTS

# Sample Inputs:
# BXXD00F0x188 (NBIFSWDSCFG::PCIE_TLP_PREFIX_LOG0) - Handled
# BXXD00F[1...4]x004 (NBIFEPFNCFG::COMMAND) - Not handled
# D18F3x004 [Status Command Function 3] (DF::StatusCommand3) - Handled

import re
import json

# Regex to match the log format
field_header_pattern = re.compile(
    r"(?:B(\d+)|)D(\d+)F(\d+)x([\da-fA-F]+)\s(?:\[(.+)\]\s)?\((.+)\)"
)


def extract_field_header(line):
    match = field_header_pattern.match(line)
    if match:
        bus, device, function, offset, description, name = match.groups()
        if bus is None:
            bus = 0
        new_entry = {
            "Bus": "Any" if bus == "XX" else int(bus),
            "Device": int(device, 16),
            "Function": int(function, 16),
            "Offset": int(offset, 16),
            "Name": name,
        }
        if description:
            new_entry["Description"] = description
        return new_entry
    return None


def parse_raw_pdf_lines(log_lines):
    parsed_lines = []
    for line in log_lines:
        parsed_line = extract_field_header(line.strip())
        if parsed_line:
            parsed_lines.append(parsed_line)
    return parsed_lines


with open("out.txt", "r") as f:
    found_headers = parse_raw_pdf_lines(f.readlines())

# Dumping parsed logs as JSON
json_output = json.dumps(found_headers, indent=4)
with open("pci_config.json", "w") as f:
    f.write(json_output)
print(json_output)
print(len(found_headers))
