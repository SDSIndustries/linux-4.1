cmd_arch/arm/boot/zImage := /usr/bin/arm-linux-gnueabi-objcopy -O binary -R .comment -S  arch/arm/boot/compressed/vmlinux arch/arm/boot/zImage
