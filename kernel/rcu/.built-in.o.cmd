cmd_kernel/rcu/built-in.o :=  /usr/bin/arm-linux-gnueabi-ld -EL    -r -o kernel/rcu/built-in.o kernel/rcu/update.o kernel/rcu/srcu.o kernel/rcu/tiny.o 
