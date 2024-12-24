sudo kexec -p /boot/vmlinuz-5.15.0-88-generic\
    --initrd=/boot/initrd.img-5.15.0-88-generic \
    --append="root=/dev/mapper/ubuntu--vg-ubuntu--lv ro 1 irqpoll nr_cpus=1 reset_devices"

# sudo sysctl kernel.softlockup_panic=1
# sudo sysctl kernel.panic_on_io_nmi=1
# sudo sysctl kernel.panic_on_unrecovered_nmi=1
# sudo sysctl kernel.unknown_nmi_panic=1
# sudo sysctl kernel.hardlockup_panic=1