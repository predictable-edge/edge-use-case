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
            'request_size': int,  # number of packets per request
            'interval': int,
            'latency_req': int    # latency requirement in ms
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
        
        # Get UE ID from namespace name
        self.ue_id = int(self.namespace[2:])
        self.rnti = None
        
        # Header size and payload size
        header_size = 28  # 5 ints + 1 string(4 chars)
        self.payload_size = MAX_UDP_SIZE - header_size
        
        self.send_times = {}
        self.lock = threading.Lock()
        self.registration_complete = threading.Event()
        self.registration_timeout = 5.0
        
        # Initialize RNTI
        self.initialize_connection()

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
        try:
            # Enter namespace and create socket
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
                data = header + b'\0' * self.payload_size
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

    def receive_responses(self, result_dir):
        try:
            recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            recv_socket.bind(('', self.listen_port))
        except Exception as e:
            print(f"Socket error for UE {self.ue_id}: {e}")
            return

        os.makedirs(result_dir, exist_ok=True)
        latency_file = os.path.join(result_dir, f'latency_rnti{self.ue_id}.txt')
        completed_requests = {}

        try:
            with open(latency_file, 'w') as f:
                f.write(f"{'Request ID':<15}{'Latency (ms)':>15}\n")
                next_request = 1

                while len(completed_requests) < self.num_requests:
                    try:
                        data, _ = recv_socket.recvfrom(64)
                        request_id, rnti_bytes = struct.unpack('!I4s', data[:8])
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
                                latency = (receive_time - self.send_times[request_id]) * 1000
                                completed_requests[request_id] = min(latency, 300.0)
                                del self.send_times[request_id]

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
        self.result_dir = os.path.join(
            '../result/tutti-service/udp-request-wired-DL', 
            datetime.now().strftime("%Y%m%d_%H%M%S")
        )
        
        # Load configurations from JSON file
        with open(config_file, 'r') as f:
            configs = json.load(f)
        
        self.clients = []
        for config in configs:
            client = UEClient(
                config=config,
                server_ip=server_ip,
                server_port=server_port
            )
            self.clients.append(client)
        
        # Small delay between starting clients
        self.start_delay = 0.5

    def run(self):
        """Run all UE clients independently"""
        send_threads = []
        recv_threads = []
        
        print("Starting receive threads...")
        # Start receive threads first
        for client in self.clients:
            recv_thread = threading.Thread(
                target=client.receive_responses,
                args=(self.result_dir,)
            )
            recv_threads.append(recv_thread)
            recv_thread.start()
            time.sleep(0.1)
        
        print("Starting send threads...")
        # Start send threads with small delay between each
        for client in self.clients:
            send_thread = threading.Thread(
                target=client.send_requests
            )
            send_threads.append(send_thread)
            send_thread.start()
            time.sleep(self.start_delay)
        
        print("Waiting for all threads to complete...")
        # Wait for all threads to complete
        for send_thread, recv_thread in zip(send_threads, recv_threads):
            try:
                send_thread.join()
                recv_thread.join()
            except Exception as e:
                print(f"Error joining threads: {e}")
        
        print("All threads completed")

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