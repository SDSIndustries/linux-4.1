cmd_crypto/algif_rng.ko := /usr/bin/arm-linux-gnueabi-ld -EL -r  -T ./scripts/module-common.lds --build-id  -o crypto/algif_rng.ko crypto/algif_rng.o crypto/algif_rng.mod.o
