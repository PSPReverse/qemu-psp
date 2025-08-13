DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

$DIR/configure \
    --target-list=arm-softmmu\
    --without-default-features\
    --enable-slirp\
    --enable-fdt=internal\
    --audio-drv-list=\
    --enable-debug\
    --enable-plugins\
    --enable-nettle
