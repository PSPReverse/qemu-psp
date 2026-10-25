#!/bin/sh
# Run only the firmware-free AMD PSP qtests (see tests/qtest/README-psp.md)
# and the register-table generator test. No firmware is needed.
#
#   scripts/psp-qtest.sh                  build what is needed, run everything
#   PSP_QTEST_PENDING=1 scripts/psp-qtest.sh   also run the pending-fixes group
#   PSP_QTEST_NO_BUILD=1 scripts/psp-qtest.sh  do not run ninja first
#   PSP_BUILD_DIR=build-x scripts/psp-qtest.sh use another build directory
#
# The test binaries are run directly instead of through "meson test", which
# refuses a build directory configured by a different meson version.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build=${PSP_BUILD_DIR:-$root/build-nix}
case $build in /*) ;; *) build=$root/$build ;; esac
QTEST_QEMU_BINARY=${QTEST_QEMU_BINARY:-$build/qemu-system-arm}
export QTEST_QEMU_BINARY

tests="amd-psp-mailbox-test amd-psp-ic-test amd-psp-timer-test
amd-psp-ccp-test amd-psp-x86-test amd-psp-migrate-test amd-psp-reset-test
amd-psp-todo-test amd-psp-pending-fixes-test"

if [ -z "${PSP_QTEST_NO_BUILD:-}" ]; then
    targets=
    for t in $tests; do targets="$targets tests/qtest/$t"; done
    # shellcheck disable=SC2086
    nice -n 19 ninja -C "$build" -j"${PSP_QTEST_JOBS:-4}" qemu-system-arm \
        $targets || exit 1
fi

failed=
start=$(date +%s)
for t in $tests; do
    printf '== %s\n' "$t"
    "$build/tests/qtest/$t" || failed="$failed $t"
done
printf '== psp/regs/test_gen_regs.py\n'
python3 "$root/psp/regs/test_gen_regs.py" || failed="$failed test_gen_regs.py"

printf 'finished in %s s\n' "$(( $(date +%s) - start ))"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
echo "all PSP tests passed"
