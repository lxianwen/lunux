
# 不使用wmemmap做容量限制，满内存启动内核
sudo kexec -l /boot/vmlinuz-5.15.114+ --initrd=/boot/initrd.img-5.15.114+ --command-line="root=/dev/mapper/ubuntu--vg-ubuntu--lv ro"
sudo kexec -e 