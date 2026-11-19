# CSPB 3753 -- SCHED_WRR: a weighted round-robin scheduler for Linux 6.8
#
#   make get-kernel   download Linux 6.8.12 into ./linux and apply the starter code
#   make kernel       build the kernel for the test VM (fast after the first time)
#   make initramfs    build the VM's root filesystem (busybox + user/ programs)
#   make run          boot the VM with an interactive shell (leave with: poweroff)
#   make test         boot the VM, run every test, print your score
#   make test TESTS="yield smp_spread"   run only some tests
#   make patch        write submission/wrr.patch for submission
#   make clean        delete build/ (keeps ./linux and your work)

TESTS ?=

all: kernel initramfs

get-kernel:
	scripts/get-kernel.sh

kernel:
	scripts/build-kernel.sh

initramfs:
	scripts/mkinitramfs.sh

run: kernel initramfs
	scripts/run-qemu.sh

test: kernel initramfs
	scripts/test.sh $(TESTS)

patch:
	scripts/make-patch.sh

clean:
	rm -rf build
	$(MAKE) -C user clean

.PHONY: all get-kernel kernel initramfs run test patch clean
