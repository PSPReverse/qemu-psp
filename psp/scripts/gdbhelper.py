#!/usr/bin/env python3

import gdb
import sys, os, shlex, signal, time
from subprocess import Popen

from pathlib import Path

sys.path.append(os.path.dirname(__file__))
from tunnels import apply_config, LoggingBreakpoint, dependent_bps, log_message

bp_defs = []
for bp_def in bp_defs:
    bp = gdb.Breakpoint(f"*{bp_def[0]}")
    bp.commands = f'print "{bp_def[1]}" \n end'
    bp.enabled = False
    dependent_bps.append(bp)

gdb.execute("set pagination off")
setup = apply_config(Path(os.environ["FUZZER_CONFIG"]))

class Bisect(gdb.Command):
    def __init__(self):
        super(Bisect, self).__init__("bisect", gdb.COMMAND_USER)

    def get_location_of_bp(self, pos: int) -> int:
        bp = [x for x in gdb.breakpoints() if x.number == pos]
        assert len(bp) == 1
        bp = bp[0]
        location = bp.location.lstrip("*0x ")
        return int(location, 16)

    def invoke(self, args, _from_tty):
        argv = gdb.string_to_argv(args)
        assert len(argv) == 2 or len(argv) == 3
        first = self.get_location_of_bp(int(argv[0]))
        second = self.get_location_of_bp(int(argv[1]))
        new_pos = first + (second - first) // 2
        pos_str = f"*{hex(new_pos)}"
        print(pos_str)
        bp = gdb.Breakpoint(pos_str)
        if len(argv) == 3:
            assert argv[2] == "true"
            bp.enabled = False
            dependent_bps.append(bp)

Bisect()

# Report error code in GDB
# This is part of the sinks, so we do it manually
gdb.Breakpoint("*0x7274").commands = (
    "echo Reached errror state \\n \n x/x 0x32000d8 \n end"
)
gdb.Breakpoint("*0x210").commands = "echo Reached wait for interrupt \n end"

LoggingBreakpoint(0xD424, "R0", "Starting load_app_id_from_flash")
LoggingBreakpoint(0xD472, "R0", "Done with load_app_id_from_flash")

LoggingBreakpoint(0xD2EE, "R0", "Starting SecOS", should_stop=True)

dependent_bps.extend(
    [
        LoggingBreakpoint(0x3F00, "R0", "Got SVC call number"),
        LoggingBreakpoint(0x298, "SP", "Returning from SVC call"),
        LoggingBreakpoint(0x298, "LR", "Returning from SVC call"),
        LoggingBreakpoint(0x2C8, "R0", "Returning from UNDEF call"),
        LoggingBreakpoint(0x2C8, "LR", "Returning from UNDEF call"),
        LoggingBreakpoint(0x710, "R0", "Switch to Context"),
        LoggingBreakpoint(0x710, "LR", "Switch to Context"),
        LoggingBreakpoint(0xF6E, "R0", "FIQ called"),
        LoggingBreakpoint(0x10, "LR", "DATA_ABORT triggered", should_stop=True),
        LoggingBreakpoint(0xEB8, "R0", "Already in UNDEF", should_stop=True),
        LoggingBreakpoint(0x200120, "R0", "Known good location", should_stop=True),
        LoggingBreakpoint(0x2000e0, "R0", "Start of memcpy"),
    ]
)


class EnableDependentsBreakpoint(gdb.Breakpoint):
    def stop(self) -> bool:
        for bp in dependent_bps:
            bp.enabled = True
        return False


class CcpMemcopyBreakpoint(gdb.Breakpoint):
    def __init__(self, addr: int):
        super(CcpMemcopyBreakpoint, self).__init__(f"*{addr}")
        self.addr = addr

    def stop(self) -> bool:

        def log_register(register, name, postprocessor=None):
            reg_value = int(gdb.parse_and_eval(f"${register.lower()}"))
            if postprocessor:
                reg_value = postprocessor(reg_value)
            else:
                reg_value = hex(reg_value)
            msg = f"{name}={reg_value}"
            return msg

        log_buffer = ["ccp_memcopy"]
        log_buffer.append(log_register("R0", "src_lo"))
        log_buffer.append(log_register("R1", "dest_lo"))
        log_buffer.append(log_register("R2", "src_len"))

        def get_memtype(x):
            if x == 0:
                return "CCP_MEMTYPE_SYSTEM"
            elif x == 1:
                return "CCP_MEMTYPE_SB"
            elif x == 2:
                return "CCP_MEMTYPE_LOCAL"
            else:
                raise Exception("CCP_MEMTYPE_UNKNOWN")

        log_buffer.append(log_register("R3", "src_memtype", lambda x: get_memtype(x)))
        log_buffer.append(log_register("R6", "dest_memtype", lambda x: get_memtype(x)))
        log_message(" ".join(log_buffer))
        return False


EnableDependentsBreakpoint("*0xD2EE")
