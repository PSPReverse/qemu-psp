from subprocess import Popen
import yaml

from pathlib import Path
import gdb

import time
import os
import shlex
import signal
import struct



BUILD_DIR = Path(__file__).parents[2] / "build"  # / "manual"
LOG_PATH = BUILD_DIR / "qemu.log"

dependent_bps = []


def log_message(msg):
    print(msg)
    with LOG_PATH.open("a") as f:
        f.write(msg + "\n")


class Setup(gdb.Command):

    def __init__(self, generation, on_chip_bl, off_chip_bl) -> None:
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
            f'{BUILD_DIR}/qemu-system-arm -d "guest_errors,unimp,trace:psp_*,trace:ccp_passthrough,trace:ccp_rsa,trace:ccp_sha*,trace:ccp*" -gdb tcp::{self.gdb_port} -S '
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
        self.kill_and_restart()
        # some time to start the emulator
        time.sleep(0.5)
        gdb.execute(f"target remote localhost:{self.gdb_port}")
        gdb.execute("sync")
        gdb.execute("si")
        for bp in dependent_bps:
            bp.enabled = False


class JumpTo(gdb.Command):
    def __init__(self):
        super(JumpTo, self).__init__("jump_to", gdb.COMMAND_USER)

    def invoke(self, args, _from_tty, temporary=True):
        argv = gdb.string_to_argv(args)
        assert len(argv) == 2
        overwrite_location = int(argv[0], 0)  # Parse hex string
        target = int(argv[1], 0)  # Parse hex string
        diff = target - overwrite_location
        assert diff > 0
        bp = gdb.Breakpoint(f"*{argv[0]}", temporary=temporary)
        bp.commands = f"jump *{argv[1]}\n end"  # Jump to target


jump = JumpTo()


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


class LoggingMemoryBreakpoint(gdb.Breakpoint):
    def __init__(self, addr: int, address: str, ctype:str, msg: str = None, should_stop=False):
        super(LoggingMemoryBreakpoint, self).__init__(f"*{addr}")
        self.addr = addr
        self.address = address
        self.ctype = ctype
        self.msg = msg
        self.should_stop = should_stop

    def stop(self) -> bool:
        reg_value = int(gdb.parse_and_eval(f"*({self.ctype}*) {self.address}"))
        msg = f"{hex(self.addr)}: {self.ctype} at {self.address:#x} = {hex(reg_value)}" + (
            f" msg = {self.msg}" if self.msg else ""
        )
        log_message(msg)
        return self.should_stop


class ProcessArray(gdb.Command):

    __tos_proc_format = ">68sBxBxBBBBBxBxQ4xI4xI8xIH10x"
    __proc_size = struct.calcsize(__tos_proc_format)
    assert __proc_size == 128
    __proc_field_names = ["regs", "next_pid", "prev_pid", "init_q", "current_q", "active", "m_m_idx", "pid", "unschedule_count", "alarm", "2_thr_ctx", "3_thr_ctx", "domain", "next_pid?"]
    __base_addr = 0x6c00
    def __init__(self):
        super().__init__("procs", gdb.COMMAND_USER)

    def invoke(self, args, from_tty):
        argv = gdb.string_to_argv(args)
        if len(argv) == 1:
            start = int(argv[0], 0)
            end = int(argv[0], 0) + 1
        elif len(argv) != 2:
            raise gdb.GdbError("need at least one param")
        else:
            start = int(argv[0], 0)
            end = int(argv[1], 0)

        if start < 0 or end <= start:
            raise gdb.GdbError("range invalid")

        header = "idx | " + " | ".join(f"{name:^16}" for name in self.__proc_field_names)
        sep = "-" * len(header)
        print(header)
        print(sep)

        inf = gdb.inferiors()[0]
        for i in range(start, end):
            addr = self.__base_addr + i * self.__proc_size
            mem = inf.read_memory(addr, self.__proc_size).tobytes()
            vals = struct.unpack(self.__tos_proc_format, mem)

            # format each field
            out = []
            for name, val in zip(self.__proc_field_names, vals):
                if name == "regs":
                    out.append(str(hex(list(val)[0])))
                else:
                    out.append(str(hex(val)))
            print(f"{str(hex(i)):^3} | " + " | ".join(f"{v:^16}" for v in out))


class ApplyInputCommand(gdb.Command):
    """Apply the binary input specified at the target the same way the fuzzer would

    This is super brittle so make sure the apply input function is the fuzzer is kept in sync with this
    """

    def __init__(self, config):
        super(ApplyInputCommand, self).__init__("apply_input", gdb.COMMAND_USER)
        self.config = config

    def invoke(self, args, _from_tty):
        argv = gdb.string_to_argv(args)
        assert len(argv) == 1
        input_path = Path(argv[0])
        with open(input_path, "rb") as f:
            data = f.read()
        input_types = [ "psp", ] #"flash", "x86"] // TODO: Support the other two input types
        print(self.config["input"])
        for input_type,inputs in [(input_type,self.config["input"].get(input_type, [])) for input_type in input_types]:
            if not inputs:
                continue
            # Variable inputs
            variable_inputs = inputs.get("input", [])
            for variable_input in variable_inputs:
                addr = variable_input["addr"]
                size = variable_input["size"]
                target_data = data[:size]
                print(f"Variable write {input_type} {hex(addr)} to {target_data}")
                data = data[size:]
                for i, byte in enumerate(target_data):
                    expr = f"set *((char*){addr+i}) = {hex(byte)}"
                    #print(expr)
                    gdb.execute(expr)
            # Constant inputs
            fixed_inputs = inputs.get("fixed", [])
            for variable_input in fixed_inputs:
                addr = variable_input["addr"]
                value = variable_input["val"]
                print(f"Constant write {hex(addr)} to {target_data}")
                # Assume all fixed inputs are integer length
                gdb.execute(f"set *{addr} = {hex(value)}")
        if self.config["input"]["mailbox"]:
            # TODO we can't just invoke the function the fuzzer uses to apply the input
            # So we have to hardcode the address here
            for addr in [ 0x03010570 + i*4 for i in range(3)]:
                value = int.from_bytes(data[:4], "little")
                data = data[4:]
                print(f"Mailbox write {hex(addr)} to {value}")
                gdb.execute(f"set *{addr} = {hex(value)}")


def apply_config(config_path: Path) -> Setup:
    """Central entrypoint for applying the config to the gdb session"""
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
    for tunnel in config["tunnels"]:
        action = tunnel["action"]
        if action == "Jump":
            jump.invoke(f"{tunnel['source']} {tunnel['target']}", False)
        elif action == "PermaJump":
            jump.invoke(f"{tunnel['source']} {tunnel['target']}", False, temporary=False)
        elif action == "SetConstant":
            bp = gdb.Breakpoint(f"*{tunnel['addr']}")
            bp.commands = (
                f"set ${str(tunnel['target']).lower()}={tunnel['value']} \n continue \n end"
            )
            bp.silent = True
        elif action == "SetMemory":
            # TODO: do not use this for fuzzing, this is not implemented in rust as of now
            bp = gdb.Breakpoint(f"*{tunnel['addr']}")
            if tunnel['target'][0] == '*' and len(tunnel['target']) == 3:
                # assume dereferenced register
                bp.commands = (
                    f"set *${str(tunnel['target'])[1:].lower()}={tunnel['value']} \n continue \n end"
                )
            else:
                bp.commands = (
                    f"set {{unsigned int}}{str(tunnel['target']).lower()}={tunnel['value']} \n continue \n end"
                )
            bp.silent = True

        elif action == "LogRegister":
            print(f"Logging register {tunnel['target']} at 0x{tunnel['addr']:x}")
            LoggingBreakpoint(tunnel['addr'], tunnel['target'])
    ApplyInputCommand(config)
    if config.get("harness", None):
        LoggingBreakpoint(config["harness"]["start"], "pc", "Start of fuzzing. Run apply_input <path>", True)
        for sink in config["harness"]["sinks"]:
            LoggingBreakpoint(sink, "R0", "Reached sink", True)
    return Setup(
        gen,
        config_path.parent.parent.resolve() / config["qemu"]["on_chip_bl_path"],
        config_path.parent.parent.resolve() / config["flash"]["base"],
    )
