import os
import subprocess
import time
import signal
import sys
import argparse

# Function to handle Ctrl+C gracefully
def signal_handler(sig, frame):
    print("\nCtrl+C detected. Terminating all processes...")
    if cpp_process:
        cpp_process.terminate()
    sys.exit(0)

# Register signal handler
signal.signal(signal.SIGINT, signal_handler)

def run_command(command, shell=False):
    """Run a shell command and return its process"""
    if shell:
        process = subprocess.Popen(command, shell=True)
    else:
        process = subprocess.Popen(command.split())
    return process

def main():
    global cpp_process
    
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='Run detecting_tcp with YOLO')
    parser.add_argument('input_url', type=str, help='Input URL for detecting_tcp (e.g. tcp://192.168.2.3:9000?listen=1)')
    parser.add_argument('--port', type=int, default=9876, help='TCP result port (optional)')
    args = parser.parse_args()

    # Working directory - where the cpp file is located
    working_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(working_dir)
    
    print("Working directory:", working_dir)
    
    # Compile C++ code
    print("Compiling C++ code...")
    try:
        subprocess.run("make", check=True)
        print("Compilation successful")
    except subprocess.CalledProcessError:
        print("Error during compilation. Exiting...")
        return 1
    
    # Run C++ program with command line arguments
    cpp_cmd = f"./detecting_tcp {args.input_url}"
    if args.port != 9876:  # Only add port if it's different from default
        cpp_cmd += f" {args.port}"
    
    print(f"Starting C++ program: {cpp_cmd}")
    cpp_process = run_command(cpp_cmd, shell=True)
    
    # Wait for 3 seconds
    print("Waiting for 3 seconds...")
    time.sleep(3)
    
    # Run Python script in conda environment
    print("Starting Python script in Conda environment...")
    conda_cmd = "conda run -n yolo python yolo_detection_shm.py"
    py_process = run_command(conda_cmd, shell=True)
    
    # Wait for both processes to complete
    py_process.wait()
    cpp_process.wait()
    
    print("All processes completed")
    return 0

if __name__ == "__main__":
    cpp_process = None
    sys.exit(main())