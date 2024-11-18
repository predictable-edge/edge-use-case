import os
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
import argparse
from collections import defaultdict

def read_latency_file(file_path):
    """
    Read a single latency data file
    
    Args:
        file_path (str): Path to the data file
        
    Returns:
        pandas.Series: Latency data, or None if processing fails
    """
    try:
        with open(file_path, 'r') as f:
            lines = f.readlines()
        
        data = []
        for line in lines[1:]:  # Skip header
            parts = line.strip().split()
            if len(parts) >= 2:
                latency = float(parts[1])
                data.append(latency)
        
        return pd.Series(data)
    except Exception as e:
        print(f"Error processing file {file_path}: {str(e)}")
        return None

def process_all_folders(root_folder, name_mapping=None, skip_rows=650):
    """
    Process data from all subfolders with custom ordering
    """
    # Define default mapping if none provided
    # if name_mapping is None:
    #     name_mapping = {
    #         'E2E_wo_WC': 'E2E w/o wireless path',
    #         'E2E': 'E2E w/ wireless path',
    #         'E2E_w_Contention': 'E2E w/ wireless contention',
    #         'E2E_w_all_Contention': 'E2E w/ wireless and\ncompute contention'
    #     }
    if name_mapping is None:
        name_mapping = {
            'E2E-wo-wireless': 'w/o wireless',
            'E2E-wo-wireless-w-computing': 'w/o wireless w/ compute contention',
            'E2E-w-wireless': 'w/ wireless path',
            'E2E-w-wireless-dl-contention': 'w/ wireless contention',
            'E2E-w-wireless-computing': 'w/ wireless path  and compute contention',
            'E2E-w-all-contention': 'w/ wireless and compute contention'
        }
    
    # Define order for display
    display_order = list(name_mapping.values())
    
    grouped_data = defaultdict(list)
    
    print(f"Scanning directory: {root_folder}")
    for folder in sorted(os.listdir(root_folder)):
        folder_path = os.path.join(root_folder, folder)
        print(folder_path)
        if os.path.isdir(folder_path):
            display_name = name_mapping.get(folder, folder)
            
            log_file = os.path.join(folder_path, 'frame-1.log')
            if os.path.isfile(log_file):
                print(f"Processing {log_file}")
                latency_data = read_latency_file(log_file)
                if latency_data is not None and not latency_data.empty:
                    if len(latency_data) > skip_rows:
                        latency_data = latency_data.iloc[skip_rows:]
                    grouped_data[display_name].extend(latency_data.tolist())
                    print(f"Successfully processed {log_file}")
    
    # Create ordered dictionary based on display_order
    final_data = {}
    for name in display_order:
        if name in grouped_data and grouped_data[name]:
            final_data[name] = pd.Series(grouped_data[name])
    
    return final_data

def create_boxplot(data_dict, output_file='latency_boxplot.pdf'):
    """
    Create an enhanced boxplot visualization with legend inside the plot
    """
    # Set the style for better visualization
    plt.style.use('seaborn-v0_8-whitegrid')
    
    # Create figure with adjusted size
    plt.figure(figsize=(13, 10))
    
    # Create DataFrame for plotting
    df_plot = pd.DataFrame(data_dict)
    
    # Custom color palette - added fourth color
    base_colors = ["#2ecc71", "#3498db", "#e74c3c", "#9b59b6", "#f1c40f", "#6abc9d"]
    colors = base_colors[:len(data_dict)]
    
    # Create boxplot with enhanced styling
    box_plot = sns.boxplot(data=df_plot,
                          width=0.5,
                          palette=colors,
                          linewidth=6,
                          whis=[0, 100],
                          medianprops={"color": "black", 
                                     "linewidth": 3},
                          boxprops={"alpha": 0.8,
                                  "linewidth": 5},
                          whiskerprops={"linewidth": 5},
                          capprops={"linewidth": 5},
                          flierprops={"markersize": 8})
    
    # Remove x-axis completely
    ax = plt.gca()
    ax.xaxis.set_visible(False)  # This removes the x-axis completely
    
    # Create custom legend
    legend_handles = [plt.Rectangle((0,0),1,1, facecolor=color, alpha=0.8) 
                     for color in colors[:len(data_dict)]]
    plt.legend(legend_handles, 
              data_dict.keys(),
              fontsize=36,
              loc='upper left',  # Changed to upper right inside the plot
              bbox_to_anchor=(-0.02, 0.99))  # Adjusted to keep some padding from the edges
    
    plt.ylabel('E2E Latency (ms)', fontsize=45, labelpad=5, fontweight='bold')
    
    # Format axis with thicker lines
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, p: f'{int(x)}'))
    ax.spines['left'].set_linewidth(3)
    ax.spines['left'].set_color('black')
    ax.spines['bottom'].set_linewidth(3)
    ax.spines['bottom'].set_color('black')
    ax.tick_params(width=2)
    
    # Enhance grid with thicker lines
    plt.grid(True, axis='y', linestyle='--', alpha=0.7, linewidth=1.5)
    
    # Format labels
    ax.tick_params(axis='y', 
                  which='major',
                  width=3,
                  length=10,
                  labelsize=45,
                  colors='black',
                  direction='in',
                  right=False,
                  left=True,
                  labelright=False,
                  zorder=4)
    
    # Set y-axis limits with padding
    ymin = df_plot.min().min()
    ymax = df_plot.max().max()
    y_padding = (ymax - ymin) * 0.12
    plt.ylim(ymin - y_padding, ymax + y_padding)
    
    # Add subtle background color
    plt.gca().set_facecolor('#f8f9fa')
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    
    # Adjust layout
    plt.tight_layout()
    
    # Create output directory if needed
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    # Save plot with high quality
    plt.savefig(output_file, 
                dpi=300, 
                bbox_inches='tight', 
                format='pdf',
                transparent = True,
                edgecolor='none')
    plt.close()

def main():
    parser = argparse.ArgumentParser(
        description='Process latency data and create boxplot visualization'
    )
    parser.add_argument(
        '-i', '--input',
        required=True,
        help='Input directory containing data folders'
    )
    parser.add_argument(
        '-o', '--output',
        default='latency_boxplot.pdf',
        help='Output path for the boxplot image (default: latency_boxplot.pdf)'
    )
    parser.add_argument(
        '-m', '--mapping',
        help='Path to JSON file containing folder name to display name mapping'
    )
    parser.add_argument(
        '-s', '--skip',
        type=int,
        default=1100,
        help='Number of initial rows to skip (default: 650)'
    )
    
    args = parser.parse_args()
    
    if not os.path.isdir(args.input):
        print(f"Error: Input directory '{args.input}' does not exist")
        return
    
    # Load custom mapping if provided
    name_mapping = None
    if args.mapping:
        try:
            import json
            with open(args.mapping, 'r') as f:
                name_mapping = json.load(f)
            print("Loaded name mapping successfully")
        except Exception as e:
            print(f"Error loading mapping file: {e}")
            return
    
    data_dict = process_all_folders(args.input, name_mapping, args.skip)
    
    if data_dict:
        create_boxplot(data_dict, args.output)
        print(f"Boxplot saved as: {args.output}")
        
        # Print statistics for each group
        for name, data in data_dict.items():
            print(f"\nStatistics for {name}:")
            print(f"Median: {data.median():.2f} ms")
            print(f"25th percentile: {data.quantile(0.25):.2f} ms")
            print(f"75th percentile: {data.quantile(0.75):.2f} ms")
            print(f"Minimum: {data.min():.2f} ms")
            print(f"Maximum: {data.max():.2f} ms")
            print(f"Number of samples: {len(data)}")
    else:
        print("No data files found or error occurred during processing")

if __name__ == "__main__":
    main()