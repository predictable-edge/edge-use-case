import argparse
import pandas as pd
import matplotlib.pyplot as plt
import os
import re
from matplotlib.ticker import MaxNLocator
from cycler import cycler

def read_data(file_path):
    """
    Reads the data file, assuming two columns: Frame and E2E latency(ms)
    Supports multiple spaces or tabs as separators and removes 'ms' from latency.
    """
    try:
        frames = []
        latencies = []
        with open(file_path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue  # Skip empty lines
                # Skip header lines
                if re.match(r'^[Ff]rame', line):
                    continue
                # Split the line by any whitespace (multiple spaces or tabs)
                parts = re.split(r'\s+', line)
                if len(parts) < 2:
                    print(f"Incorrect format in file {file_path}: '{line}'")
                    continue
                try:
                    frame = int(parts[0])
                except ValueError:
                    print(f"Non-integer Frame value in file {file_path}: '{parts[0]}'")
                    continue
                # Extract latency number and remove 'ms'
                latency_str = parts[1]
                latency_match = re.match(r'(\d+)', latency_str)
                if latency_match:
                    latency = int(latency_match.group(1))
                else:
                    print(f"Incorrect latency format in file {file_path}: '{latency_str}'")
                    continue
                frames.append(frame)
                latencies.append(latency)
        if not frames:
            print(f"No valid data found in file {file_path}.")
            return None
        return pd.DataFrame({'Frame': frames, 'E2E_Latency_ms': latencies})
    except Exception as e:
        print(f"Error reading file {file_path}: {e}")
        return None

def plot_data(file_paths, output_path):
    """
    Plots the E2E latency data from multiple files and saves the plot as a PDF.
    Enhanced version with clean lines and elegant styling.
    """
    # Set figure style
    plt.style.use('default')
    
    # Define custom colors and line styles
    colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd', '#8c564b']
    line_styles = [
        '-',      # solid
        '--',     # dashed
        '-.',     # dash-dot
        ':',      # dotted
        (0, (3, 1, 1, 1, 1, 1)),  # dash-dot-dot-dot
        (0, (5, 1))               # dense dashed
    ]
    
    plt.rc('axes', prop_cycle=(cycler('color', colors)))
    
    # Create figure and axis with higher DPI
    fig, ax = plt.subplots(figsize=(14, 8), dpi=300)
    
    # Set background color
    ax.set_facecolor('#f8f9fa')
    fig.patch.set_facecolor('white')
    
    # Get axis limits for positioning the legend
    num_files = len(file_paths)
    max_latency = 0
    min_latency = float('inf')
    
    # First pass to get data ranges
    dfs = []
    for file in file_paths:
        df = read_data(file)
        if df is not None and not df.empty:
            dfs.append(df)
            latency = df['E2E_Latency_ms'][300:]
            max_latency = max(max_latency, latency.max())
            min_latency = min(min_latency, latency.min())
    
    # Plot each file's data
    for idx, (file, df) in enumerate(zip(file_paths, dfs)):
        if df is not None and not df.empty:
            frames = df['Frame']
            latency = df['E2E_Latency_ms']
            label = os.path.splitext(os.path.basename(file))[0]  # Remove file extension
            
            # Plot with clean lines
            plt.plot(frames[300:], latency[300:], 
                    label=label,
                    linewidth=1.2,  # Thinner lines
                    linestyle=line_styles[idx % len(line_styles)],
                    alpha=0.9)
    
    # Enhance the grid
    ax.grid(True, linestyle='--', alpha=0.5, color='gray', linewidth=0.5)
    ax.set_axisbelow(True)
    
    # Set labels and title
    ax.set_xlabel('Frame Label', fontsize=12, fontweight='bold')
    ax.set_ylabel('E2E Latency (ms)', fontsize=12, fontweight='bold')
    ax.set_title('End-to-End Latency Analysis', fontsize=14, fontweight='bold', pad=20)
    
    # Customize tick labels
    ax.tick_params(axis='both', which='major', labelsize=10)
    
    # Optimize number of x-axis ticks
    ax.xaxis.set_major_locator(MaxNLocator(10))
    
    # Position legend inside the plot
    legend = ax.legend(
        bbox_to_anchor=(0.02, 0.98),
        loc='upper left',
        borderaxespad=0,
        frameon=True,
        framealpha=0.9,
        edgecolor='gray',
        fontsize=10,
        ncol=1 if num_files <= 3 else 2
    )
    
    # Add a light background to the legend
    legend.get_frame().set_facecolor('#f8f9fa')
    
    # Customize spines
    for spine in ax.spines.values():
        spine.set_color('#cccccc')
        spine.set_linewidth(0.8)
    
    # Adjust layout
    plt.tight_layout()
    
    # Save the plot
    plt.savefig(output_path, 
                format='pdf',
                dpi=300,
                bbox_inches='tight',
                pad_inches=0.2)
    print(f"Enhanced plot saved as PDF at: {output_path}")
    plt.close()

def main():
    parser = argparse.ArgumentParser(description='Plot E2E latency from multiple text files.')
    parser.add_argument('files', metavar='F', type=str, nargs='+',
                        help='Paths to the input data files.')
    parser.add_argument('--output', '-o', type=str, required=True,
                        help='Path to save the output PDF plot.')
    args = parser.parse_args()
    
    if not args.files:
        print("Please provide at least one input file path.")
        return
    
    # Ensure the output directory exists
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    plot_data(args.files, args.output)

if __name__ == "__main__":
    main()