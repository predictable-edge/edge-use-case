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
import struct

def ensure_dir(directory):
    """Create directory if it doesn't exist."""
    if not os.path.exists(directory):
        os.makedirs(directory)

def cleanup_resources(shm, semaphores, shm_name, sem_names):
    """Clean up shared memory and semaphores."""
    if shm:
        shm.close_fd()
    for sem in semaphores:
        if sem:
            sem.close()
    
    # Try to unlink shared resources
    try:
        posix_ipc.unlink_shared_memory(shm_name)
    except:
        pass
    
    for name in sem_names:
        try:
            posix_ipc.unlink_semaphore(name)
        except:
            pass

def signal_handler(sig, frame):
    """Handle Ctrl+C gracefully."""
    print("\nStopping frame processing...")
    sys.exit(0)

class SharedRingBuffer:
    """Python representation of the C++ SharedRingBuffer structure"""
    FORMAT = "iiiiiiqq"  # corresponds to the C++ struct layout
    SIZE = struct.calcsize(FORMAT)
    
    def __init__(self, data):
        unpacked = struct.unpack(self.FORMAT, data[:self.SIZE])
        self.buffer_size = unpacked[0]
        self.frame_width = unpacked[1]
        self.frame_height = unpacked[2]
        self.write_index = unpacked[3]
        self.read_index = unpacked[4]
        self.format_change = unpacked[5]
        self.frame_data_offset = unpacked[6]
        self.frame_size = unpacked[7]
    
    def update(self, data):
        """Update only the dynamic fields from shared memory"""
        # Only unpack the first 6 integers (static fields like offset don't change)
        unpacked = struct.unpack("iiiiii", data[:24])
        self.buffer_size = unpacked[0]
        self.frame_width = unpacked[1]
        self.frame_height = unpacked[2]
        self.write_index = unpacked[3]
        self.read_index = unpacked[4]
        self.format_change = unpacked[5]
    
    def update_read_index(self, data, new_read_index):
        """Update the read_index field in the shared memory"""
        self.read_index = new_read_index
        struct.pack_into("i", data, 16, new_read_index)  # 16 bytes offset to read_index

def process_frames_with_yolo(
    model_path,
    conf=0.3,
    show=False,
    device='cuda',
    save_results=True
):
    """
    Process frames from shared memory ring buffer with YOLO model
    
    Args:
        model_path (str): Path to YOLO model
        conf (float): Confidence threshold for YOLO
        show (bool): Whether to display detection results
        device (str): Device to run YOLO on ('cpu' or 'cuda')
        save_results (bool): Whether to save detection results
    """
    # Setup signal handler for graceful shutdown
    signal.signal(signal.SIGINT, signal_handler)
    
    # Shared memory parameters
    SHM_NAME = "/yolo_frame_ringbuf"
    SEM_SLOTS_EMPTY_NAME = "/sem_slots_empty"
    SEM_SLOTS_FILLED_NAME = "/sem_slots_filled"
    SEM_MUTEX_NAME = "/sem_mutex"
    
    # Initialize shared memory and semaphore resources
    shm = None
    sem_slots_empty = None
    sem_slots_filled = None
    sem_mutex = None
    
    try:
        # Open existing shared memory
        shm = posix_ipc.SharedMemory(SHM_NAME)
        
        # Open existing semaphores
        sem_slots_empty = posix_ipc.Semaphore(SEM_SLOTS_EMPTY_NAME)
        sem_slots_filled = posix_ipc.Semaphore(SEM_SLOTS_FILLED_NAME)
        sem_mutex = posix_ipc.Semaphore(SEM_MUTEX_NAME)
        
        # Map the shared memory to this process
        shm_map = mmap.mmap(shm.fd, 0)
        
        # Read the ring buffer header to get structure information
        header_data = shm_map[:SharedRingBuffer.SIZE]
        ring_buffer = SharedRingBuffer(header_data)
        
        print(f"Connected to shared memory ring buffer with {ring_buffer.buffer_size} slots")
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
            results_file = f"results/ringbuf_yolo_{timestamp}.txt"
            
            with open(results_file, 'w') as f:
                f.write(f"{'Frame':<10}{'Inference_Time_ms':>20}{'Objects_Detected':>20}{'Buffer_Status':>20}\n")
        
        # Process frames
        frame_count = 0
        total_latency = 0
        last_format_change = False
        
        print("Waiting for frames...")
        while True:
            # Wait for filled slot
            sem_slots_filled.acquire()
            
            # Acquire mutex to read buffer state
            sem_mutex.acquire()
            
            # Update ring buffer metadata
            ring_buffer.update(shm_map[:24])
            
            # Get current read position
            current_read_idx = ring_buffer.read_index
            
            # Check if format has changed
            if ring_buffer.format_change == 1:
                last_format_change = True
                # Reset format change flag
                struct.pack_into("i", shm_map, 20, 0)  # 20 bytes offset to format_change
            
            # Calculate frame data offset
            frame_data_offset = ring_buffer.frame_data_offset + (current_read_idx * ring_buffer.frame_size)
            
            # Update read index for next round
            next_read_idx = (current_read_idx + 1) % ring_buffer.buffer_size
            ring_buffer.update_read_index(shm_map, next_read_idx)
            
            # Get current buffer occupancy
            buffer_status = f"R:{current_read_idx} W:{ring_buffer.write_index}"
            
            # Release mutex
            sem_mutex.release()
            
            start_time = time.time()
            
            # Access frame data from shared memory
            frame_size = ring_buffer.frame_width * ring_buffer.frame_height * 3
            frame_data = memoryview(shm_map)[frame_data_offset:frame_data_offset + frame_size]
            
            # Convert to numpy array
            frame_array = np.frombuffer(frame_data, dtype=np.uint8).reshape(ring_buffer.frame_height, ring_buffer.frame_width, 3)
            
            # Signal that we're done with this slot
            sem_slots_empty.release()
            
            # If dimensions changed, log it
            if last_format_change:
                print(f"Frame dimensions changed to {ring_buffer.frame_width}x{ring_buffer.frame_height}")
                last_format_change = False
            
            # Run YOLO on frame
            results = model.predict(frame_array, conf=conf, verbose=False)
            inference_time = (time.time() - start_time) * 1000  # Convert to ms
            
            # Get detection count
            boxes = results[0].boxes
            num_detections = len(boxes)
            
            # Log results
            if frame_count % 10 == 0:
                print(f"Frame {frame_count}: {inference_time:.2f}ms, {num_detections} objects, Buffer: {buffer_status}")
            
            # Save results if enabled
            if save_results:
                with open(results_file, 'a') as f:
                    formatted_line = f"{frame_count:<10}{inference_time:>20.2f}{num_detections:>20}{buffer_status:>20}\n"
                    f.write(formatted_line)
            
            # Display if enabled
            if show:
                # Draw bounding boxes
                annotated_frame = results[0].plot()
                
                # Add text with frame info
                cv2.putText(
                    annotated_frame, 
                    f"Frame: {frame_count} | Inf: {inference_time:.1f}ms | Obj: {num_detections} | Buf: {buffer_status}", 
                    (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2
                )
                
                # Show the frame
                cv2.imshow("Ring Buffer YOLO Detections", annotated_frame)
                
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
        
        cleanup_resources(
            shm, 
            [sem_slots_empty, sem_slots_filled, sem_mutex], 
            SHM_NAME, 
            [SEM_SLOTS_EMPTY_NAME, SEM_SLOTS_FILLED_NAME, SEM_MUTEX_NAME]
        )
        
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