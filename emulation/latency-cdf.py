import argparse
import os
import numpy as np
import matplotlib.pyplot as plt
from datetime import datetime

def parse_latency_file(file_path):
    """
    Parse the latency file and extract latency values.
    
    Args:
        file_path (str): Path to the input latency file
    
    Returns:
        numpy.ndarray: Array of latency values
    """
    latencies = []
    with open(file_path, 'r') as f:
        for line in f:
            try:
                # Extract latency value (assuming format is "Label Latency X ms")
                latency = float(line.split()[-2])
                latencies.append(latency)
            except (IndexError, ValueError):
                # Skip lines that can't be parsed
                continue
    return np.array(latencies)

def generate_cdf_plot(latencies, output_path):
    """
    Generate and save a Cumulative Distribution Function (CDF) plot.
    
    Args:
        latencies (numpy.ndarray): Array of latency values
        output_path (str): Path to save the output plot
    """
    # Set up the plot with a clean, professional look
    plt.figure(figsize=(10, 6))
    plt.rcParams.update({
        'font.size': 10,
        'axes.labelsize': 12,
        'axes.titlesize': 14,
        'xtick.labelsize': 10,
        'ytick.labelsize': 10
    })
    
    # Sort latencies and calculate CDF
    sorted_latencies = np.sort(latencies)
    cdf = np.arange(1, len(sorted_latencies) + 1) / len(sorted_latencies)
    
    # Plot CDF with improved aesthetics
    plt.plot(sorted_latencies, cdf, color='#1E90FF', linewidth=2.5)
    plt.fill_between(sorted_latencies, cdf, alpha=0.3, color='#87CEFA')
    
    # Customize plot
    plt.title('Latency Cumulative Distribution Function', fontweight='bold')
    plt.xlabel('Latency (ms)')
    plt.ylabel('Cumulative Probability')
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add percentile lines
    percentiles = [50, 90, 95, 99]
    for p in percentiles:
        percentile_value = np.percentile(sorted_latencies, p)
        plt.axhline(p/100, color='red', linestyle='--', alpha=0.7)
        plt.axvline(percentile_value, color='red', linestyle='--', alpha=0.7)
        plt.text(percentile_value, 0.1, f'{p}th: {percentile_value:.2f} ms', 
                 verticalalignment='bottom', horizontalalignment='right')
    
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
        numpy.ndarray: Latency values above the specified percentile
    """
    threshold = np.percentile(latencies, 100 - percentile)
    return latencies[latencies >= threshold]

def main():
    # Set up argument parsing
    parser = argparse.ArgumentParser(description='Latency CDF Analysis')
    parser.add_argument('input_path', type=str, help='Input latency file path')
    args = parser.parse_args()
    
    # Read latencies
    latencies = parse_latency_file(args.input_path)
    
    # Generate output paths
    # Replace 'result' with 'figure' and 'latency.txt' with 'latency.pdf'
    figure_path = args.input_path.replace('result', 'figure').replace('latency.txt', 'latency.pdf')
    
    # Generate CDF plot
    generate_cdf_plot(latencies, figure_path)
    
    # Find and print top 1% latency values
    top_percentile = find_top_percentile_data(latencies)
    print(f"\nTop 1% Latency Value (above {np.percentile(latencies, 99):.2f} ms):")
    print(f"Largest value: {top_percentile[-1]:.2f} ms")
    
    print(f"\nCDF Plot saved to: {figure_path}")

if __name__ == '__main__':
    main()