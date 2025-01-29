#!/bin/bash

# Function to display usage information
usage() {
    echo "Usage: $0 -c CPU_RANGE -- PROGRAM [PROGRAM_ARGS...]"
    echo "Example:"
    echo "  $0 -c 0-11 -- ./program -i input.mp4 -f 60"
    echo "  This will run the program on CPUs 0-11"
    exit 1
}

# Function to validate CPU range format and values
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

# Function to convert CPU range to taskset format
convert_to_taskset_format() {
    local cpu_range=$1
    echo "$cpu_range" | tr ',' ','
}

# Main script logic
cpu_range=""
cmd_args=()
found_separator=0

# Parse command line arguments
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

# Convert CPU range to taskset format
taskset_cpus=$(convert_to_taskset_format "$cpu_range")

# Run the program with taskset
echo "Starting program on CPU: $cpu_range"
taskset -c "$taskset_cpus" "${cmd_args[@]}"