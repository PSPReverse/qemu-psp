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
        cmd = Popen(shlex.split(cmd), stdout=qemu_log, stderr=qemu_log)
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

s = Setup()

# overwrite of 0x100ac
gdb.Breakpoint("*0x06988").commands = "set $r0=0x1 \n print $r0 \n end"

s.invoke("", False)
