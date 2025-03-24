import numpy as np
import zmq
from ultralytics import YOLO
import cv2
import argparse
import time
import os
from datetime import datetime

def ensure_dir(directory):
    """Create directory if it doesn't exist."""
    if not os.path.exists(directory):
        os.makedirs(directory)

def process_frames_with_yolo(
    model_path,
    zmq_server="tcp://localhost:5555",
    conf=0.3,
    show=False,
    device='cuda',
    save_results=True
):
    """
    Receive frames from ZeroMQ server and process with YOLO model
    
    Args:
        model_path (str): Path to YOLO model
        zmq_server (str): ZeroMQ server address
        conf (float): Confidence threshold for YOLO
        show (bool): Whether to display detection results
        device (str): Device to run YOLO on ('cpu' or 'cuda')
        save_results (bool): Whether to save detection results
    """
    # Setup ZeroMQ subscriber
    context = zmq.Context()
    socket = context.socket(zmq.SUB)
    socket.connect(zmq_server)
    socket.setsockopt_string(zmq.SUBSCRIBE, "")  # Subscribe to all messages
    
    print(f"Connected to ZeroMQ server at {zmq_server}")
    print(f"Loading YOLO model {model_path}...")
    
    # Load YOLO model
    model = YOLO(model_path)
    
    # Warmup inference with a dummy frame to initialize GPU and model
    print("Performing warmup inference...")
    dummy_frame = np.zeros((640, 640, 3), dtype=np.uint8)
    _ = model.predict(dummy_frame, conf=conf, verbose=False)
    print("Warmup complete, ready for real frames")
    
    # Create results directory if saving
    if save_results:
        ensure_dir('results')
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        results_file = f"results/zmq_yolo_{timestamp}.txt"
        
        with open(results_file, 'w') as f:
            f.write(f"{'Frame':<10}{'Inference_Time_ms':>20}{'Objects_Detected':>20}\n")
    
    # Process frames
    frame_count = 0
    total_latency = 0
    try:
        print("Waiting for frames...")
        while True:
            pass
            # Receive frame metadata
            metadata = socket.recv_string()
            width, height, frame_num = map(int, metadata.split(','))
            
            # Receive frame data
            frame_data = socket.recv()
            
            # Convert to numpy array
            frame_array = np.frombuffer(frame_data, dtype=np.uint8).reshape(height, width, 3)
            
            # Run YOLO on frame
            start_time = time.time()
            results = model.predict(frame_array, conf=conf, verbose=False)
            inference_time = (time.time() - start_time) * 1000  # Convert to ms
            
            # Get detection count
            boxes = results[0].boxes
            num_detections = len(boxes)
            
            # Log results
            if frame_count % 10 == 0:
                print(f"Frame {frame_num}: {inference_time:.2f}ms, {num_detections} objects detected")
            
            # Save results if enabled
            if save_results:
                with open(results_file, 'a') as f:
                    formatted_line = f"{frame_num:<10}{inference_time:>20.2f}{num_detections:>20}\n"
                    f.write(formatted_line)
            
            # Display if enabled
            if show:
                # Draw bounding boxes
                annotated_frame = results[0].plot()
                
                # Add text with frame info
                cv2.putText(
                    annotated_frame, 
                    f"Frame: {frame_num} | Inf: {inference_time:.1f}ms | Objects: {num_detections}", 
                    (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2
                )
                
                # Show the frame
                cv2.imshow("ZMQ YOLO Detections", annotated_frame)
                
                # Break loop on 'q' key
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
            
            frame_count += 1
            total_latency += inference_time
    
    except KeyboardInterrupt:
        print("\nStopping frame processing...")
    finally:
        if show:
            cv2.destroyAllWindows()
        
        # Print summary
        if frame_count > 0:
            avg_latency = total_latency / frame_count
            print(f"\nProcessed {frame_count} frames")
            print(f"Average inference time: {avg_latency:.2f}ms")
            if save_results:
                print(f"Results saved to {results_file}")

def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description='ZeroMQ YOLO Client for Real-time Object Detection',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    parser.add_argument('--model', type=str, default='yolov8n.pt',
                      help='Path to YOLO model')
    parser.add_argument('--server', type=str, default='tcp://localhost:5555',
                      help='ZeroMQ server address')
    parser.add_argument('--conf', type=float, default=0.3,
                      help='Confidence threshold')
    parser.add_argument('--show', action='store_true',
                      help='Enable visualization')
    parser.add_argument('--device', type=str, choices=['cpu', 'cuda'], default='cuda',
                      help='Device to run YOLO on')
    parser.add_argument('--no-save', action='store_false', dest='save',
                      help='Disable saving results')
    
    return parser.parse_args()

def main():
    """Main function."""
    args = parse_args()
    
    process_frames_with_yolo(
        model_path=args.model,
        zmq_server=args.server,
        conf=args.conf,
        show=args.show,
        device=args.device,
        save_results=args.save
    )

if __name__ == "__main__":
    main() 