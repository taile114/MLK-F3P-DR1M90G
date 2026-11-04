mmc part
load ${devtype} ${devnum}:${distro_bootpart} 0x10000000 ${prefix}uImage.lz4
load ${devtype} ${devnum}:${distro_bootpart} 0x08000000 ${prefix}system.dtb
setenv bootargs console=ttyS0,115200n8 earlycon=uart,mmio32,0xf8401000 loglevel=7 root=/dev/mmcblk0p2 rootwait rootfstype=ext4 rw
bootm 0x10000000 - 0x08000000
