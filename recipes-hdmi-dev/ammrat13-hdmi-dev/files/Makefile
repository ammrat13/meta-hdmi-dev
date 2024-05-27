# See: poky/meta-skeleton/recipes-kernel/hello-mod/files/Makefile
#
# It seems `Makefile`s for kernel modules are very similar to each other, given
# the heavy lifting is done by the kernel itself. We can just use the template
# and modify `obj-m`.

obj-m := ammrat13-hdmi-dev.o

SRC := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean
