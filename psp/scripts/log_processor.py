import sys


def process_log(input):
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
        kv_pairs = {}
        addr = None
        value = None
        addr_is_offset = False
        for part in parts[1:]:
            if "=" not in part:
                # gets printed in addr is None check
                break
            key, val = part.split("=")
            if key == "addr":
                addr = val
            elif key == "offset":
                addr_is_offset = True
                addr = val
            elif key == "value":
                value = val
            else:
                kv_pairs[key] = val

        if addr is None or value is None:
            print(line.strip(), flush=True)
            continue

        # Check if this line continues the sequence
        if (
            command == previous_command
            and kv_pairs == previous_kv_pairs
            and int(addr, 16) in [int(end_addr, 16) + i for i in range(1, 5)]
        ):
            # Continue the sequence
            end_addr = addr
            values.append(value)
        else:
            # Print the previous sequence if it exists
            if previous_command is not None:
                compact_write(
                    previous_command,
                    previous_kv_pairs,
                    start_addr,
                    end_addr,
                    values,
                    addr_is_offset
                )

            # Start a new sequence
            previous_command = command
            previous_kv_pairs = kv_pairs
            start_addr = addr
            end_addr = addr
            values = [value]

    # Print the last sequence if it exists
    if previous_command is not None:
        compact_write(
            previous_command, previous_kv_pairs, start_addr, end_addr, values,addr_is_offset
        )


def compact_write(command, kv_pairs, start_addr, end_addr, values, addr_is_offset):
    addr_range = f"{start_addr}-{end_addr}" if start_addr != end_addr else start_addr
    value_str = rle_encode(values)

    # Reconstruct the key-value pair string
    kv_pairs_str = " ".join(f"{k}={v}" for k, v in kv_pairs.items())

    print(f"{command} {kv_pairs_str} {'offset' if addr_is_offset else 'addr'}={addr_range} value={value_str}", flush=True)


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
    if len(sys.argv) > 2:
        print(f"Usage: {sys.argv[0]} <log_file>", flush=True)
    elif len(sys.argv) == 1:
        process_log(sys.stdin)
    else:
        with open(sys.argv[1], "r") as log_file:
            process_log(log_file)

