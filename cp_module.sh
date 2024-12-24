# [需修改镜像目录]
image_root=/home/dell/lmy/my_images

linux_kernel_root=.
image_path=$image_root/ubuntu-rootfs-raw-20G.image
mount_dir=$image_root/ubuntu-rootfs-dir

# check
if [[ ! -d $image_root ]]
then
    echo $image_root" not exists"
    exit
fi
if [[ ! -f $image_path ]]
then
    echo $image_path" not exists"
    exit
fi
if [[ ! -d $mount_dir ]]
then
    echo $mount_dir" not exists"
    exit
fi


# mount
sudo mount $image_path $mount_dir
ret=$?
if [[ ! ret ]]
then
    echo "mount $image_path to $mount_dir failed"
    exit
fi
echo "success mounting $image_path to $mount_dir"

# copy
module_path=$mount_dir"/root/my_modules"

sudo mkdir -p $module_path/drivers/dax
sudo mkdir -p $module_path/drivers/nvdimm

sudo rm -r $module_path/drivers/dax
sudo rm -r $module_path/drivers/nvdimm

sudo cp -r $linux_kernel_root/drivers/dax $module_path/drivers/dax
sudo cp -r $linux_kernel_root/drivers/nvdimm $module_path/drivers/nvdimm

echo "success copying modules to $module_path"

# umount
sudo umount $mount_dir
if [[ ! ret ]]
then
    echo "umount $mount_dir failed"
    exit
fi
echo "success umounting $image_path to $mount_dir"