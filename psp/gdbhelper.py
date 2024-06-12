#!/usr/bin/env python3

import gdb
import sys, os, shlex, signal, time
from subprocess import Popen

from pathlib import Path

sys.path.append(os.path.dirname(__file__))


class Setup(gdb.Command):

    def __init__(self):
        super(Setup, self).__init__("setup", gdb.COMMAND_USER)
        self.build_dir = Path(__file__).parent.parent / "build" / "manual"
        print(self.build_dir.absolute())

    def kill_and_restart(self):
        path_to_pid = self.build_dir / "pid.txt"
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
            f'{self.build_dir}/qemu-system-arm -d "guest_errors,trace:psp_*" -s -S '
            f'--machine {os.getenv("PSP_GENERATION", "amd-psp-zen2") } -device loader,file={os.environ["PSP_ON_CHIP_BL"]},addr=0xffff0000,force-raw=on '
            f'-global driver=amd_psp.smnflash,property=flash_img,value={os.environ["PSP_FLASH_IMAGE"]}'
        )
        # yes this leaks the FD until the gc collects the cmd, but I can't figure out a better way rn
        qemu_log = open(self.build_dir / "qemu.log", "a")
        cmd = Popen(
            shlex.split(cmd), stdout=qemu_log, stderr=qemu_log, start_new_session=True
        )
        print(f"New qemu has PID {cmd.pid}")
        with open(path_to_pid, "w") as f:
            f.write(str(cmd.pid))

    def invoke(self, _args, _from_tty):
        self.kill_and_restart()
        # some time to start the emulator
        time.sleep(0.5)
        gdb.execute("target remote localhost:1234")
        gdb.execute("sync")
        gdb.execute("si")


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
        assert len(argv) == 2
        first = self.get_location_of_bp(int(argv[0]))
        second = self.get_location_of_bp(int(argv[1]))
        new_pos = first + (second - first) // 2
        pos_str = f"*{hex(new_pos)}"
        print(pos_str)
        gdb.Breakpoint(pos_str)


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


s = Setup()
Bisect()
j = JumpTo()

# overwrite of 0x100ac
bp = gdb.Breakpoint("*0x06988")
bp.commands = "set $r0=0x1 \n continue \n end"
bp.silent = True

# Jump to X86_IRQ_HANLDE_WORKLOOP_MAYBE
j.invoke("0x02b0e 0x2b8a", False)


# overwrite of function that returns error
# bp = gdb.Breakpoint("*0x02b14")
# bp.commands = "set $r0=0x0 \n continue \n end"
# bp.silent = True

# Report error code in GDB
gdb.Breakpoint("*0x00007274").commands = (
    "echo Reached errror state \\n \n x/x 0x32000d8 \n end"
)

# fix function in x86_workloop
bp = gdb.Breakpoint("*0x9236")
bp.commands = "set $r0=0x0 \n continue \n end"
bp.silent = True

s.invoke("", False)
