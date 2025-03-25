import numpy as np
from ultralytics import YOLO
import cv2
import argparse
import time
import os
from datetime import datetime
import mmap
import posix_ipc
import signal
import sys
import socket
import threading

def ensure_dir(directory):
    """Create directory if it doesn't exist."""
    if not os.path.exists(directory):
        os.makedirs(directory)

def cleanup_resources(shm, sem_ready, sem_processed, shm_name, sem_ready_name, sem_processed_name):
    """Clean up shared memory and semaphores."""
    if shm:
        shm.close_fd()
    if sem_ready:
        sem_ready.close()
    if sem_processed:
        sem_processed.close()
    
    # Try to unlink shared resources
    try:
        posix_ipc.unlink_shared_memory(shm_name)
    except:
        pass
    
    try:
        posix_ipc.unlink_semaphore(sem_ready_name)
    except:
        pass
    
    try:
        posix_ipc.unlink_semaphore(sem_processed_name)
    except:
        pass

def signal_handler(sig, frame):
    """Handle Ctrl+C gracefully."""
    print("\nStopping frame processing...")
    sys.exit(0)

def setup_tcp_server(port=9001):
    """Setup TCP server for sending detection results."""
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind(('0.0.0.0', port))
    server_socket.listen(1)
    print(f"TCP server listening on port {port}")
    
    # Accept client connection
    client_socket, client_address = server_socket.accept()
    print(f"Client connected from {client_address}")
    
    return server_socket, client_socket

def send_detection_results(client_socket, frame_num, _):
    """Send frame number over TCP in a simple format."""
    if client_socket is None:
        return
    
    # Send simple frame number message
    message = f"FRAME:{frame_num}\n"
    try:
        client_socket.sendall(message.encode('utf-8'))
    except Exception as e:
        print(f"Error sending detection results: {e}")

def process_frames_with_yolo(
    model_path,
    conf=0.3,
    show=False,
    device='cuda',
    save_results=True,
    tcp_port=9001
):
    """
    Process frames from shared memory with YOLO model
    
    Args:
        model_path (str): Path to YOLO model
        conf (float): Confidence threshold for YOLO
        show (bool): Whether to display detection results
        device (str): Device to run YOLO on ('cpu' or 'cuda')
        save_results (bool): Whether to save detection results
        tcp_port (int): Port to use for TCP communication
    """
    # Setup signal handler for graceful shutdown
    signal.signal(signal.SIGINT, signal_handler)
    
    # Setup TCP server for sending detection results
    server_socket = None
    client_socket = None
    try:
        server_socket, client_socket = setup_tcp_server(tcp_port)
    except Exception as e:
        print(f"Failed to setup TCP server: {e}")
        print("Continuing without TCP communication...")
    
    # Shared memory parameters
    SHM_NAME = "/yolo_frame_buffer"
    SEM_READY_NAME = "/frame_ready"
    SEM_PROCESSED_NAME = "/frame_processed"
    METADATA_SIZE = 256
    
    # Initialize shared memory and semaphore resources
    shm = None
    sem_ready = None
    sem_processed = None
    
    try:
        # Open existing shared memory
        shm = posix_ipc.SharedMemory(SHM_NAME, posix_ipc.O_CREAT | posix_ipc.O_RDWR)
        
        # Open existing semaphores
        sem_ready = posix_ipc.Semaphore(SEM_READY_NAME)
        sem_processed = posix_ipc.Semaphore(SEM_PROCESSED_NAME)
        
        # Map the shared memory to this process
        shm_map = mmap.mmap(shm.fd, 0)
        
        print(f"Connected to shared memory and semaphores")
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
            results_file = f"results/shm_yolo_{timestamp}.txt"
            
            with open(results_file, 'w') as f:
                f.write(f"{'Frame':<10}{'Inference_Time_ms':>20}{'Objects_Detected':>20}\n")
        
        # Process frames
        frame_count = 0
        total_latency = 0
        
        print("Waiting for frames...")
        while True:
            # Wait for producer to signal a new frame is ready
            sem_ready.acquire()
            
            # Read metadata from shared memory
            metadata_str = shm_map[:METADATA_SIZE].decode().strip('\0')
            width, height, frame_num = map(int, metadata_str.split(','))
            frame_size = width * height * 3

            start_time = time.time()
            # Read frame data from shared memory
            frame_data = memoryview(shm_map)[METADATA_SIZE:METADATA_SIZE + frame_size]
            
            # Convert to numpy array without copying data
            frame_array = np.frombuffer(frame_data, dtype=np.uint8).reshape(height, width, 3)
            
            # Signal that we've read the data
            sem_processed.release()
            
            # Run YOLO on frame
            results = model.predict(frame_array, conf=conf, verbose=False)
            
            # Get detection count
            boxes = results[0].boxes
            num_detections = len(boxes)
            
            # Send frame number via TCP (simplified)
            if client_socket:
                send_detection_results(client_socket, frame_num + 1, None)
            
            inference_time = (time.time() - start_time) * 1000  # Convert to ms
            timestamp_ms = int(time.monotonic() * 1000)
            print(f"Frame {frame_num} finished: {timestamp_ms}")
            
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
                cv2.imshow("Shared Memory YOLO Detections", annotated_frame)
                
                # Break loop on 'q' key
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
            
            frame_count += 1
            total_latency += inference_time
    
    except posix_ipc.ExistentialError as e:
        print(f"Shared memory or semaphore doesn't exist: {e}")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        # Clean up resources
        if show:
            cv2.destroyAllWindows()
        
        if client_socket:
            client_socket.close()
        if server_socket:
            server_socket.close()
            
        cleanup_resources(shm, sem_ready, sem_processed, SHM_NAME, SEM_READY_NAME, SEM_PROCESSED_NAME)
        
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
        description='Shared Memory YOLO Client for Real-time Object Detection',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    parser.add_argument('--model', type=str, default='yolov8n.pt',
                      help='Path to YOLO model')
    parser.add_argument('--conf', type=float, default=0.3,
                      help='Confidence threshold')
    parser.add_argument('--show', action='store_true',
                      help='Enable visualization')
    parser.add_argument('--device', type=str, choices=['cpu', 'cuda'], default='cuda',
                      help='Device to run YOLO on')
    parser.add_argument('--no-save', action='store_false', dest='save',
                      help='Disable saving results')
    parser.add_argument('--tcp-port', type=int, default=9001,
                      help='TCP port for sending detection results')
    
    return parser.parse_args()

def main():
    """Main function."""
    args = parse_args()
    
    process_frames_with_yolo(
        model_path=args.model,
        conf=args.conf,
        show=args.show,
        device=args.device,
        save_results=args.save,
        tcp_port=args.tcp_port
    )

if __name__ == "__main__":
    main()