import argparse
import os
import numpy as np
import matplotlib.pyplot as plt
from datetime import datetime

def parse_latency_file(file_path, start_line=0):
    """
    Parse the latency file and extract latency values starting from specified line.
    Format:
    Request ID        Latency (ms)
    1                 38.95
    2                 38.80
    ...
    
    Args:
        file_path (str): Path to the input latency file
        start_line (int): Line number to start parsing from (0-based index)
    
    Returns:
        numpy.ndarray: Array of latency values
    """
    latencies = []
    with open(file_path, 'r') as f:
        # Skip header line
        next(f)
        
        # Skip lines before start_line
        for _ in range(start_line):
            next(f, None)
        
        # Process remaining lines
        for line in f:
            try:
                # Split line by whitespace and take the second value
                parts = line.split()
                if len(parts) >= 2:
                    latency = float(parts[1])
                    latencies.append(latency)
            except (IndexError, ValueError) as e:
                print(f"Warning: Could not parse line: {line.strip()}")
                continue
    
    return np.array(latencies)

def generate_multi_cdf_plot(file_data, output_path, start_line, ddl=None):
    """
    Generate and save a Cumulative Distribution Function (CDF) plot for multiple files.
    
    Args:
        file_data: List of tuples (file_path, legend_name)
        output_path (str): Path to save the output plot
        start_line (int): Starting line number used for data
        ddl (float): Deadline to mark with vertical line
    """
    # Set up the plot with a clean, professional look
    plt.figure(figsize=(10, 6))
    plt.rcParams.update({
        'font.size': 22,
        'axes.labelsize': 22,
        'axes.titlesize': 24,
        'xtick.labelsize': 25,
        'ytick.labelsize': 24
    })
    
    colors = [
        '#1E90FF',  # 道奇蓝
        '#FF6B6B',  # 浅珊瑚红
        '#4CAF50',  # 绿色
        '#FFA500',  # 橙色
        '#9370DB',  # 中紫色
        '#20B2AA',  # 浅海绿
        '#FF69B4',  # 热粉红
        '#FFD700',  # 金色
        '#8A2BE2',  # 蓝紫色
        '#00CED1'   # 深青色
    ]
    
    # Plot each file's CDF
    for idx, (file_path, legend_name) in enumerate(file_data):
        # Read and process data
        latencies = parse_latency_file(file_path, start_line)
        sorted_latencies = np.sort(latencies)
        cdf = np.arange(1, len(sorted_latencies) + 1) / len(sorted_latencies)
        
        # Plot CDF
        color = colors[idx % len(colors)]
        plt.plot(sorted_latencies, cdf, color=color, linewidth=2.5, label=legend_name)
        
        # Calculate and mark percentiles
        for p in [50, 99]:
            percentile_value = np.percentile(sorted_latencies, p)
            # 找到最接近百分位值的点的索引
            idx_percentile = np.abs(sorted_latencies - percentile_value).argmin()
            y_pos = cdf[idx_percentile]
            
            # 添加标记点
            plt.plot(percentile_value, y_pos, 'o', color=color, markersize=8)
            
            # 添加标注文本
            # if p == 50:
            #     plt.text(percentile_value, y_pos,
            #             f' P50={percentile_value:.1f}',
            #             verticalalignment='bottom',
            #             horizontalalignment='left',
            #             color=color)
            # else:
            #     plt.text(percentile_value, y_pos,
            #             f' P99={percentile_value:.1f}',
            #             verticalalignment='bottom',
            #             horizontalalignment='left',
            #             color=color)
    
    # Add deadline line if specified
    if ddl is not None:
        plt.axvline(x=ddl, color='red', linestyle='--', linewidth=3.0)
        # Add 'SLA' annotation next to the line
        plt.text(ddl + 1, 0.5, 'SLA', 
                rotation=0,
                verticalalignment='center',
                color='red',
                fontsize=22)
    
    # Customize plot
    plt.title(f'Latency CDF of UE1', fontweight='bold')
    plt.xlabel('UL Transmission Latency (ms)')
    plt.ylabel('CDF')
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend(loc='lower right')
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    # Save plot
    plt.tight_layout()
    plt.savefig(output_path, dpi=300)
    plt.close()

def main():
    # Set up argument parsing
    parser = argparse.ArgumentParser(description='Multi-file Latency CDF Analysis')
    parser.add_argument('inputs', nargs='+', help='Alternating input file paths and legend names')
    parser.add_argument('--start-line', type=int, default=0, 
                      help='Line number to start analysis from (0-based index)')
    parser.add_argument('--ddl', type=float, 
                      help='Deadline to mark with vertical line (in ms)')
    args = parser.parse_args()
    
    # Parse input arguments into pairs of (file_path, legend_name)
    if len(args.inputs) % 2 != 0:
        raise ValueError("Must provide pairs of file paths and legend names")
    
    file_data = [(args.inputs[i], args.inputs[i+1]) 
                 for i in range(0, len(args.inputs), 2)]
    
    # Generate output path from legend names
    output_name = "-".join(name for _, name in file_data) + ".pdf"
    figure_path = os.path.join(".", "figure", output_name)
    
    # Generate CDF plot with deadline line
    generate_multi_cdf_plot(file_data, figure_path, args.start_line, args.ddl)
    
    print(f"\nAnalyzing data starting from line {args.start_line}")
    for file_path, legend_name in file_data:
        latencies = parse_latency_file(file_path, args.start_line)
        print(f"\nFile: {file_path} ({legend_name})")
        print(f"Total data points: {len(latencies)}")
        print(f"P50: {np.percentile(latencies, 50):.2f} ms")
        print(f"P99: {np.percentile(latencies, 99):.2f} ms")
    
    print(f"\nCDF Plot saved to: {figure_path}")

if __name__ == '__main__':
    main()