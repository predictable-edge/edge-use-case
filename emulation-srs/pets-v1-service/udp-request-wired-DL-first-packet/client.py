import socket
import argparse
import struct
import time
import os
import json
import ctypes
import threading
import subprocess
from datetime import datetime
import queue

libc = ctypes.CDLL('libc.so.6', use_errno=True)
MAX_UDP_SIZE = 1300

def setns(fd, nstype):
    if libc.setns(fd, nstype) != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, os.strerror(errno))

def enter_netns(namespace):
    netns_path = f'/var/run/netns/{namespace}'
    try:
        fd = os.open(netns_path, os.O_RDONLY)
        setns(fd, 0)
        os.close(fd)
    except Exception as e:
        print(f"Failed to enter namespace {namespace}: {e}")
        raise

def parse_ue_info(line):
    """
    Parse a single line of UE information
    Format example:
    NR    1     2  0    0         idle              registered     1 10.45.0.3
    NR    0     1  0 4b0c      running              registered     1 10.45.0.2
    Returns:
        tuple: (ue_id, rnti_str, rrc_state, emm_state) or None if parse fails
    Note: RNTI is kept as string to handle both decimal and hex formats
    """
    try:
        parts = line.split()
        if len(parts) >= 8 and 'NR' in parts[0]:
            ue_id = int(parts[2])     # UE ID is in column 3
            rnti_str = parts[4]       # RNTI is in column 5 (keep as string)
            rrc_state = parts[5]      # RRC state in column 6
            emm_state = parts[6]      # EMM state in column 7
            return (ue_id, rnti_str, rrc_state, emm_state)
    except Exception as e:
        print(f"Debug - Parse error for line '{line}': {e}")
        return None
    return None

def get_ue_rnti(ue_id):
    """
    Get UE's RNTI by parsing screen output from amarisoft UE command.
    Args:
        ue_id: int, UE identifier (1-based)
    Returns:
        int: RNTI value if found and valid, None otherwise
    """
    try:
        # Execute command to send 'ue' to screen session
        cmd = "screen -S lte -X stuff 'ue\n'"
        subprocess.run(cmd, shell=True)
        
        time.sleep(0.5)
        
        # Capture screen output to file
        cmd = "screen -S lte -X hardcopy /tmp/ue_output.txt"
        subprocess.run(cmd, shell=True)
        
        # Parse output file to find RNTI for specific UE
        with open('/tmp/ue_output.txt', 'r') as f:
            for line in f:
                info = parse_ue_info(line)
                if info and info[0] == ue_id:
                    ue_id, rnti_str, rrc_state, emm_state = info
                    if rnti_str and rrc_state == "running":
                        return rnti_str
                    else:
                        print(f"UE {ue_id} not in running state: RNTI={rnti_str}, RRC={rrc_state}, EMM={emm_state}")
                        return None
    except Exception as e:
        print(f"Error getting RNTI: {e}")
    return None

def trigger_rrc_connection(namespace):
    """
    Trigger RRC connection by sending ping packets in specified network namespace
    Args:
        namespace: network namespace name (e.g. 'ue0')
    Returns:
        bool: True if ping was successful, False otherwise
    """
    try:
        cmd = f"ip netns exec {namespace} ping -c 1 192.168.2.2"
        print(cmd)
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        
        if result.returncode == 0:
            print(f"Successfully triggered RRC connection in namespace {namespace}")
            return True
        else:
            print(f"Failed to trigger RRC connection in namespace {namespace}")
            return False
            
    except Exception as e:
        print(f"Error while triggering RRC connection: {e}")
        return False

class UEClient:
    def __init__(self, config, server_ip, server_port):
        """
        config: Dictionary containing UE configuration
        {
            'namespace': str,
            'listen_port': int,
            'num_requests': int,
            'request_size': int,  # number of packets per request or file size in KB
            'interval': int,
            'latency_req': int,   # latency requirement in ms
            'controller_ip': str,
            'controller_port': int,
            'type': str           # 'latency' or 'file'
        }
        """
        self.namespace = config['namespace']
        self.server_ip = server_ip
        self.server_port = server_port
        self.listen_port = config['listen_port']
        self.packets_per_request = config['request_size']
        self.num_requests = config['num_requests']
        self.interval = config['interval']
        self.latency_req = config.get('latency_req', 100)
        self.controller_ip = config['controller_ip']
        self.controller_port = config['controller_port']
        self.type = config.get('type', 'latency')  # Default to latency if not specified
        
        # For file transfer, use fixed port 20000
        if self.type == 'file':
            self.file_port = 20000  # Use fixed port 20000 for all file transfers
            self.file_size_kb = self.packets_per_request  # In KB
        
        # Get UE ID from namespace name
        self.ue_id = int(self.namespace[2:])
        self.rnti = None
        
        self.payload_size = MAX_UDP_SIZE
        self.packet_size = self.payload_size + 28
        
        self.send_times = {}
        self.lock = threading.Lock()
        self.registration_complete = threading.Event()
        self.registration_timeout = 5.0
        self.result_dir = None
        
        # First initialize RNTI
        self.initialize_connection()
        
        # Only create controller socket for latency type
        if self.type == 'latency':
            self.controller_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                self.controller_socket.connect((self.controller_ip, self.controller_port))
                print(f"Connected to controller at {self.controller_ip}:{self.controller_port}")
                # Send an initial message to controller
                self.send_controller_message(0)
            except Exception as e:
                print(f"Failed to connect to controller: {e}")
                raise
        else:
            self.controller_socket = None

    def send_controller_message(self, seq_number):
        """Send message to controller"""
        try:
            message = f"Start|{self.rnti}|{seq_number}\n"
            self.controller_socket.sendall(message.encode())
        except Exception as e:
            print(f"Error sending controller message: {e}")

    def initialize_connection(self):
        """Initialize RRC connection and get initial RNTI"""
        # First try to trigger RRC connection
        trigger_rrc_connection(self.namespace)
        
        # Try to get RNTI multiple times with short delays
        max_attempts = 5
        for attempt in range(max_attempts):
            self.rnti = get_ue_rnti(self.ue_id)
            if self.rnti is not None:
                print(f"Successfully initialized UE {self.ue_id} with RNTI {self.rnti}")
                return
            if attempt < max_attempts - 1:  # Don't sleep on last attempt
                print(f"Attempt {attempt + 1}: Waiting for UE {self.ue_id} to transition to running state...")
                time.sleep(0.5)
        
        raise Exception(f"Could not get initial RNTI for UE {self.ue_id} after {max_attempts} attempts")

    def update_rnti(self):
        """Update RNTI value from real-time UE status"""
        # Just return the stored RNTI - no need to check repeatedly
        return self.rnti

    def send_requests(self):
        """Choose the appropriate send method based on type"""
        if self.type == 'file':
            self.send_file_requests()
        else:
            self.send_latency_requests()
    
    def send_latency_requests(self):
        """Original method for latency testing with UDP"""
        try:
            # Enter namespace and create socket for UE communication
            enter_netns(self.namespace)
            print(f"UE {self.rnti}: Entered namespace {self.namespace}")
            
            send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            print(f"UE {self.rnti}: Created socket with server {self.server_ip}:{self.server_port}")
            
            try:
                # Send registration packet
                print(f"UE {self.rnti}: Sending registration packet...")
                header = struct.pack('!IIII4sI', 
                                   0, 0,
                                   self.packets_per_request,
                                   self.listen_port,
                                   self.rnti.encode().ljust(4),
                                   self.latency_req)
                data = header + b'\0' * self.packet_size
                send_socket.sendto(data, (self.server_ip, self.server_port))
                
                print(f"UE {self.rnti}: Waiting for registration confirmation...")
                if not self.registration_complete.wait(self.registration_timeout):
                    print(f"UE {self.rnti}: Registration timeout")
                    return
                
                print(f"UE {self.rnti}: Registration confirmed, starting requests...")
                time.sleep(5)
                
                # Send requests
                for request_id in range(1, self.num_requests + 1):
                    with self.lock:
                        self.send_times[request_id] = time.time()
                    
                    # First send controller message before sending any UDP packets
                    self.send_controller_message(request_id)
                    
                    for seq_num in range(self.packets_per_request):
                        header = struct.pack('!IIII4sI', 
                                           request_id, seq_num, 
                                           self.packets_per_request,
                                           self.listen_port,
                                           self.rnti.encode().ljust(4),
                                           self.latency_req)
                        data = header + b'\0' * self.payload_size
                        send_socket.sendto(data, (self.server_ip, self.server_port))
                    
                    if request_id % 100 == 0:
                        print(f"UE {self.rnti}: Sent request {request_id}")
                    
                    if request_id != self.num_requests:
                        time.sleep(self.interval / 1000.0)
                
                print(f"UE {self.rnti}: All requests sent")
                
            except Exception as e:
                print(f"Error sending requests for UE {self.ue_id}: {e}")
                raise
            
        except Exception as e:
            print(f"Error in send loop for UE {self.ue_id}: {e}")
        finally:
            send_socket.close()
            if self.controller_socket:
                self.controller_socket.close()
                
    def send_file_requests(self):
        """Method for file transfer with TCP"""
        try:
            # Enter namespace and create socket for UE communication
            enter_netns(self.namespace)
            print(f"UE {self.rnti}: Entered namespace {self.namespace}")
            
            try:
                # Send files
                for request_id in range(1, self.num_requests + 1):
                    # Create TCP socket for each file transfer
                    file_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    file_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    
                    try:
                        # Connect to server's file port
                        # print(f"UE {self.rnti}: Connecting to server for file transfer at {self.server_ip}:{self.file_port}")
                        file_socket.connect((self.server_ip, self.file_port))
                        
                        with self.lock:
                            self.send_times[request_id] = time.time()
                        
                        # Send registration info: RNTI|request_id|file_size_kb|latency_req
                        reg_info = f"{self.rnti}|{request_id}|{self.file_size_kb}|{self.latency_req}\n".encode()
                        file_socket.sendall(reg_info)
                        
                        # Create file data (random bytes)
                        file_data = b'\0' * (self.file_size_kb * 1024)  # Convert KB to bytes
                        
                        # Send file data
                        bytes_sent = 0
                        total_size = len(file_data)
                        
                        while bytes_sent < total_size:
                            sent = file_socket.send(file_data[bytes_sent:bytes_sent + 4096])
                            if sent == 0:
                                raise RuntimeError("Socket connection broken")
                            bytes_sent += sent
                        
                        # Wait for completion response
                        response = file_socket.recv(1024).decode().strip()
                        
                        if response.startswith("DONE"):
                            # Format: "DONE|request_id|time"
                            parts = response.split('|')
                            if len(parts) >= 3 and int(parts[1]) == request_id:
                                receive_time = time.time()
                                with self.lock:
                                    if request_id in self.send_times:
                                        latency = (receive_time - self.send_times[request_id]) * 1000
                                        self.write_file_result(request_id, latency)
                        
                        if request_id % 10 == 0:
                            print(f"UE {self.rnti}: Sent file {request_id}/{self.num_requests}")
                        
                    finally:
                        file_socket.close()
                    
                    if request_id != self.num_requests:
                        time.sleep(self.interval / 1000.0)
                
                print(f"UE {self.rnti}: All files sent")
                
            except Exception as e:
                print(f"Error sending files for UE {self.ue_id}: {e}")
                raise
            
        except Exception as e:
            print(f"Error in file transfer loop for UE {self.ue_id}: {e}")
        finally:
            if self.controller_socket:
                self.controller_socket.close()
                
    def write_file_result(self, request_id, latency):
        """Write file transfer result to output file"""
        if not self.result_dir:
            print(f"Error: result_dir not set for UE {self.ue_id}")
            return
            
        try:
            os.makedirs(self.result_dir, exist_ok=True)
            latency_file = os.path.join(self.result_dir, f'latency_rnti{self.ue_id}.txt')
            
            with open(latency_file, 'a') as f:
                if request_id == 1:
                    # Write configuration header for first request
                    f.write(f"\n\n=== Configuration: file_size={self.file_size_kb}KB, interval={self.interval} ===\n")
                    f.write(f"{'Request ID':<15}{'Latency (ms)':>15}\n")
                
                f.write(f"{request_id:<15}{latency:>15.2f}\n")
                f.flush()
        
        except Exception as e:
            print(f"Error writing file result for UE {self.ue_id}: {e}")

    def receive_responses(self, result_dir):
        self.result_dir = result_dir
        
        # For file type, we don't need to receive UDP responses
        if self.type == 'file':
            return
        
        try:
            recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            recv_socket.bind(('', self.listen_port))
        except Exception as e:
            print(f"Socket error for UE {self.ue_id}: {e}")
            return

        os.makedirs(result_dir, exist_ok=True)
        latency_file = os.path.join(result_dir, f'latency_rnti{self.ue_id}.txt')
        first_packet_file = os.path.join(result_dir, f'first_packet_latency_rnti{self.ue_id}.txt')
        completed_requests = {}
        first_packet_times = {}

        try:
            # Open both files in append mode
            with open(latency_file, 'a') as f, open(first_packet_file, 'a') as fp:
                # Write configuration headers
                f.write(f"\n\n=== Configuration: request_size={self.packets_per_request}, interval={self.interval} ===\n")
                f.write(f"{'Request ID':<15}{'Latency (ms)':>15}\n")
                
                fp.write(f"\n\n=== Configuration: request_size={self.packets_per_request}, interval={self.interval} ===\n")
                fp.write(f"{'Request ID':<15}{'First Packet Latency (ms)':>25}{'Total Latency (ms)':>20}\n")
                
                next_request = 1

                while len(completed_requests) < self.num_requests:
                    try:
                        data, _ = recv_socket.recvfrom(64)
                        request_id, rnti_bytes, response_type = struct.unpack('!I4sB', data[:9])
                        recv_rnti = rnti_bytes.decode().strip()
                        
                        if recv_rnti != self.rnti:
                            continue

                        # Mark registration as complete when receiving response to request_id 0
                        if request_id == 0:
                            self.registration_complete.set()
                            continue

                        receive_time = time.time()
                        with self.lock:
                            if request_id in self.send_times:
                                if response_type == 1:  # First packet response
                                    first_packet_latency = (receive_time - self.send_times[request_id]) * 1000
                                    first_packet_times[request_id] = first_packet_latency
                                elif response_type == 2:  # Complete request response
                                    total_latency = (receive_time - self.send_times[request_id]) * 1000
                                    completed_requests[request_id] = total_latency
                                    del self.send_times[request_id]

                                    # Write both latencies to the first packet file if we have both
                                    if request_id in first_packet_times:
                                        fp.write(f"{request_id:<15}{first_packet_times[request_id]:>25.2f}{total_latency:>20.2f}\n")
                                        fp.flush()
                                        del first_packet_times[request_id]

                        # Write to the original latency file when request is complete
                        while next_request <= self.num_requests and next_request in completed_requests:
                            latency = completed_requests[next_request]
                            f.write(f"{next_request:<15}{latency:>15.2f}\n")
                            f.flush()
                            next_request += 1

                    except Exception as e:
                        print(f"Error receiving for UE {self.ue_id}: {e}")

        finally:
            recv_socket.close()

class MultiUEClient:
    def __init__(self, config_file, server_ip, server_port):
        """
        config_file: Path to JSON configuration file
        """
        # Load configurations from JSON file
        with open(config_file, 'r') as f:
            configs = json.load(f)
        
        # Create folder name from UE configurations
        folder_name_parts = []
        for config in configs:
            ue_part = f"{config['namespace']}-{config['request_size']}-{config['interval']}-{config['latency_req']}"
            folder_name_parts.append(ue_part)
        
        folder_name = '-'.join(folder_name_parts)
        # Add timestamp at the end
        folder_name = f"{folder_name}-{datetime.now().strftime('%Y%m%d_%H%M%S')}"
        
        self.result_dir = os.path.join(
            '../result/udp-request-wired-DL-first-packet', 
            folder_name
        )
        
        # Group configurations by namespace
        self.ue_configs = {}
        for config in configs:
            namespace = config['namespace']
            if namespace not in self.ue_configs:
                self.ue_configs[namespace] = []
            self.ue_configs[namespace].append(config)
        
        # Small delay between starting clients
        self.start_delay = 0.5
        self.server_ip = server_ip
        self.server_port = server_port

    def run_ue_configs(self, configs):
        """Run all configurations for a single UE sequentially"""
        for config in configs:
            print(f"Running configuration: {config['namespace']}-{config['request_size']}-{config['interval']}")
            client = UEClient(
                config=config,
                server_ip=self.server_ip,
                server_port=self.server_port
            )
            
            # Start receive thread
            recv_thread = threading.Thread(
                target=client.receive_responses,
                args=(self.result_dir,)
            )
            recv_thread.start()
            time.sleep(0.1)
            
            # Start send thread
            send_thread = threading.Thread(
                target=client.send_requests
            )
            send_thread.start()
            
            # Wait for both threads to complete before starting next config
            send_thread.join()
            recv_thread.join()
            time.sleep(self.start_delay)

    def run(self):
        """Run configurations for different UEs in parallel"""
        ue_threads = []
        
        # Create thread for each UE to run its configs sequentially
        for namespace, configs in self.ue_configs.items():
            print(f"Starting configurations for {namespace}")
            ue_thread = threading.Thread(
                target=self.run_ue_configs,
                args=(configs,)
            )
            ue_threads.append(ue_thread)
            ue_thread.start()
            time.sleep(0.1)  # Small delay between starting UEs
        
        # Wait for all UEs to complete
        for ue_thread in ue_threads:
            ue_thread.join()
        
        print("All configurations completed")

def main():
    parser = argparse.ArgumentParser(description='Multi-UE UDP Client')
    parser.add_argument('--server-ip', type=str, required=True)
    parser.add_argument('--server-port', type=int, default=10000)
    parser.add_argument('--config', type=str, required=True,
                      help='Path to JSON configuration file')
    
    args = parser.parse_args()
    
    multi_client = MultiUEClient(
        config_file=args.config,
        server_ip=args.server_ip,
        server_port=args.server_port
    )
    
    multi_client.run()

if __name__ == "__main__":
    main() 