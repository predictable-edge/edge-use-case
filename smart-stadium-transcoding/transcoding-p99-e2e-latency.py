import os
import re
import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
from datetime import datetime

def parse_timestamp_folder(task_path):
    """
    Find the latest timestamp folder in the task directory
    Args:
        task_path: Path to the task directory
    Returns:
        Latest timestamp folder path or None if not found
    """
    try:
        # List all timestamp folders
        folders = [f for f in os.listdir(task_path) if os.path.isdir(os.path.join(task_path, f))]
        # Filter folders with timestamp format (YYYYMMDD-HHMMSSXXX)
        timestamp_folders = [f for f in folders if re.match(r'\d{8}-\d{9}', f)]
        
        if not timestamp_folders:
            return None
            
        # Convert to datetime objects for comparison
        dated_folders = []
        for folder in timestamp_folders:
            try:
                timestamp = datetime.strptime(folder, '%Y%m%d-%H%M%S%f')
                dated_folders.append((timestamp, folder))
            except ValueError:
                continue
                
        if not dated_folders:
            return None
            
        # Get the latest timestamp folder
        latest_folder = max(dated_folders, key=lambda x: x[0])[1]
        return os.path.join(task_path, latest_folder)
    except Exception as e:
        print(f"Error processing task path {task_path}: {e}")
        return None

def read_latency_file(file_path):
    """
    Read and parse the latency log file
    Args:
        file_path: Path to the frame-1.log file
    Returns:
        List of latency values or None if file can't be read
    """
    try:
        latencies = []
        with open(file_path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                # Skip header line
                if line.startswith('Frame'):
                    continue
                    
                # Extract latency value
                match = re.search(r'(\d+)\s+ms', line)
                if match:
                    latencies.append(int(match.group(1)))
                    
        return latencies
    except Exception as e:
        print(f"Error reading file {file_path}: {e}")
        return None

def calculate_p99(latencies):
    """
    Calculate P99 latency from the data (using data from frame 400 onwards)
    Args:
        latencies: List of latency values
    Returns:
        P99 latency value
    """
    if len(latencies) < 400:
        return None
    
    data_subset = latencies[399:]  # Start from 400th frame (index 399)
    return np.percentile(data_subset, 99)

def plot_task_latencies(base_path, output_path):
    """
    Create a line plot of P99 latencies across tasks
    Args:
        base_path: Base directory containing task folders
        output_path: Path to save the output plot
    """
    # Collect task data
    task_data = []
    
    # Find all task directories
    task_dirs = [d for d in os.listdir(base_path) if d.startswith('task') and 
                os.path.isdir(os.path.join(base_path, d))]
    
    # Extract task numbers and sort
    task_dirs = sorted(task_dirs, key=lambda x: int(re.search(r'task(\d+)', x).group(1)))
    
    # Process each task
    for task_dir in task_dirs:
        task_num = int(re.search(r'task(\d+)', task_dir).group(1))
        task_path = os.path.join(base_path, task_dir)
        
        # Get latest timestamp folder
        timestamp_folder = parse_timestamp_folder(task_path)
        if not timestamp_folder:
            print(f"No valid timestamp folder found in {task_dir}")
            continue
            
        # Read frame-1.log
        log_file = os.path.join(timestamp_folder, 'frame-1.log')
        latencies = read_latency_file(log_file)
        
        if latencies:
            p99 = calculate_p99(latencies)
            if p99 is not None:
                task_data.append((task_num, p99))
    
    if not task_data:
        print("No valid data found to plot")
        return
    
    # Sort data by task number
    task_data.sort(key=lambda x: x[0])
    
    # Create the plot
    plt.figure(figsize=(12, 8), dpi=300)
    
    # Extract x and y values
    x_values = [x[0] for x in task_data]
    y_values = [y[1] for y in task_data]
    
    # Plot settings
    plt.plot(x_values, y_values, 
             linewidth=1.2, 
             color='#1f77b4',
             marker='o',
             markersize=6,
             markerfacecolor='white',
             markeredgecolor='#1f77b4',
             markeredgewidth=1.5)
    
    # Customize the plot
    plt.grid(True, linestyle='--', alpha=0.7, color='gray', linewidth=0.5)
    plt.xlabel('Number of Tasks', fontsize=12, fontweight='bold')
    plt.ylabel('P99 E2E Latency (ms)', fontsize=12, fontweight='bold')
    plt.title('P99 E2E Latency vs Number of Tasks', fontsize=14, fontweight='bold', pad=20)
    
    # Set integer ticks for x-axis
    ax = plt.gca()
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    
    # Add points annotation
    for x, y in zip(x_values, y_values):
        plt.annotate(f'{y:.0f}', 
                    (x, y), 
                    textcoords="offset points", 
                    xytext=(0,10), 
                    ha='center',
                    fontsize=8)
    
    # Adjust layout and save
    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight', pad_inches=0.2)
    print(f"Plot saved as: {output_path}")
    plt.close()

def main():
    parser = argparse.ArgumentParser(description='Plot P99 latency across tasks.')
    parser.add_argument('base_path', type=str,
                        help='Base directory containing task folders')
    parser.add_argument('--output', '-o', type=str, required=True,
                        help='Path to save the output plot')
    
    args = parser.parse_args()
    
    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    plot_task_latencies(args.base_path, args.output)

if __name__ == "__main__":
    main()