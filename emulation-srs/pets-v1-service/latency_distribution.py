import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from scipy import stats
import argparse
import os

def analyze_latency_data(file_path, output_name):
    # Read data file
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    # Find the line where data begins
    start_line = 0
    for i, line in enumerate(lines):
        if "Request ID" in line and "First Packet Latency" in line:
            start_line = i + 1
            break
    
    # Extract data
    request_ids = []
    first_packet_latencies = []
    total_latencies = []
    
    for line in lines[start_line:]:
        if line.strip() == "":
            continue
        parts = line.split()
        if len(parts) >= 3 and parts[0].isdigit():
            request_id = int(parts[0])
            first_latency = float(parts[1])
            total_latency = float(parts[2])
            
            request_ids.append(request_id)
            first_packet_latencies.append(first_latency)
            total_latencies.append(total_latency)
    
    # Create DataFrame
    df = pd.DataFrame({
        'Request ID': request_ids,
        'First Packet Latency': first_packet_latencies,
        'Total Latency': total_latencies
    })
    
    # Calculate difference: Total Latency - First Packet Latency
    df['Latency Difference'] = df['Total Latency'] - df['First Packet Latency']
    
    # Calculate statistics
    first_mean = np.mean(df['First Packet Latency'])
    first_var = np.var(df['First Packet Latency'])
    first_std = np.std(df['First Packet Latency'])
    first_median = np.median(df['First Packet Latency'])
    first_p99 = np.percentile(df['First Packet Latency'], 99)
    
    diff_mean = np.mean(df['Latency Difference'])
    diff_var = np.var(df['Latency Difference'])
    diff_std = np.std(df['Latency Difference'])
    diff_median = np.median(df['Latency Difference'])
    diff_p99 = np.percentile(df['Latency Difference'], 99)
    
    # Print statistics
    print(f"First Packet Latency Statistics:")
    print(f"  Mean: {first_mean:.2f} ms")
    print(f"  Median: {first_median:.2f} ms")
    print(f"  P99: {first_p99:.2f} ms")
    print(f"  Variance: {first_var:.2f} ms²")
    print(f"  Standard Deviation: {first_std:.2f} ms")
    print()
    
    print(f"Total - First Packet Latency Statistics:")
    print(f"  Mean: {diff_mean:.2f} ms")
    print(f"  Median: {diff_median:.2f} ms")
    print(f"  P99: {diff_p99:.2f} ms")
    print(f"  Variance: {diff_var:.2f} ms²")
    print(f"  Standard Deviation: {diff_std:.2f} ms")
    
    # Create figures directory if it doesn't exist
    os.makedirs('figure', exist_ok=True)
    
    # Increase figure size for better visibility
    plt.figure(figsize=(16, 10))
    
    # Set larger font sizes
    plt.rcParams.update({
        'font.size': 18,
        'axes.titlesize': 24,
        'axes.labelsize': 22,
        'xtick.labelsize': 20,
        'ytick.labelsize': 20,
        'legend.fontsize': 20,
    })
    
    # Create range for the plot
    x_range = np.linspace(0, max(max(df['First Packet Latency']), max(df['Latency Difference'])) + 5, 1000)
    
    # Calculate KDE (Probability Density Function)
    first_kde = stats.gaussian_kde(df['First Packet Latency'])
    diff_kde = stats.gaussian_kde(df['Latency Difference'])
    
    # Plot PDFs with thicker lines for better visibility
    plt.plot(x_range, first_kde(x_range), label=f'First Packet Latency (Mean={first_mean:.2f}, Var={first_var:.2f})', 
             color='blue', linewidth=4)
    plt.plot(x_range, diff_kde(x_range), label=f'Total - First Packet Latency (Mean={diff_mean:.2f}, Var={diff_var:.2f})', 
             color='red', linewidth=4)
    
    # Set larger labels
    plt.xlabel('Latency (ms)', fontweight='bold')
    plt.ylabel('Probability Density', fontweight='bold')
    plt.title('Latency Distribution Comparison (PDF)', fontweight='bold')
    
    # Add more prominent grid
    plt.grid(True, alpha=0.4, linestyle='--', linewidth=1.5)
    
    # Increase legend size and move to a better position
    plt.legend(loc='upper right', frameon=True, framealpha=0.9)
    
    # Adjust layout for better spacing with larger text
    plt.tight_layout()
    
    # Save figure with specified name in the figures directory
    plt.savefig(f'figure/{output_name}', dpi=300, bbox_inches='tight')
    plt.show()
    
    return df

# Parse command line arguments
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Analyze latency data from a file and generate distribution plots.')
    parser.add_argument('file_path', type=str, help='Path to the latency data file')
    parser.add_argument('--output', type=str, default='latency_distribution.pdf', 
                        help='Output filename for the plot (will be saved in figure/ directory)')
    
    args = parser.parse_args()
    
    df = analyze_latency_data(args.file_path, args.output)