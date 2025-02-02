import socket
import argparse
import struct
import time
import os
import json
import ctypes
import threading
from datetime import datetime

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

def get_ue_info_from_namespace(namespace):
    """
    Read UE index and RNTI from the namespace
    Returns: (ue_idx, rnti) or None if not found
    """
    try:
        # Store current namespace to restore later
        with open('/proc/self/ns/net', 'r') as f:
            original_ns = f.fileno()
            
        # Enter UE namespace
        enter_netns(namespace)
        
        # Read UE info from srsRAN files
        # Note: Update these paths based on actual srsRAN file locations
        ue_idx_path = '/tmp/ue_idx'
        rnti_path = '/tmp/rnti'
        
        try:
            with open(ue_idx_path, 'r') as f:
                ue_idx = int(f.read().strip())
            with open(rnti_path, 'r') as f:
                rnti = int(f.read().strip())
            return ue_idx, rnti
        except FileNotFoundError:
            print(f"UE info files not found in namespace {namespace}")
            return None
        finally:
            # Restore original namespace
            setns(original_ns, 0)
    except Exception as e:
        print(f"Error reading UE info from namespace {namespace}: {e}")
        return None

class UEClient:
    def __init__(self, config, server_ip, server_port):
        """
        config: Dictionary containing UE configuration
        {
            'rnti': int,
            'namespace': str,
            'listen_port': int,
            'num_requests': int,
            'request_size': int,  # number of packets per request
            'interval': int,
            'latency_req': int    # latency requirement in ms
        }
        """
        self.rnti = config['rnti']
        self.namespace = config['namespace']
        self.server_ip = server_ip
        self.server_port = server_port
        self.listen_port = config['listen_port']
        self.packets_per_request = config['request_size']
        self.num_requests = config['num_requests']
        self.interval = config['interval']
        self.latency_req = config.get('latency_req', 100)  # Default to 20ms if not specified
        
        # Header size and payload size
        header_size = 20  # 5 ints: request_id, seq_num, total_packets, rnti, listen_port
        self.payload_size = MAX_UDP_SIZE - header_size
        
        self.send_times = {}
        self.lock = threading.Lock()

    def send_requests(self):
        try:
            enter_netns(self.namespace)
            send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        except Exception as e:
            print(f"Error creating socket for UE {self.rnti}: {e}")
            return

        # Create payload of maximum allowed size
        payload = b'\0' * self.payload_size

        try:
            for request_id in range(1, self.num_requests + 1):
                with self.lock:
                    self.send_times[request_id] = time.time()

                # Send all packets for this request
                for seq_num in range(self.packets_per_request):
                    header = struct.pack('!IIIII', 
                                       request_id, seq_num, 
                                       self.packets_per_request,
                                       self.rnti,
                                       self.listen_port)
                    data = header + payload
                    send_socket.sendto(data, (self.server_ip, self.server_port))

                if request_id % 100 == 0:
                    print(f"UE {self.rnti}: Sent request {request_id} ({self.packets_per_request} packets)")

                if request_id != self.num_requests:
                    time.sleep(self.interval / 1000.0)

        except Exception as e:
            print(f"Error in send loop for UE {self.rnti}: {e}")
        finally:
            send_socket.close()

    def receive_responses(self, result_dir):
        try:
            recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            recv_socket.bind(('', self.listen_port))  # Bind to all interfaces
        except Exception as e:
            print(f"Socket error for UE {self.rnti}: {e}")
            return

        os.makedirs(result_dir, exist_ok=True)
        latency_file = os.path.join(result_dir, f'latency_rnti{self.rnti}.txt')
        completed_requests = {}

        try:
            with open(latency_file, 'w') as f:
                f.write(f"{'Request ID':<15}{'Latency (ms)':>15}\n")
                next_request = 1

                while len(completed_requests) < self.num_requests:
                    try:
                        recv_socket.settimeout(0.1)
                        try:
                            data, _ = recv_socket.recvfrom(64)
                            request_id, recv_rnti = struct.unpack('!II', data[:8])
                            
                            if recv_rnti != self.rnti:
                                continue

                            receive_time = time.time()
                            with self.lock:
                                if request_id in self.send_times:
                                    latency = (receive_time - self.send_times[request_id]) * 1000
                                    completed_requests[request_id] = min(latency, 300.0)
                                    del self.send_times[request_id]

                        except socket.timeout:
                            current_time = time.time()
                            with self.lock:
                                for req_id, send_time in list(self.send_times.items()):
                                    if current_time - send_time > 1.0:
                                        completed_requests[req_id] = 300.0
                                        del self.send_times[req_id]

                        while next_request <= self.num_requests and next_request in completed_requests:
                            latency = completed_requests[next_request]
                            f.write(f"{next_request:<15}{latency:>15.2f}\n")
                            f.flush()
                            next_request += 1

                    except Exception as e:
                        print(f"Error receiving for UE {self.rnti}: {e}")

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
    
    def run(self):
        """Run all UE clients concurrently"""
        send_threads = []
        recv_threads = []
        
        # Start all send and receive threads
        for client in self.clients:
            send_thread = threading.Thread(
                target=client.send_requests
            )
            recv_thread = threading.Thread(
                target=client.receive_responses,
                args=(self.result_dir,)
            )
            
            send_threads.append(send_thread)
            recv_threads.append(recv_thread)
            
            send_thread.start()
            recv_thread.start()
        
        # Wait for all threads to complete
        for send_thread, recv_thread in zip(send_threads, recv_threads):
            send_thread.join()
            recv_thread.join()

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