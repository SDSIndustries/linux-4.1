cmd_arch/arm/boot/dts/at91-sama5d4ek.dtb := mkdir -p arch/arm/boot/dts/ ; /usr/bin/arm-linux-gnueabi-gcc -E -Wp,-MD,arch/arm/boot/dts/.at91-sama5d4ek.dtb.d.pre.tmp -nostdinc -I./arch/arm/boot/dts -I./arch/arm/boot/dts/include -I./drivers/of/testcase-data -undef -D__DTS__ -x assembler-with-cpp -o arch/arm/boot/dts/.at91-sama5d4ek.dtb.dts.tmp arch/arm/boot/dts/at91-sama5d4ek.dts ; ./scripts/dtc/dtc -O dtb -o arch/arm/boot/dts/at91-sama5d4ek.dtb -b 0 -i arch/arm/boot/dts/  -d arch/arm/boot/dts/.at91-sama5d4ek.dtb.d.dtc.tmp arch/arm/boot/dts/.at91-sama5d4ek.dtb.dts.tmp ; cat arch/arm/boot/dts/.at91-sama5d4ek.dtb.d.pre.tmp arch/arm/boot/dts/.at91-sama5d4ek.dtb.d.dtc.tmp > arch/arm/boot/dts/.at91-sama5d4ek.dtb.d

source_arch/arm/boot/dts/at91-sama5d4ek.dtb := arch/arm/boot/dts/at91-sama5d4ek.dts

deps_arch/arm/boot/dts/at91-sama5d4ek.dtb := \
  arch/arm/boot/dts/sama5d4.dtsi \
  arch/arm/boot/dts/skeleton.dtsi \
  arch/arm/boot/dts/include/dt-bindings/clock/at91.h \
  arch/arm/boot/dts/include/dt-bindings/dma/at91.h \
  arch/arm/boot/dts/include/dt-bindings/pinctrl/at91.h \
  arch/arm/boot/dts/include/dt-bindings/interrupt-controller/irq.h \
  arch/arm/boot/dts/include/dt-bindings/gpio/gpio.h \

arch/arm/boot/dts/at91-sama5d4ek.dtb: $(deps_arch/arm/boot/dts/at91-sama5d4ek.dtb)

$(deps_arch/arm/boot/dts/at91-sama5d4ek.dtb):
