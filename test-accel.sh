#!/bin/bash

source env.sh

echo "scripts_root_path: "$scripts_root_path

# spec accel的路径目前是写到gen_cmd.py里面了
# SPEC_ACCEL_PATH="XXXX"

sudo sysctl page_hotness.autonuma_enable=0
sudo sysctl page_hotness.demote_enable=1
sudo sysctl page_hotness.kunmapd_enable=1

sudo sysctl page_hotness.page_unmap_cold_threshold_sec=20
sudo sysctl page_hotness.page_recently_accessed_threshold_sec=1

script_path="my_scripts/temp.sh"
out_dir_prefix=""

run_gen_accel()
{
    script_absolute_path=$(realpath "$script_path")
    caller_script_absolute_path=$(realpath $0)

    echo -e "\n=== generating script...."
    cd $scripts_root_path/gen_cmd

    # 此处的mytest.cfg需要根据情况进行修改
    python3 gen_cmd.py\
        --add_out_dir_time_suffix\
        --set_spec_env\
        --benchmark_path="runaccel"\
        --benchmark_args="--config=mytest.cfg --tune=base --define model=lop --size=ref --noreportable --iterations=1 404.lbm"\
        --out_script_path="$script_absolute_path"\
        --caller_script_path="$caller_script_absolute_path"\
        --out_dir_prefix="$out_dir_prefix"\
        --method_type="ours"\
        --log_numa_maps\
        --log_vmstat\
        --log_sysctl\
        --log_dmesg\
        --quiet
    
    cd -

    echo -e "\n=== running script...."
    bash $script_path

    echo -e "\n=== finishing...."
}


run_accel()
{
    name=$1
    bench=$2

    out_dir_prefix=$name
    script_path="my_scripts/temp.sh"

    run_gen_accel $bench
}

# check cgroup
if [[ ! -d "/sys/fs/cgroup/mygroup" ]]; then
    sudo cgcreate -g cpu,memory:mygroup
    sudo chown $USER:$USER -R /sys/fs/cgroup
fi

bash /home/dell/lmy/my_scripts/gen_cmd/common_scripts/init.sh "4G"
run_accel "test-accel"