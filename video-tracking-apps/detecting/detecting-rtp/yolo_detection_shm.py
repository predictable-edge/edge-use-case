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
import struct  # Add struct module for binary packing
import json  # Add json for class mapping
import threading  # Add threading for monitoring thread

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

def cleanup_result_resources(result_shm, result_sem_ready, result_sem_processed, result_shm_name, result_sem_ready_name, result_sem_processed_name):
    """Clean up result shared memory and semaphores."""
    if result_shm:
        result_shm.close_fd()
    if result_sem_ready:
        result_sem_ready.close()
    if result_sem_processed:
        result_sem_processed.close()
    
    # Try to unlink shared resources
    try:
        posix_ipc.unlink_shared_memory(result_shm_name)
    except:
        pass
    
    try:
        posix_ipc.unlink_semaphore(result_sem_ready_name)
    except:
        pass
    
    try:
        posix_ipc.unlink_semaphore(result_sem_processed_name)
    except:
        pass

def signal_handler(sig, frame):
    """Handle Ctrl+C gracefully."""
    print("\nStopping frame processing...")
    sys.exit(0)

# Frame monitoring thread function
def monitor_frame_progress(frame_counter, running_flag):
    """
    Monitor if frames are still being processed and terminate if stalled
    
    Args:
        frame_counter (list): Reference to frame counter (using list for mutability)
        running_flag (list): Reference to running flag (using list for mutability)
    """
    last_frame_count = 0
    while running_flag[0]:
        time.sleep(1)  # Check every 1 second
        if frame_counter[0] > 0 and frame_counter[0] == last_frame_count:
            print("Frame processing has stalled. Terminating process.")
            os._exit(1)  # Force terminate the process
        last_frame_count = frame_counter[0]

def process_frames_with_yolo(
    model_path,
    conf=0.3,
    show=False,
    device='cuda',
    save_results=True
):
    """
    Process frames from shared memory with YOLO model
    
    Args:
        model_path (str): Path to YOLO model
        conf (float): Confidence threshold for YOLO
        show (bool): Whether to display detection results
        device (str): Device to run YOLO on ('cpu' or 'cuda')
        save_results (bool): Whether to save detection results
    """
    # Setup signal handler for graceful shutdown
    signal.signal(signal.SIGINT, signal_handler)
    
    # Shared memory parameters for input
    SHM_NAME = "/yolo_frame_buffer"
    SEM_READY_NAME = "/frame_ready"
    SEM_PROCESSED_NAME = "/frame_processed"
    METADATA_SIZE = 256
    
    # Shared memory parameters for results - now we need more space for detections
    RESULT_SHM_NAME = "/yolo_result_buffer"
    RESULT_SEM_READY_NAME = "/result_ready"
    RESULT_SEM_PROCESSED_NAME = "/result_processed"
    RESULT_SIZE = 8192  # Increase size to accommodate detection results
    
    # Initialize shared memory and semaphore resources
    shm = None
    sem_ready = None
    sem_processed = None
    
    # Initialize result shared memory and semaphore resources
    result_shm = None
    result_sem_ready = None
    result_sem_processed = None
    
    # COCO class names that YOLOv8 is trained on (for reference)
    COCO_CLASSES = [
        'person', 'bicycle', 'car', 'motorcycle', 'airplane', 'bus', 'train', 'truck', 'boat', 'traffic light',
        'fire hydrant', 'stop sign', 'parking meter', 'bench', 'bird', 'cat', 'dog', 'horse', 'sheep', 'cow',
        'elephant', 'bear', 'zebra', 'giraffe', 'backpack', 'umbrella', 'handbag', 'tie', 'suitcase', 'frisbee',
        'skis', 'snowboard', 'sports ball', 'kite', 'baseball bat', 'baseball glove', 'skateboard', 'surfboard',
        'tennis racket', 'bottle', 'wine glass', 'cup', 'fork', 'knife', 'spoon', 'bowl', 'banana', 'apple',
        'sandwich', 'orange', 'broccoli', 'carrot', 'hot dog', 'pizza', 'donut', 'cake', 'chair', 'couch',
        'potted plant', 'bed', 'dining table', 'toilet', 'tv', 'laptop', 'mouse', 'remote', 'keyboard', 'cell phone',
        'microwave', 'oven', 'toaster', 'sink', 'refrigerator', 'book', 'clock', 'vase', 'scissors', 'teddy bear',
        'hair drier', 'toothbrush'
    ]
    
    # Frame count for monitoring progress - using list for mutability
    frame_counter = [0]
    running_flag = [True]
    
    # Start monitoring thread
    monitor_thread = threading.Thread(
        target=monitor_frame_progress, 
        args=(frame_counter, running_flag),
        daemon=True
    )
    monitor_thread.start()
    print("Monitoring thread started")
    
    try:
        # Open existing shared memory for input
        shm = posix_ipc.SharedMemory(SHM_NAME, posix_ipc.O_CREAT | posix_ipc.O_RDWR)
        sem_ready = posix_ipc.Semaphore(SEM_READY_NAME)
        sem_processed = posix_ipc.Semaphore(SEM_PROCESSED_NAME)
        shm_map = mmap.mmap(shm.fd, 0)
        
        print(f"Connected to input shared memory and semaphores")
        
        # Connect to the result shared memory (should be created by detecting_tcp)
        max_retry = 30  # Maximum number of retries (5 seconds total with 0.2s sleep)
        retry_count = 0
        
        while retry_count < max_retry:
            try:
                # Try to open existing shared memory for results
                result_shm = posix_ipc.SharedMemory(RESULT_SHM_NAME)
                result_sem_ready = posix_ipc.Semaphore(RESULT_SEM_READY_NAME)
                result_sem_processed = posix_ipc.Semaphore(RESULT_SEM_PROCESSED_NAME)
                result_shm_map = mmap.mmap(result_shm.fd, 0)
                print(f"Connected to results shared memory and semaphores after {retry_count} retries")
                break
            except posix_ipc.ExistentialError:
                # If the shared memory doesn't exist yet, wait a bit and retry
                print(f"Results shared memory not available yet, retrying... ({retry_count+1}/{max_retry})")
                time.sleep(0.2)
                retry_count += 1
                if retry_count >= max_retry:
                    raise Exception("Timed out waiting for results shared memory")
        
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
            
            inference_time = (time.time() - start_time) * 1000  # Convert to ms
            timestamp_ms = int(time.monotonic() * 1000)
            # print(f"Frame {frame_num} finished: {timestamp_ms}, detections: {num_detections}")
            
            # Log results
            if frame_counter[0] % 10 == 0:
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
                    
            # Wait for result buffer to be available for writing
            try:
                # Acquire the semaphore for exclusive access to the result buffer
                result_sem_processed.acquire()
                
                # Pack all detection results into a binary format
                # First, pack the frame number and number of detections
                result_bytes = struct.pack('ii', frame_num, num_detections)
                
                # Then pack each detection
                if num_detections > 0:
                    # Get boxes, confidences, and class indices
                    xyxy = boxes.xyxy.cpu().numpy()  # Boxes in xyxy format
                    conf_values = boxes.conf.cpu().numpy()  # Confidence values
                    cls_indices = boxes.cls.cpu().numpy().astype(np.int32)  # Class indices
                    
                    # For each detection, pack x1, y1, x2, y2, confidence, class_id
                    for i in range(num_detections):
                        box = xyxy[i]
                        x1, y1, x2, y2 = box
                        confidence = conf_values[i]
                        class_id = cls_indices[i]
                        
                        # Pack as floats and int
                        detection_bytes = struct.pack('fffffi', 
                                                    float(x1), float(y1), 
                                                    float(x2), float(y2), 
                                                    float(confidence), 
                                                    int(class_id))
                        result_bytes += detection_bytes
                
                # Write the bytes directly to shared memory
                # Make sure we don't exceed the allocated memory size
                if len(result_bytes) > RESULT_SIZE:
                    print(f"Warning: Detection data size ({len(result_bytes)} bytes) exceeds buffer size ({RESULT_SIZE} bytes)")
                    # Truncate data if necessary
                    result_bytes = result_bytes[:RESULT_SIZE]
                
                # Clear buffer first
                result_shm_map[:] = b'\x00' * RESULT_SIZE
                
                # Write data
                result_shm_map[:len(result_bytes)] = result_bytes
                
                # Signal that result is ready
                result_sem_ready.release()
            except Exception as e:
                print(f"Error writing result: {e}")
            
            frame_counter[0] += 1
            total_latency += inference_time
    
    except posix_ipc.ExistentialError as e:
        print(f"Shared memory or semaphore doesn't exist: {e}")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        # Signal monitoring thread to stop
        running_flag[0] = False
        
        # Clean up resources
        if show:
            cv2.destroyAllWindows()
            
        cleanup_resources(shm, sem_ready, sem_processed, SHM_NAME, SEM_READY_NAME, SEM_PROCESSED_NAME)
        cleanup_result_resources(result_shm, result_sem_ready, result_sem_processed, 
                               RESULT_SHM_NAME, RESULT_SEM_READY_NAME, RESULT_SEM_PROCESSED_NAME)
        
        # Print summary
        if frame_counter[0] > 0:
            avg_latency = total_latency / frame_counter[0]
            print(f"\nProcessed {frame_counter[0]} frames")
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
    
    return parser.parse_args()

def main():
    """Main function."""
    args = parse_args()
    
    process_frames_with_yolo(
        model_path=args.model,
        conf=args.conf,
        show=args.show,
        device=args.device,
        save_results=args.save
    )

if __name__ == "__main__":
    main()