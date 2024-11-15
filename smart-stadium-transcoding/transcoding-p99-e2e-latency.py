import os
import re
import argparse
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
from datetime import datetime
import logging

def parse_timestamp_folder(task_path):
    """
    Find the latest timestamp folder in the task directory
    """
    try:
        folders = [f for f in os.listdir(task_path) if os.path.isdir(os.path.join(task_path, f))]
        timestamp_folders = [f for f in folders if re.match(r'\d{8}-\d{9}', f)]
        
        if not timestamp_folders:
            return None
            
        dated_folders = []
        for folder in timestamp_folders:
            try:
                timestamp = datetime.strptime(folder, '%Y%m%d-%H%M%S%f')
                dated_folders.append((timestamp, folder))
            except ValueError:
                continue
                
        if not dated_folders:
            return None
            
        latest_folder = max(dated_folders, key=lambda x: x[0])[1]
        return os.path.join(task_path, latest_folder)
    except Exception as e:
        print(f"Error processing task path {task_path}: {e}")
        return None

def read_latency_file(file_path):
    """
    Read and parse the latency log file
    """
    try:
        latencies = []
        with open(file_path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('Frame'):
                    continue
                    
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
    """
    if len(latencies) < 400:
        return None
    
    data_subset = latencies[399:]  # Start from 400th frame (index 399)
    return np.percentile(data_subset, 99)

def plot_task_latencies(base_path, output_path):
    """
    Create a line plot of P99 latencies across multiple video processing tasks
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
        
    task_data.sort(key=lambda x: x[0])
    
    plt.figure(figsize=(16, 10), dpi=300, facecolor='none')
    ax = plt.gca()
    ax.set_facecolor('none')
    
    x_values = [x[0] for x in task_data]
    y_values = [y[1] for y in task_data]
    
    y_min = -max(y_values) * 0.02 
    y_max = max(y_values) * 1.10
    
    threshold = 200
    # plt.axhspan(y_min, threshold, 
    #             color='#cc0000', alpha=0.1, zorder=1)
    threshold_line = plt.axhline(y=threshold, 
                                color='#aa0000', 
                                linestyle=(0, (1, 1)),
                                # linestyle='dotted', 
                                linewidth=12, 
                                alpha=0.8, 
                                zorder=10)
    
    plt.annotate('SLA (200ms)', 
                xy=(min(x_values), threshold),
                xytext=(775, 5),
                textcoords='offset points',
                fontsize=50,
                fontweight='bold',
                color='#cc0000',
                va='bottom',
                ha='right')
    
    plt.plot(x_values, y_values, 
             linewidth=9.0,
             color='#ff7f0e',
             marker='o',
             markersize=14,
             markerfacecolor='white',
             markeredgecolor='#ff7f0e',
             markeredgewidth=3.0,
             zorder=3)
    
    plt.xlabel('Number of Processing Videos', 
              fontsize=48,
              fontweight='bold',
              labelpad=20,
              color='black')
              
    plt.ylabel('P99 Computing\nLatency (ms)',
              fontsize=48,
              fontweight='bold',
              labelpad=5,
              color='black')
    
    ax.tick_params(axis='y', 
                  which='major',
                  width=3,
                  length=10,
                  labelsize=53,
                  colors='black',
                  direction='in',
                  right=False,
                  left=True,
                  labelright=False,
                  zorder=4)
                  
    ax.tick_params(axis='x',
                  which='major',
                  width=3,
                  length=0,
                  labelsize=60,
                  colors='black')
    
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    
    for x, y in zip(x_values, y_values):
        plt.annotate(f'{y:.0f}', 
                    (x, y), 
                    textcoords="offset points", 
                    xytext=(0,15), 
                    ha='center',
                    fontsize=50,
                    fontweight='bold',
                    color='black')

    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['left'].set_color('black')
    ax.spines['bottom'].set_color('black')
    ax.spines['left'].set_linewidth(3)
    ax.spines['bottom'].set_linewidth(3)
  
    ax.yaxis.grid(True, 
                  linestyle='-', 
                  linewidth=2,
                  alpha=0.3,
                  color='black',
                  zorder=1,
                  clip_on=True)
    
    x_min = min(x_values)
    x_max = max(x_values)
    x_margin = (x_max - x_min) * 0.1
    plt.xlim(x_min - x_margin, x_max + x_margin)
    
    plt.ylim(y_min, y_max)
    
    y_ticks = ax.get_yticks()
    ax.set_yticks([tick for tick in y_ticks if tick >= 0])
    
    plt.tight_layout(pad=1.5)
    
    if output_path:
        ensure_dir_exists(output_path)
        plt.savefig(output_path, 
                   dpi=300, 
                   bbox_inches='tight',
                   facecolor='none',
                   edgecolor='none',
                   transparent=True,
                   pad_inches=0.3)
        print(f"Plot saved as: {output_path}")
    
    plt.close()
    

def ensure_dir_exists(file_path):
    directory = os.path.dirname(file_path)
    if directory and not os.path.exists(directory):
        os.makedirs(directory)

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