#!/usr/bin/env bash
# pack.sh — build + pack a flashable .ufw from the SDK (run inside the linux/amd64 VM).
# Hardened: Removed silent error suppressions to ensure build integrity.
set -euo pipefail

SDK="${SDK:-$HOME/ac79-sdk}"                      # space-free path (make dislikes spaces)
TARGET="${TARGET:-ac791n_demo_demo_audio}"        # replace with your app target
PB="${PB:?set PB=/path/to/jieli-linux-post-build-tools-*}"
OC=/opt/jieli/common/bin/objcopy
export QT_QPA_PLATFORM=offscreen
export PATH="$PB:$PATH"

cd "$SDK"
make "$TARGET" -j"$(nproc)"                        # -> cpu/wl82/tools/sdk.elf
cd cpu/wl82/tools

for s in text data ram0_data cache_ram_data dynamic_data; do
  "$OC" -O binary -j ".$s" sdk.elf "$s.bin"
done
cat text.bin data.bin dynamic_data.bin ram0_data.bin cache_ram_data.bin > app.bin

isd_download isd_config.ini -gen2 -tonorflash -dev wl82 -boot 0x1c02000 -div1 -wait 300 \
  -uboot uboot.boot -app app.bin cfg_tool.bin -reboot 500 -extend-bin
fw_add -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw
fw_add -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw
ufw_maker -fw_to_ufw jl_isd.fw                                                   # -> jl_isd.ufw

echo "successfully built and packed: $(pwd)/jl_isd.ufw ($(stat -c%s jl_isd.ufw) B)"