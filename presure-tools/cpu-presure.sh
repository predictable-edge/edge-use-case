#!/bin/bash

# Function to display usage instructions
usage() {
    echo "Usage: $0 -l CPU_LOAD -c CPU_RANGE"
    echo "Example:"
    echo "  $0 -l 90 -c 1-3,5,7"
    echo "  This will run stress-ng on cores 1, 2, 3, 5, and 7 with 90% CPU load."
    exit 1
}

# Initialize variables
cpu_load=""
cpu_range=""

# Parse input arguments
while getopts "l:c:" opt; do
    case $opt in
        l)
            cpu_load="$OPTARG"
            ;;
        c)
            cpu_range="$OPTARG"
            ;;
        *)
            usage
            ;;
    esac
done

# Check if both arguments are provided
if [[ -z "$cpu_load" || -z "$cpu_range" ]]; then
    echo "Error: Missing arguments."
    usage
fi

# Validate CPU load
if ! [[ "$cpu_load" =~ ^[0-9]+$ ]] || [ "$cpu_load" -lt 1 ] || [ "$cpu_load" -gt 100 ]; then
    echo "Error: CPU load must be an integer between 1 and 100."
    exit 1
fi

# Convert CPU range into a list of cores
cpu_list=()
IFS=',' read -r -a ranges <<< "$cpu_range"
for range in "${ranges[@]}"; do
    if [[ "$range" =~ ^[0-9]+-[0-9]+$ ]]; then
        # Handle continuous ranges like 1-3
        start=$(echo "$range" | cut -d'-' -f1)
        end=$(echo "$range" | cut -d'-' -f2)
        if [ "$start" -gt "$end" ]; then
            echo "Error: Invalid range $range. Start must be less than or equal to end."
            exit 1
        fi
        for ((i=start; i<=end; i++)); do
            cpu_list+=("$i")
        done
    elif [[ "$range" =~ ^[0-9]+$ ]]; then
        # Handle individual cores like 5
        cpu_list+=("$range")
    else
        echo "Error: Invalid range format: $range"
        exit 1
    fi
done

# Validate each core in the list
total_cores=$(nproc)
for cpu in "${cpu_list[@]}"; do
    if [ "$cpu" -ge "$total_cores" ]; then
        echo "Error: CPU $cpu exceeds the maximum available cores ($total_cores)."
        exit 1
    fi
done

# Calculate the number of CPUs to use
cpu_count=${#cpu_list[@]}

# Generate the core list as a comma-separated string
cpu_binding=$(IFS=','; echo "${cpu_list[*]}")

# Run stress-ng with the specified parameters
echo "Running stress-ng with CPU load: $cpu_load%, cores: $cpu_binding, calculated --cpu: $cpu_count"
taskset -c "$cpu_binding" stress-ng --cpu "$cpu_count" --cpu-load "$cpu_load"