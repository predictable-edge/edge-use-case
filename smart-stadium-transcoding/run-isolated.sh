#!/bin/bash

# Function to display usage
usage() {
    echo "Usage: $0 -c CPU_RANGE -- PROGRAM [PROGRAM_ARGS...]"
    echo "Example:"
    echo "  $0 -c 0-11 -- ./program -i input.mp4 -f 60"
    echo "  This will allocate CPUs 0-11 for your program"
    exit 1
}

# Function to validate the CPU range
validate_cpu_range() {
    local cpu_range=$1
    if ! [[ $cpu_range =~ ^[0-9,-]+$ ]]; then
        echo "Error: Invalid CPU range format. Should be like '0-11,13,15'"
        exit 1
    fi

    IFS=',' read -r -a ranges <<< "$cpu_range"
    for range in "${ranges[@]}"; do
        if [[ "$range" =~ ^[0-9]+-[0-9]+$ ]]; then
            start=$(echo "$range" | cut -d'-' -f1)
            end=$(echo "$range" | cut -d'-' -f2)
            if [ "$start" -gt "$end" ] || [ "$end" -ge "$(nproc)" ]; then
                echo "Error: Invalid range $range. Start must be less than end and within valid CPU range."
                exit 1
            fi
        elif [[ "$range" =~ ^[0-9]+$ ]]; then
            if [ "$range" -ge "$(nproc)" ]; then
                echo "Error: CPU $range exceeds the available cores ($(nproc))."
                exit 1
            fi
        else
            echo "Error: Invalid CPU range entry: $range"
            exit 1
        fi
    done
}

# Function to get remaining CPUs
get_remaining_cpus() {
    local used_range=$1
    local used_cpus=()

    IFS=',' read -r -a ranges <<< "$used_range"
    for range in "${ranges[@]}"; do
        if [[ "$range" =~ ^[0-9]+-[0-9]+$ ]]; then
            start=$(echo "$range" | cut -d'-' -f1)
            end=$(echo "$range" | cut -d'-' -f2)
            for ((i=start; i<=end; i++)); do
                used_cpus+=("$i")
            done
        else
            used_cpus+=("$range")
        fi
    done

    local all_cpus=()
    for ((i=0; i<$(nproc); i++)); do
        all_cpus+=("$i")
    done

    local remaining_cpus=()
    for cpu in "${all_cpus[@]}"; do
        if ! [[ " ${used_cpus[*]} " =~ " $cpu " ]]; then
            remaining_cpus+=("$cpu")
        fi
    done

    IFS=','; echo "${remaining_cpus[*]}"
}

# Function to set up CPU isolation
setup_isolation() {
    local app_cpus=$1
    local system_cpus
    system_cpus=$(get_remaining_cpus "$app_cpus")

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

# Function to clean up isolation settings and terminate all related processes
cleanup_isolation() {
    echo "Cleaning up resources..."

    for pid in $(cat /sys/fs/cgroup/app/cgroup.procs 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null
    done

    for pid in $(cat /sys/fs/cgroup/system/cgroup.procs 2>/dev/null); do
        echo $pid > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    done

    rmdir /sys/fs/cgroup/app 2>/dev/null
    rmdir /sys/fs/cgroup/system 2>/dev/null

    echo "System restored to original state."
}

# Main script logic
cpu_range=""
cmd_args=()
found_separator=0
running_pid=""

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

# Validate required arguments
if [ -z "$cpu_range" ]; then
    echo "Error: CPU range must be specified with -c option"
    usage
fi

validate_cpu_range "$cpu_range"

if [ ${#cmd_args[@]} -eq 0 ]; then
    echo "Error: No program specified"
    usage
fi

# Ensure proper cleanup when interrupted
trap cleanup_isolation EXIT

setup_isolation "$cpu_range"

echo "Starting program on CPU: $cpu_range"
"${cmd_args[@]}" &
running_pid=$!
echo $running_pid > /sys/fs/cgroup/app/cgroup.procs

wait $running_pid