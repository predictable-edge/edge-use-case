#!/bin/bash

usage() {
    echo "Usage: $0 -c CPU_RANGE"
    echo "Example:"
    echo "  $0 -c 0-11"
    echo "  $0 -c 0,2,4-6"
    echo "  This will restrict all non-Docker processes to specified CPUs"
    exit 1
}

validate_cpu_range() {
    local cpu_range=$1
    
    IFS=',' read -ra RANGES <<< "$cpu_range"
    
    for range in "${RANGES[@]}"; do
        if [[ $range =~ ^[0-9]+$ ]]; then
            if [ $range -gt 23 ]; then
                echo "Error: CPU number $range is greater than 23"
                exit 1
            fi
        elif [[ $range =~ ^[0-9]+-[0-9]+$ ]]; then
            local start=$(echo $range | cut -d'-' -f1)
            local end=$(echo $range | cut -d'-' -f2)
            
            if [ $start -gt $end ]; then
                echo "Error: Invalid range $range: start must be less than end"
                exit 1
            fi
            
            if [ $end -gt 23 ]; then
                echo "Error: CPU number $end in range $range is greater than 23"
                exit 1
            fi
        else
            echo "Error: Invalid CPU format in '$range'. Must be number or range (e.g., '0' or '0-11')"
            exit 1
        fi
    done
}

setup_cgroups() {
    local system_cpus=$1
    
    mkdir -p /sys/fs/cgroup/system
    
    echo "$system_cpus" > /sys/fs/cgroup/system/cpuset.cpus
    echo "0" > /sys/fs/cgroup/system/cpuset.mems
    
    for pid in $(ps -ef | awk '{print $2}'); do
        if ! grep -q docker /proc/$pid/cgroup 2>/dev/null && \
           ! grep -q docker /proc/$pid/cmdline 2>/dev/null && \
           ! grep -q containerd /proc/$pid/cmdline 2>/dev/null; then
            echo $pid > /sys/fs/cgroup/system/cgroup.procs 2>/dev/null
        fi
    done
}

cleanup_cgroups() {
    for pid in $(cat /sys/fs/cgroup/system/cgroup.procs 2>/dev/null); do
        echo $pid > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    done
    
    rmdir /sys/fs/cgroup/system 2>/dev/null
    
    echo "System restored to original state"
}

while getopts ":c:h" opt; do
    case $opt in
        c)
            cpu_range="$OPTARG"
            ;;
        h)
            usage
            ;;
        \?)
            echo "Invalid option: -$OPTARG"
            usage
            ;;
    esac
done

if [ -z "$cpu_range" ]; then
    echo "Error: CPU range must be specified with -c option"
    usage
fi

validate_cpu_range "$cpu_range"

trap cleanup_cgroups EXIT

echo "Setting up CPU isolation for non-Docker processes to CPUs: $cpu_range"
setup_cgroups "$cpu_range"

echo "CPU isolation is active. Press Ctrl+C to restore system to original state."
while true; do sleep 1; done