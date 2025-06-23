# This is the object file we are building from our C source.
obj-m += mfs.o

# The 'all' target is the default. It builds the kernel module.
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# The 'clean' target removes all compiled files.
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
