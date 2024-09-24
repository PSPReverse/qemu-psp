from collections import OrderedDict
import json
import sys
from pathlib import Path

addr_types = ["addr", "address", "offset", "reg"]


class PCISpaceEnhancer:
    def __init__(self) -> None:
        with (Path(__file__).parent / "pci_config.json").open("r") as f:
            self.config = json.load(f)

    def enhance(self, kv_pairs, addr):
        for item in self.config:
            if self.is_equal(item, kv_pairs, addr):
                kv_pairs["name"] = item["Name"]

    def is_equal(self, item, kv_pairs, addr):
        return (
            (item["Bus"] == "Any" or item["Bus"] == int(kv_pairs["bus"], 0))
            and item["Device"] == int(kv_pairs["device"], 0)
            and item["Function"] == int(kv_pairs["function"], 0)
            and item["Offset"] == int(addr["offset"], 0)
        )


class LogProcessor:
    def __init__(self) -> None:
        self.pci_enhancer = PCISpaceEnhancer()

    def process_log(self, input):
        previous_command = None
        previous_kv_pairs = None
        start_addr = None
        end_addr = None
        values = []
        for line in input:
            # Split the line into parts
            parts = line.strip().split()

            if len(parts) < 3:
                print(line.strip(), flush=True)
                continue

            # The first part is the command
            command = parts[0]

            # Extract key-value pairs into a dictionary, except addr and value
            kv_pairs = OrderedDict()
            addr = {}
            value = None
            unparseable = False
            for part in parts[1:]:
                if part == "(default)":
                    # Ignore (default)
                    continue
                if "=" not in part:
                    unparseable = True
                    break
                key, val = part.split("=", 1)
                if key in addr_types:
                    addr[key] = val
                elif key == "value":
                    value = val
                else:
                    kv_pairs[key] = val

            if unparseable or len(addr) == 0 or value is None:
                print(line.strip(), flush=True)
                continue

            # Check if this line continues the sequence
            if (
                command == previous_command
                and kv_pairs == previous_kv_pairs
                and is_adjacent(addr, end_addr)
            ):
                # Continue the sequence
                end_addr = addr
                values.append(value)
            else:
                # Print the previous sequence if it exists
                if previous_command is not None:
                    self.compact_write(
                        previous_command,
                        previous_kv_pairs,
                        start_addr,
                        end_addr,
                        values,
                    )

                # Start a new sequence
                previous_command = command
                previous_kv_pairs = kv_pairs
                start_addr = addr
                end_addr = addr
                values = [value]

        # Print the last sequence if it exists
        if previous_command is not None:
            self.compact_write(
                previous_command, previous_kv_pairs, start_addr, end_addr, values
            )

    def compact_write(
        self,
        command: str,
        kv_pairs: dict[str, str],
        start_addr: dict[str, str],
        end_addr: dict[str, str],
        values: list[str],
    ) -> None:
        self.command_specific_post_process(
            command, kv_pairs, start_addr, end_addr, values
        )
        addr_str = " ".join(
            (
                f"{key}={start_addr[key]}"
                if start_addr[key] == end_addr[key]
                else f"{key}={start_addr[key]}-{end_addr[key]}"
            )
            for key in start_addr
        )
        if addr_str != "":
            addr_str += " "
        value_str = rle_encode(values)
        # Reconstruct the key-value pair string
        kv_pairs_str = " ".join(f"{k}={v}" for k, v in kv_pairs.items())
        if kv_pairs_str != "":
            kv_pairs_str += " "
        print(
            f"{command} {kv_pairs_str}{addr_str}value={value_str}",
            flush=True,
        )

    def command_specific_post_process(
        self,
        command: str,
        kv_pairs: dict[str, str],
        start_addr: dict[str, str],
        end_addr: dict[str, str],
        values: list[str],
    ) -> None:
        if "region" in kv_pairs and kv_pairs["region"] == "unknown":
            del kv_pairs["region"]
            del start_addr["offset"]
            end_addr.pop("offset", None)
        if "psp_x86_config_space" in command:
            if end_addr is None:
                # TODO: Handle multiple consecutive reads
                return
            self.pci_enhancer.enhance(kv_pairs, start_addr)


def is_adjacent(addr1: dict[str, str], addr2: dict[str, str]) -> bool:
    # commands are same so keys are same
    # all scaling is linear, so if any key is not adjacent, all keys are not adjacent
    for key in addr1.keys():
        if int(addr1[key], 16) in [int(addr2[key], 16) + i for i in range(1, 5)]:
            return True
    return False


def rle_encode(values):
    if not values:
        return "[]"
    if len(values) == 1:
        return values[0]

    encoded_values = []
    current_value = values[0]
    count = 1

    for value in values[1:]:
        if value == current_value:
            count += 1
        else:
            if count > 1:
                encoded_values.append(f"{current_value}*{count}")
            else:
                encoded_values.append(current_value)
            current_value = value
            count = 1

    # Add the last value
    if count > 1:
        encoded_values.append(f"{current_value}*{count}")
    else:
        encoded_values.append(current_value)

    return f"[{', '.join(encoded_values)}]"


if __name__ == "__main__":
    lp = LogProcessor()
    if len(sys.argv) > 2:
        print(f"Usage: {sys.argv[0]} <log_file>", flush=True)
    elif len(sys.argv) == 1:
        lp.process_log(sys.stdin)
    else:
        with open(sys.argv[1], "r") as log_file:
            lp.process_log(log_file)
