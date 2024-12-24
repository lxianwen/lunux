#!/bin/bash

source env.sh
echo "scripts_root_path: "$scripts_root_path
echo "GUPS_PATH: "$GUPS_PATH

sudo sysctl page_hotness.autonuma_enable=0
sudo sysctl page_hotness.demote_enable=1
sudo sysctl page_hotness.kunmapd_enable=1

sudo sysctl page_hotness.page_unmap_cold_threshold_sec=20
sudo sysctl page_hotness.page_recently_accessed_threshold_sec=1

sudo sysctl page_hotness.gups_enable=1

mkdir my_scripts
script_path="my_scripts/temp.sh"
out_dir_prefix=""

# gups_args="4 100000000 32 8 30" # 4 thread, 10亿次访问, 4M 1M
# gups_args="4 1000000000 32 8 30" # 4 thread, 10亿次访问, 16G 1G
gups_args="4 1000000000 34 8 32" # 4 thread, 10亿次访问, 16G 4G
# gups_args="16 1000000000 34 8 32" # 16 thread, 10亿次访问, 16G 4G
method_type="ours"

run_gups()
{
    name=$1

    out_dir_prefix=$name

    script_absolute_path=$(realpath "$script_path")
    caller_script_absolute_path=$(realpath $0)

    # /home/dell/lmy/my_scripts/pebs_syscall/kill_pebs_syscall
    # echo 3 | sudo tee /proc/sys/vm/drop_caches
    # free
    # sync
    # sudo killall -9 lbm_base.none

    echo -e "\n=== generating script...."
    cd /home/dell/lmy/my_scripts/gen_cmd

    python3 gen_cmd.py\
        --add_out_dir_time_suffix\
        --cgroup_prefix="cgexec -g cpu:mygroup"\
        --benchmark_path="$GUPS_PATH"\
        --benchmark_args="$gups_args"\
        --out_script_path="$script_absolute_path"\
        --caller_script_path="$caller_script_absolute_path"\
        --out_dir_prefix="$out_dir_prefix"\
        --method_type="$method_type"\
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

bash $scripts_root_path/gen_cmd/common_scripts/init.sh "8G"

# check cgroup
if [[ ! -d "/sys/fs/cgroup/mygroup" ]]; then
    sudo cgcreate -g cpu,memory:mygroup
    sudo chown $USER:$USER -R /sys/fs/cgroup
fi

# echo 3 | sudo tee /proc/sys/vm/drop-caches
# free
# sync
# run_gups "3-21-sync1-4Ghot-nopebs-gups"

# gups_args="4 1000000000 34 8 32" # 4 thread, 10亿次访问, 16G 4G
# sudo sysctl page_hotness.page_unmap_cold_threshold_sec=1
# run_gups "6-21-4thread-gups-test"

gups_args="16 1000000000 34 8 32" # 16 thread, 10亿次访问, 16G 4G
sudo sysctl page_hotness.page_unmap_cold_threshold_sec=20
run_gups "gups-16thread-test"
