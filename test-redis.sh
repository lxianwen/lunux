#!/bin/bash

source env.sh
echo "scripts_root_path: "$scripts_root_path
echo "REDIS_ROOT: "$REDIS_ROOT

sudo sysctl page_hotness.autonuma_enable=0
sudo sysctl page_hotness.demote_enable=1
sudo sysctl page_hotness.kunmapd_enable=1

sudo sysctl page_hotness.page_unmap_cold_threshold_sec=20
sudo sysctl page_hotness.page_recently_accessed_threshold_sec=1

mkdir my_scripts
tmp_script_path="my_scripts/temp.sh"
out_dir_prefix=""

run_gen_redis()
{
    script_absolute_path=$(realpath "$script_path")
    caller_script_absolute_path=$(realpath $0)

    echo -e "\n=== generating script...."
    cd $scripts_root_path/gen_cmd

    # test_db="$scripts_root_path/gen_cmd/bench_cmd/debug_redis_db.sh"
    test_db="$scripts_root_path/gen_cmd/bench_cmd/test_redis_db.sh"

    python3 gen_cmd.py\
        --add_out_dir_time_suffix\
        --daemon_script_path="$test_db"\
        --benchmark_path="$REDIS_ROOT/src/redis-server"\
        --benchmark_args="$REDIS_ROOT/redis.conf"\
        --out_script_path="$script_absolute_path"\
        --caller_script_path="$caller_script_absolute_path"\
        --out_dir_prefix="$out_dir_prefix"\
        --method_type="ours"\
        --log_my_stat\
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


run_redis()
{
    name=$1

    out_dir_prefix=$name
    script_path="my_scripts/temp.sh"

    run_gen_redis
}

bash $scripts_root_path/gen_cmd/common_scripts/init.sh "8G"

run_redis "6-22-redis"