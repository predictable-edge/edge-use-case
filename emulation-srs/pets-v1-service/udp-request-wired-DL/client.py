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

libc = ctypes.CDLL('libc.so.6', use_errno=True)
MAX_UDP_SIZE = 1300

# Network namespace related functions remain the same
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

# UE info parsing functions remain the same
def parse_ue_info(line):
    """Parse a single line of UE information"""
    try:
        parts = line.split()
        if len(parts) >= 8 and 'NR' in parts[0]:
            ue_id = int(parts[2])
            rnti_str = parts[4]
            rrc_state = parts[5]
            emm_state = parts[6]
            return (ue_id, rnti_str, rrc_state, emm_state)
    except Exception as e:
        print(f"Parse error for line '{line}': {e}")
        return None
    return None

def get_ue_rnti(ue_id):
    """Get UE's RNTI from screen output"""
    try:
        cmd = "screen -S lte -X stuff 'ue\n'"
        subprocess.run(cmd, shell=True)
        time.sleep(0.5)
        
        cmd = "screen -S lte -X hardcopy /tmp/ue_output.txt"
        subprocess.run(cmd, shell=True)
        
        with open('/tmp/ue_output.txt', 'r') as f:
            for line in f:
                info = parse_ue_info(line)
                if info and info[0] == ue_id:
                    ue_id, rnti_str, rrc_state, emm_state = info
                    if rnti_str and rrc_state == "running":
                        return rnti_str
    except Exception as e:
        print(f"Error getting RNTI: {e}")
    return None

def trigger_rrc_connection(namespace):
    """Trigger RRC connection using ping"""
    try:
        cmd = f"ip netns exec {namespace} ping -c 1 192.168.2.2"
        print(cmd)
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.returncode == 0
    except Exception as e:
        print(f"Error triggering RRC connection: {e}")
        return False

class UEClient:
    def __init__(self, config, server_ip, server_port):
        # Basic UE configuration
        self.namespace = config['namespace']
        self.server_ip = server_ip
        self.server_port = server_port
        self.listen_port = config['listen_port']
        self.packets_per_request = config['request_size']
        self.num_requests = config['num_requests']
        self.interval = config['interval']
        self.latency_req = config.get('latency_req', 100)
        
        # Controller configuration
        self.controller_ip = config['controller_ip']
        self.controller_port = config['controller_port']
        self.controller_socket = None
        
        # UE identification
        self.ue_id = int(self.namespace[2:])
        self.rnti = None
        
        # UDP packet configuration
        header_size = 28  # 5 ints + 1 string(4 chars)
        self.payload_size = MAX_UDP_SIZE - header_size
        
        # Synchronization
        self.send_times = {}
        self.lock = threading.Lock()
        self.registration_complete = threading.Event()
        self.registration_timeout = 5.0
        
        # Initialize connections
        self.initialize_connection()
        self.connect_to_controller()

    def connect_to_controller(self):
        """Establish TCP connection with controller"""
        try:
            self.controller_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.controller_socket.connect((self.controller_ip, self.controller_port))
            print(f"UE {self.rnti}: Connected to controller at {self.controller_ip}:{self.controller_port}")
        except Exception as e:
            print(f"UE {self.rnti}: Failed to connect to controller: {e}")
            self.controller_socket = None

    def notify_controller(self, seq_num):
        """Send notification to controller about new request"""
        if not self.controller_socket:
            return
            
        try:
            # Format: Start|rnti|seq_number
            msg = f"Start|{self.rnti}|{seq_num}\n"
            self.controller_socket.send(msg.encode('utf-8'))
        except Exception as e:
            print(f"UE {self.rnti}: Failed to notify controller: {e}")
            self.controller_socket = None

    def initialize_connection(self):
        """Initialize RRC connection and get RNTI"""
        trigger_rrc_connection(self.namespace)
        self.rnti = get_ue_rnti(self.ue_id)
        if self.rnti is None:
            raise Exception(f"Could not get RNTI for UE {self.ue_id}")
        print(f"Successfully initialized UE {self.ue_id} with RNTI {self.rnti}")

    def send_requests(self):
        try:
            enter_netns(self.namespace)
            send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            
            try:
                # Send registration packet
                header = struct.pack('!IIII4sI', 
                                   0, 0,
                                   self.packets_per_request,
                                   self.listen_port,
                                   self.rnti.encode().ljust(4),
                                   self.latency_req)
                data = header + b'\0' * self.payload_size
                send_socket.sendto(data, (self.server_ip, self.server_port))
                
                if not self.registration_complete.wait(self.registration_timeout):
                    print(f"UE {self.rnti}: Registration timeout")
                    return
                
                # Send requests at specified interval
                for request_id in range(1, self.num_requests + 1):
                    # Notify controller about new request
                    self.notify_controller(request_id)
                    
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
                
            except Exception as e:
                print(f"Error sending requests for UE {self.ue_id}: {e}")
                raise
            
        except Exception as e:
            print(f"Error in send loop for UE {self.ue_id}: {e}")
        finally:
            send_socket.close()
            if self.controller_socket:
                self.controller_socket.close()

    # receive_responses method remains the same as the simple version

# MultiUEClient class remains the same as the simple version

# Main function remains the same as the simple version

if __name__ == "__main__":
    main() 