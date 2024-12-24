ubuntuBaseImage="/home/dell/lmy/my_images/ubuntu-rootfs-raw-20G.image"
kernelDir=.

bash cp_module.sh

sudo qemu-system-x86_64 \
    --enable-kvm\
    -machine pc,nvdimm=on \
    -m 2G,slots=4,maxmem=32G \
    -nographic -kernel $kernelDir/vmlinux \
    -smp cores=4,threads=1,sockets=2 \
    -hda $ubuntuBaseImage \
    -object memory-backend-ram,id=mem0,size=1G  \
    -object memory-backend-ram,id=mem1,size=1G  \
    -numa node,memdev=mem0,cpus=0-3,nodeid=0 \
    -numa node,memdev=mem1,cpus=4-7,nodeid=1 \
    -numa node,nodeid=2 -numa node,nodeid=3 \
    -object memory-backend-ram,id=nvdimm1,size=4G\
    -device nvdimm,memdev=nvdimm1,id=nv1,unarmed=off,node=2 \
    -object memory-backend-ram,id=nvdimm2,size=4G\
    -device nvdimm,memdev=nvdimm2,id=nv2,unarmed=off,node=3 \
    -append "console=ttyS0 crashkernel=256M root=/dev/sda rootfstype=ext4 rw loglevel=8"