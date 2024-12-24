# sudo journalctl --sync

# memmap需要按照实际情况进行修改
# 4G local DRAM + 4G remote DRAM
sudo kexec -l /boot/vmlinuz-5.15.114huawei+ --initrd=/boot/initrd.img-5.15.114huawei+ \
    --command-line="root=/dev/mapper/ubuntu--vg-ubuntu--lv ro memmap=89G!4G memmap=93G!96G"
sudo kexec -e

# # 2G 2G can't boot
# sudo kexec -l /boot/vmlinuz-5.15.114b1+ --initrd=/boot/initrd.img-5.15.114b1+ \
#     --command-line="root=/dev/mapper/ubuntu--vg-ubuntu--lv ro memmap=91G!4G memmap=95G!96G"
# sudo kexec -e