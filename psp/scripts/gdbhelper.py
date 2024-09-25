#!/usr/bin/env python3

import gdb
import sys, os, shlex, signal, time
from subprocess import Popen

from pathlib import Path

sys.path.append(os.path.dirname(__file__))


bp_defs = []
dependent_bps = []
for bp_def in bp_defs:
    bp = gdb.Breakpoint(f"*{bp_def[0]}")
    bp.commands = f'print "{bp_def[1]}" \n end'
    bp.enabled = False
    dependent_bps.append(bp)

BUILD_DIR = Path(__file__).parents[2] / "build" / "manual"

LOG_PATH = BUILD_DIR / "qemu.log"


def log_message(msg):
    print(msg)
    with LOG_PATH.open("a") as f:
        f.write(msg + "\n")


gdb.execute("set pagination off")


class Setup(gdb.Command):

    def __init__(self, generation, on_chip_bl, off_chip_bl):
        super(Setup, self).__init__("setup", gdb.COMMAND_USER)
        print(BUILD_DIR.absolute())
        self.generation = generation
        self.on_chip_bl = on_chip_bl
        self.off_chip_bl = off_chip_bl
        self.gdb_port = 1234
        self.monitor_port = 4444

    def kill_and_restart(self):
        path_to_pid = BUILD_DIR / "pid.txt"
        try:
            gdb.execute("detach")
        except gdb.error as e:
            # we might not be attached
            pass
        try:
            with open(path_to_pid, "r") as file:
                data = file.read().rstrip()
                print(f"Attempting to kill QEMU with pid {data}")
                os.kill(int(data), signal.SIGKILL)
        except Exception as e:
            # best effort killing
            print(e)
        cmd = (
            f'{BUILD_DIR}/qemu-system-arm -d "guest_errors,unimp,trace:psp_*,trace:ccp_passthrough,trace:ccp_rsa,trace:ccp_sha*" -gdb tcp::{self.gdb_port} -S '
            f"--machine {self.generation} -device loader,file={self.on_chip_bl},addr=0xffff0000,force-raw=on "
            f"-global driver=amd_psp.smnflash,property=flash_img,value={self.off_chip_bl} "
            f"-chardev socket,id=mon0,host=127.0.0.1,port={self.monitor_port},server=on,wait=off "
            "-monitor chardev:mon0"
        )
        print(f"Starting QEMU with command: {cmd}")
        # yes this leaks the FD until the gc collects the cmd, but I can't figure out a better way rn
        qemu_log = open(LOG_PATH, "a")
        cmd = Popen(
            shlex.split(cmd), stdout=qemu_log, stderr=qemu_log, start_new_session=True
        )

        print(f"New qemu has PID {cmd.pid}")
        with open(path_to_pid, "w") as f:
            f.write(str(cmd.pid))

    def invoke(self, args, _from_tty):
        global BUILD_DIR
        global LOG_PATH
        argv = gdb.string_to_argv(args)
        self.os_mem_hack = False
        if len(argv) > 0 and bool(argv[0]):
            print("OS MEM HACKING")
            self.gdb_port = 1235
            self.monitor_port = 4445
            BUILD_DIR = BUILD_DIR.parent / "osmem"
            LOG_PATH = BUILD_DIR / "qemu.log"
            self.os_mem_hack = True
        self.kill_and_restart()
        # some time to start the emulator
        time.sleep(0.5)
        gdb.execute(f"target remote localhost:{self.gdb_port}")
        if not self.os_mem_hack:
            gdb.execute("sync")
        gdb.execute("si")
        for bp in dependent_bps:
            bp.enabled = False


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


class JumpTo(gdb.Command):
    def __init__(self):
        super(JumpTo, self).__init__("jump_to", gdb.COMMAND_USER)

    def invoke(self, args, _from_tty):
        argv = gdb.string_to_argv(args)
        assert len(argv) == 2
        overwrite_location = int(argv[0], 0)  # Parse hex string
        target = int(argv[1], 0)  # Parse hex string
        diff = target - overwrite_location
        assert diff > 0
        bp = gdb.Breakpoint(f"*{argv[0]}")
        bp.commands = f"jump *{argv[1]}\n end"


import yaml

config_path = Path(os.environ["FUZZER_CONFIG"])
with open(config_path, "r") as f:
    config = yaml.safe_load(f)

zen = config["qemu"]["zen"]
if zen == "Zen":
    gen = "amd-psp-zen"
elif zen == "Zen+":
    gen = "amd-psp-zen+"
elif zen == "Zen2":
    gen = "amd-psp-zen2"
elif zen == "Zen3":
    gen = "amd-psp-zen3"
elif zen == "ZenTesla":
    gen = "amd-psp-zentesla"
else:
    raise Exception(f"Unkown zen version: {zen}")

s = Setup(
    gen,
    config_path.parent.parent.resolve() / config["qemu"]["on_chip_bl_path"],
    config_path.parent.parent.resolve() / config["flash"]["base"],
)
Bisect()
j = JumpTo()


class LoggingBreakpoint(gdb.Breakpoint):
    def __init__(self, addr: int, target: str, msg: str = None, should_stop=False):
        super(LoggingBreakpoint, self).__init__(f"*{addr}")
        self.addr = addr
        self.target = target
        self.msg = msg
        self.should_stop = should_stop

    def stop(self) -> bool:
        reg_value = int(gdb.parse_and_eval(f"${self.target.lower()}"))
        msg = f"{hex(self.addr)}: {self.target} = {hex(reg_value)}" + (
            f" msg = {self.msg}" if self.msg else ""
        )
        log_message(msg)
        return self.should_stop


for tunnel in config["tunnels"]:
    action = tunnel["action"]
    if action == "Jump":
        j.invoke(f"{tunnel["source"]} {tunnel["target"]}", False)
    elif action == "SetConstant":
        bp = gdb.Breakpoint(f"*{tunnel["addr"]}")
        bp.commands = (
            f"set ${tunnel["target"].lower()}={tunnel["value"]} \n continue \n end"
        )
        bp.silent = True
    elif action == "LogRegister":
        print(f"Logging register {tunnel['target']} at {tunnel['addr']:x}")
        LoggingBreakpoint(tunnel["addr"], tunnel["target"])

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
