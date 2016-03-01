cmd_drivers/dma/dmatest.ko := /usr/bin/arm-linux-gnueabi-ld -EL -r  -T ./scripts/module-common.lds --build-id  -o drivers/dma/dmatest.ko drivers/dma/dmatest.o drivers/dma/dmatest.mod.o
