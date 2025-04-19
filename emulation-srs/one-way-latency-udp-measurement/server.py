import socket
import json
import time
import os
import struct
from datetime import datetime
from collections import defaultdict

class UDPLatencyServer:
    """UDP server that measures one-way latency of incoming packets"""
    
    def __init__(self, ip="0.0.0.0", port=8000, buffer_size=65536):
        """Initialize the UDP server with given parameters"""
        self.udp_ip = ip
        self.udp_port = port
        self.buffer_size = buffer_size
        self.sock = None
        self.results = {}
        self.fragment_tracker = defaultdict(dict)
        
        # Create results directory if it doesn't exist
        if not os.path.exists("results"):
            os.makedirs("results")
    
    def start(self):
        """Start the UDP server and listen for incoming packets"""
        # Create UDP socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((self.udp_ip, self.udp_port))
        
        print(f"UDP server listening on {self.udp_ip}:{self.udp_port}")
        
        try:
            while True:
                # Receive data
                data, addr = self.sock.recvfrom(self.buffer_size)
                recv_time = time.time()
                
                self.process_packet(data, addr, recv_time)
        
        except KeyboardInterrupt:
            print("Server shutting down")
        finally:
            self.close()
    
    def process_packet(self, data, addr, recv_time):
        """Process received UDP packet"""
        try:
            # Check if this is a fragmented request
            if len(data) >= 17 and data[16:17] == b'\x01':  # The 17th byte is fragment info
                self.handle_fragmented_packet(data, addr, recv_time)
            else:
                self.handle_single_packet(data, addr, recv_time)
        
        except Exception as e:
            print(f"Error processing packet: {e}")
    
    def handle_fragmented_packet(self, data, addr, recv_time):
        """Handle a fragmented packet"""
        # Fragmented packet format (extended header)
        # request_id (4 bytes), send_time (8 bytes), size (4 bytes), fragment_idx (1 byte), total_fragments (1 byte)
        header_format = '!IQIIB'
        header_size = struct.calcsize(header_format)
        
        if len(data) < header_size:
            print(f"Fragment too small to contain header. Skipping.")
            return
        
        request_id, send_time, request_size, fragment_idx, total_fragments = struct.unpack(
            header_format, data[:header_size]
        )
        
        # Store fragment info
        if fragment_idx == 0:  # First fragment has the timestamp
            # Calculate latency from the first fragment
            latency = (recv_time - send_time/1000) * 1000
            
            self.fragment_tracker[request_id]['latency'] = latency
            self.fragment_tracker[request_id]['size'] = request_size
            self.fragment_tracker[request_id]['total_fragments'] = total_fragments
            self.fragment_tracker[request_id]['received_fragments'] = [fragment_idx]
            
            print(f"Received first fragment for request {request_id}, size: {request_size} bytes")
        else:
            # Track subsequent fragments
            if request_id in self.fragment_tracker:
                self.fragment_tracker[request_id]['received_fragments'].append(fragment_idx)
                
                print(f"Received fragment {fragment_idx + 1}/{total_fragments} for request {request_id}")
        
        # Check if we have all fragments
        if request_id in self.fragment_tracker and len(self.fragment_tracker[request_id]['received_fragments']) == self.fragment_tracker[request_id]['total_fragments']:
            # All fragments received, move to results
            self.results[request_id] = {
                'latency': self.fragment_tracker[request_id]['latency'],
                'size': self.fragment_tracker[request_id]['size']
            }
            
            print(f"Completed request {request_id}, all {total_fragments} fragments received")
            
            # Clean up tracker
            del self.fragment_tracker[request_id]
        
        # Check for FINAL marker in the last fragment
        if fragment_idx == total_fragments - 1 and len(data) > header_size and data[header_size:header_size+5] == b"FINAL":
            num_requests = struct.unpack('!I', data[header_size+5:header_size+9])[0]
            self.save_results(num_requests, request_size)
    
    def handle_single_packet(self, data, addr, recv_time):
        """Handle a single (non-fragmented) packet"""
        # Standard single-packet format
        # request_id (4 bytes), send_time (8 bytes), size (4 bytes)
        request_id, send_time, request_size = struct.unpack('!IQI', data[:16])
        
        # Calculate one-way latency (ms)
        latency = (recv_time - send_time/1000) * 1000
        
        # Store result
        self.results[request_id] = {
            "latency": latency,
            "size": request_size
        }
        
        print(f"Received request {request_id} from {addr}, size: {request_size} bytes, latency: {latency:.2f} ms")
        
        # If this is the last packet, save results
        if len(data) > 16 and data[16:21] == b"FINAL":
            num_requests = struct.unpack('!I', data[21:25])[0]
            self.save_results(num_requests, request_size)
    
    def save_results(self, request_number, request_size):
        """Save the latency results to a file"""
        timestamp = datetime.now().strftime("%Y%m%d%H%M%S")
        filename = f"results/udp-{request_number}-{request_size}-{timestamp}.txt"
        
        with open(filename, 'w') as f:
            f.write("Request_ID,Latency_ms\n")
            for req_id in sorted(self.results.keys()):
                f.write(f"{req_id},{self.results[req_id]['latency']:.2f}\n")
        
        print(f"Results saved to {filename}")
        
        # Reset for next test
        self.results = {}
    
    def close(self):
        """Close the server socket"""
        if self.sock:
            self.sock.close()
            self.sock = None


def main():
    # Create and start the UDP server
    server = UDPLatencyServer()
    server.start()


if __name__ == "__main__":
    main()
