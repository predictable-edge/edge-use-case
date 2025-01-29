import datetime
import os
import numpy as np
import matplotlib.pyplot as plt
from ultralytics import YOLO
import argparse

def ensure_dir(directory):
    """
    Create directory if it doesn't exist.
    
    Args:
        directory (str): Path to directory
    """
    if not os.path.exists(directory):
        os.makedirs(directory)

def generate_cdf_plot(latencies, output_path):
    """
    Generate and save a Cumulative Distribution Function (CDF) plot for inference times.
    
    Args:
        latencies (numpy.ndarray): Array of latency values
        output_path (str): Path to save the output plot
    """
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
    plt.title('Inference Time CDF', fontweight='bold')
    plt.xlabel('Inference Time (ms)')
    plt.ylabel('Cumulative Probability')
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add percentile lines
    for p in [50, 99]:
        percentile_value = np.percentile(sorted_latencies, p)
        plt.axhline(p/100, color='red', linestyle='--', alpha=0.7)
        plt.axvline(percentile_value, color='red', linestyle='--', alpha=0.7)
    
    plt.tight_layout()
    plt.savefig(output_path, dpi=300)
    plt.close()

def track_and_log(
    model_path,
    source="https://youtu.be/LNwODJXcvt4",
    conf=0.3,
    iou=0.5,
    show=True,
    device='cpu',
    start_line=50,
    output_name=None,
    imgsz=1280
):
    """
    Perform object tracking using YOLO model and log inference times.
    
    Args:
        model_path (str): Path to the YOLO model file
        source (str): Source of the video stream
        conf (float): Confidence threshold
        iou (float): IoU threshold
        show (bool): Whether to show detection results
        device (str): Computation device ('cpu' or 'cuda')
        start_line (int): Starting line for CDF analysis
        output_name (str): Custom output file name (optional)
        imgsz (int): Input image size
    """
    # Create results and figures directories
    ensure_dir('results')
    ensure_dir('figures')
    
    # Generate file names
    if output_name is None:
        timestamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        base_name = timestamp
    else:
        base_name = output_name.rsplit('.', 1)[0]  # Remove extension if present
    
    results_file = f"results/latency_{base_name}.txt"
    figure_file = f"figures/latency_{base_name}.pdf"
    
    # Load YOLO model
    model = YOLO(model_path)
    
    # Store latencies for CDF plot
    latencies = []
    
    # Write header to results file
    with open(results_file, 'w', encoding='utf-8') as f:
        f.write("Frame     Inference Time\n")  # Header with fixed width
        
        # Process video frames
        results_generator = model.track(
            source=source,
            conf=conf,
            iou=iou,
            show=show,
            device=device,
            stream=True,
            imgsz=imgsz 
        )
        
        for frame_idx, result in enumerate(results_generator):
            inference_time = result.speed["inference"]
            
            # Only append latencies after start_line
            if frame_idx >= start_line:
                latencies.append(inference_time)
            
            # Write frame index and inference time with fixed width formatting
            line = f"{frame_idx:<9}{inference_time:>.2f}\n"  # Left align frame, right align time
            f.write(line)
            print(line, end='')
    
    # Generate CDF plot
    if latencies:  # Only generate plot if we have data
        generate_cdf_plot(np.array(latencies), figure_file)
        print(f"\nCDF plot saved to: {figure_file}")
    
    print(f"Results saved to: {results_file}")

def parse_args():
    """
    Parse command line arguments.
    
    Returns:
        argparse.Namespace: Parsed command line arguments
    """
    parser = argparse.ArgumentParser(
        description='YOLO Object Tracking and Latency Analysis',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    # Add command line arguments
    parser.add_argument('--output', type=str, default=None,
                      help='Output file name (default: timestamp.txt)')
    parser.add_argument('--device', type=str, choices=['cpu', 'gpu'], default='cpu',
                      help='Computation device to use')
    parser.add_argument('--start-line', type=int, default=50,
                      help='Starting line for CDF analysis')
    parser.add_argument('--imgsz', type=int, default=1280,
                      help='Input image size')
    parser.add_argument('--model-type', type=str, 
                      choices=['n', 's', 'm', 'l', 'x'],
                      default='x',
                      help='YOLOv8 model type (n:nano, s:small, m:medium, l:large, x:xlarge)')
    
    return parser.parse_args()

def main():
    """
    Main function to execute the tracking and analysis process with command line arguments.
    """
    # Parse command line arguments
    args = parse_args()
    
    # Convert 'gpu' to 'cuda' for YOLO compatibility
    device = 'cuda' if args.device == 'gpu' else 'cpu'
    
    # Create model path based on model type
    model_path = f"yolov8{args.model_type}.pt"
    
    # Execute tracking and logging
    track_and_log(
        model_path=model_path,
        source="https://youtu.be/LNwODJXcvt4",
        conf=0.3,
        iou=0.5,
        show=True,
        device=device,
        start_line=args.start_line,
        output_name=args.output,
        imgsz=args.imgsz 
    )

if __name__ == "__main__":
    main()