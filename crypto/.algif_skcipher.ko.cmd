cmd_crypto/algif_skcipher.ko := /usr/bin/arm-linux-gnueabi-ld -EL -r  -T ./scripts/module-common.lds --build-id  -o crypto/algif_skcipher.ko crypto/algif_skcipher.o crypto/algif_skcipher.mod.o
