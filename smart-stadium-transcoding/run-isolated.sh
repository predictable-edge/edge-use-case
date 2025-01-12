#!/bin/bash

usage() {
    echo "Usage: $0 -c CPU_RANGE -- PROGRAM [PROGRAM_ARGS...]"
    echo "Example:"
    echo "  $0 -c 0-11 -- ./program -i input.mp4 -f 60"
    echo "  This will allocate CPU 0-11 for your program"
    exit 1
}

validate_cpu_range() {
    local cpu_range=$1
    if ! [[ $cpu_range =~ ^[0-9]+-[0-9]+$ ]]; then
        echo "Error: Invalid CPU range format. Should be like '0-11'"
        exit 1
    fi
    
    local start=$(echo $cpu_range | cut -d'-' -f1)
    local end=$(echo $cpu_range | cut -d'-' -f2)
    
    if [ $start -gt $end ] || [ $end -gt 23 ]; then
        echo "Error: Invalid CPU range. Must be between 0-23 and start must be less than end"
        exit 1
    fi
}

get_remaining_cpus() {
    local used_range=$1
    local start=$(echo $used_range | cut -d'-' -f1)
    local end=$(echo $used_range | cut -d'-' -f2)
    local all_cpus=""
    
    for ((i=0; i<=23; i++)); do
        if [ $i -lt $start ] || [ $i -gt $end ]; then
            if [ -z "$all_cpus" ]; then
                all_cpus="$i"
            else
                all_cpus="$all_cpus,$i"
            fi
        fi
    done
    echo $all_cpus
}

setup_isolation() {
    local app_cpus=$1
    local system_cpus=$(get_remaining_cpus $app_cpus)
    
    mkdir -p /sys/fs/cgroup/app
    mkdir -p /sys/fs/cgroup/system
    
    echo "$app_cpus" > /sys/fs/cgroup/app/cpuset.cpus
    echo "0" > /sys/fs/cgroup/app/cpuset.mems
    
    echo "$system_cpus" > /sys/fs/cgroup/system/cpuset.cpus
    echo "0" > /sys/fs/cgroup/system/cpuset.mems
    
    for pid in $(ps -ef | awk '{print $2}'); do
        echo $pid > /sys/fs/cgroup/system/cgroup.procs 2>/dev/null
    done
}

cleanup_isolation() {
    for pid in $(cat /sys/fs/cgroup/app/cgroup.procs 2>/dev/null); do
        echo $pid > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    done
    for pid in $(cat /sys/fs/cgroup/system/cgroup.procs 2>/dev/null); do
        echo $pid > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    done
    
    rmdir /sys/fs/cgroup/app 2>/dev/null
    rmdir /sys/fs/cgroup/system 2>/dev/null
    
    echo "System restored to original state"
}

# 初始化变量
cpu_range=""
cmd_args=()
found_separator=0

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -c)
            cpu_range="$2"
            shift 2
            ;;
        -h)
            usage
            ;;
        --)
            found_separator=1
            shift
            cmd_args=("$@")
            break
            ;;
        *)
            if [ $found_separator -eq 1 ]; then
                cmd_args+=("$1")
            fi
            shift
            ;;
    esac
done

# 验证必要参数
if [ -z "$cpu_range" ]; then
    echo "Error: CPU range must be specified with -c option"
    usage
fi

validate_cpu_range "$cpu_range"

if [ ${#cmd_args[@]} -eq 0 ]; then
    echo "Error: No program specified"
    usage
fi

trap cleanup_isolation EXIT

setup_isolation "$cpu_range"

echo "Starting program on CPU: $cpu_range"
"${cmd_args[@]}" &
pid=$!
echo $pid > /sys/fs/cgroup/app/cgroup.procs

wait $pid