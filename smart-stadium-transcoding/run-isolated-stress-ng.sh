#!/bin/bash

usage() {
    echo "Usage: $0 -c CPU_RANGE -l CPU_LOAD -- PROGRAM [PROGRAM_ARGS...]"
    echo "Example:"
    echo "  $0 -c 0-11 -l 20 -- ./program -i input.mp4 -f 60"
    echo "  This will allocate CPU 0-11 for both your program and stress-ng with 20% CPU load"
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

# 计算CPU范围内的核心数
get_cpu_count() {
    local cpu_range=$1
    local start=$(echo $cpu_range | cut -d'-' -f1)
    local end=$(echo $cpu_range | cut -d'-' -f2)
    echo $((end - start + 1))
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
    if [ ! -z "$stress_pid" ]; then
        kill $stress_pid 2>/dev/null
    fi

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

cpu_range=""
cpu_load=20  # 默认CPU负载为20%
cmd_args=()
found_separator=0

while [[ $# -gt 0 ]]; do
    case $1 in
        -c)
            cpu_range="$2"
            shift 2
            ;;
        -l)
            cpu_load="$2"
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

cpu_count=$(get_cpu_count "$cpu_range")

echo "Starting program on CPU: $cpu_range"
"${cmd_args[@]}" &
pid=$!
echo $pid > /sys/fs/cgroup/app/cgroup.procs

echo "Starting stress-ng on CPU: $cpu_range with $cpu_count cores at ${cpu_load}% load"
stress-ng --cpu $cpu_count --cpu-load $cpu_load &
stress_pid=$!
echo $stress_pid > /sys/fs/cgroup/app/cgroup.procs

wait $pid
kill $stress_pid 2>/dev/null