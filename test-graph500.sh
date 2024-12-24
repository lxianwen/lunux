#!/bin/bash

# 为了使普通用户使用pebs
# sudo sysctl -w kernel.perf_event_paranoid=-1
# echo 3 | sudo tee /proc/sys/vm/drop_caches

source env.sh

echo "scripts_root_path: "$scripts_root_path
echo "GRAPH500_PATH: "$GRAPH500_PATH

sudo sysctl page_hotness.autonuma_enable=0
sudo sysctl page_hotness.demote_enable=1
sudo sysctl page_hotness.kunmapd_enable=1

sudo sysctl page_hotness.page_unmap_cold_threshold_sec=20
sudo sysctl page_hotness.page_recently_accessed_threshold_sec=1

mkdir my_scripts
tmp_script_path="my_scripts/temp.sh"
out_dir_prefix=""

run_gen_graph500()
{
    num=$1
    script_absolute_path=$(realpath "$tmp_script_path")
    caller_script_absolute_path=$(realpath $0)

    echo -e "\n=== generating script...."
    cd $scripts_root_path/gen_cmd
    python3 gen_cmd.py\
        --add_out_dir_time_suffix\
        --benchmark_path="$GRAPH500_PATH"\
        --out_script_path="$script_absolute_path"\
        --caller_script_path="$caller_script_absolute_path"\
        --out_dir_prefix="$out_dir_prefix"\
        --benchmark_args "$num $num"\
        --method_type="ours"\
        --log_numa_maps\
        --log_vmstat\
        --log_sysctl\
        --log_dmesg\
        --quiet
    cd -

    echo -e "\n=== running script...."
    bash $tmp_script_path

    echo -e "\n=== finishing...."
}

run_graph500()
{
    name=$1
    num=$2

    out_dir_prefix=$name
    run_gen_graph500 $num
}


# check cgroup
if [[ ! -d "/sys/fs/cgroup/mygroup" ]]; then
    sudo cgcreate -g cpu,memory:mygroup
    sudo chown $USER:$USER -R /sys/fs/cgroup
fi

bash $scripts_root_path/gen_cmd/common_scripts/init.sh "8G"
run_graph500 "test_graph500_25" 25