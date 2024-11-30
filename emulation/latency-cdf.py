import argparse
import os
import numpy as np
import matplotlib.pyplot as plt
from datetime import datetime

def parse_latency_file(file_path, start_line=0):
    """
    Parse the latency file and extract latency values starting from specified line.
    
    Args:
        file_path (str): Path to the input latency file
        start_line (int): Line number to start parsing from (0-based index)
    
    Returns:
        numpy.ndarray: Array of latency values
    """
    latencies = []
    with open(file_path, 'r') as f:
        # Skip lines before start_line
        for _ in range(start_line):
            next(f, None)
        
        # Process remaining lines
        for line in f:
            try:
                # Extract latency value (assuming format is "Label Latency X ms")
                latency = float(line.split()[-2])
                latencies.append(latency)
            except (IndexError, ValueError):
                # Skip lines that can't be parsed
                continue
    return np.array(latencies)

def generate_cdf_plot(latencies, output_path, start_line):
    """
    Generate and save a Cumulative Distribution Function (CDF) plot.
    
    Args:
        latencies (numpy.ndarray): Array of latency values
        output_path (str): Path to save the output plot
        start_line (int): Starting line number used for data
    """
    # Set up the plot with a clean, professional look
    plt.figure(figsize=(10, 6))
    plt.rcParams.update({
        'font.size': 20,
        'axes.labelsize': 22,
        'axes.titlesize': 24,
        'xtick.labelsize': 20,
        'ytick.labelsize': 20
    })
    
    # Sort latencies and calculate CDF
    sorted_latencies = np.sort(latencies)
    cdf = np.arange(1, len(sorted_latencies) + 1) / len(sorted_latencies)
    
    # Plot CDF with improved aesthetics
    plt.plot(sorted_latencies, cdf, color='#1E90FF', linewidth=2.5)
    plt.fill_between(sorted_latencies, cdf, alpha=0.3, color='#87CEFA')
    
    # Customize plot
    plt.title(f'Latency CDF', fontweight='bold')
    plt.xlabel('Latency (ms)')
    plt.ylabel('Cumulative Probability')
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add percentile lines
    percentile_values = []
    for p in [50, 99]:
        percentile_value = np.percentile(sorted_latencies, p)
        percentile_values.append(percentile_value)
        plt.axhline(p/100, color='red', linestyle='--', alpha=0.7)
        plt.axvline(percentile_value, color='red', linestyle='--', alpha=0.7)
   
    p50_value, p99_value = percentile_values

    plt.text(p50_value * 0.95, 0.5 * 0.95,
            f'P50: {p50_value:.2f} ms',
            verticalalignment='top',
            horizontalalignment='right')
   
    plt.text(p99_value * 1.02, 0.99 * 0.9,
            f'P99: {p99_value:.2f} ms',
            verticalalignment='bottom',
            horizontalalignment='right')
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    # Save plot
    plt.tight_layout()
    plt.savefig(output_path, dpi=300)
    plt.close()

def find_top_percentile_data(latencies, percentile=1):
    """
    Find data points in the top percentile of latencies.
    
    Args:
        latencies (numpy.ndarray): Array of latency values
        percentile (float): Percentile threshold
    
    Returns:
        tuple: (indices of top percentile latencies, top percentile latency values)
    """
    threshold = np.percentile(latencies, 100 - percentile)
    top_percentile_mask = latencies >= threshold
    top_percentile_indices = np.where(top_percentile_mask)[0]
    top_percentile_values = latencies[top_percentile_mask]
    
    return top_percentile_indices, top_percentile_values

def main():
    # Set up argument parsing
    parser = argparse.ArgumentParser(description='Latency CDF Analysis')
    parser.add_argument('input_path', type=str, help='Input latency file path')
    parser.add_argument('--start-line', type=int, default=0, 
                      help='Line number to start analysis from (0-based index)')
    args = parser.parse_args()
    
    # Read latencies starting from specified line
    latencies = parse_latency_file(args.input_path, args.start_line)
    
    # Generate output paths
    # Replace 'result' with 'figure' and 'latency.txt' with 'latency.pdf'
    figure_path = args.input_path.replace('result', 'figure').replace('latency.txt', 'latency.pdf')
    
    # Generate CDF plot
    generate_cdf_plot(latencies, figure_path, args.start_line)
    
    # Find top 1% latency values and their indices
    top_percentile_indices, top_percentile_values = find_top_percentile_data(latencies)
    
    print(f"\nAnalyzing data starting from line {args.start_line}")
    print(f"Total data points analyzed: {len(latencies)}")
    print(f"\nTop 1% Latency Values (above {np.percentile(latencies, 99):.2f} ms):")
    print("Index\tLatency (ms)")
    prev_idx = None
    for idx, value in zip(top_percentile_indices, top_percentile_values):
        actual_idx = idx + args.start_line
        if prev_idx is not None and actual_idx > prev_idx + 1:
            print()  # Add empty line for non-consecutive indices
        print(f"{actual_idx}\t{value:.2f}")
        prev_idx = actual_idx
    
    print(f"\nCDF Plot saved to: {figure_path}")

if __name__ == '__main__':
    main()