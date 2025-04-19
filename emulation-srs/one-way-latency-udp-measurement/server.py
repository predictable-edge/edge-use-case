import socket
import json
import time
import os
import struct
import argparse
from datetime import datetime
from collections import defaultdict

class UDPLatencyServer:
    """UDP server that measures one-way latency of incoming packets"""
    
    # Constants for packet types
    SETUP_PACKET = 0
    DATA_PACKET = 1
    FRAGMENT_PACKET = 2
    
    def __init__(self, ip="0.0.0.0", port=8000, buffer_size=65536):
        """Initialize the UDP server with given parameters"""
        self.udp_ip = ip
        self.udp_port = port
        self.buffer_size = buffer_size
        self.sock = None
        self.results = {}
        self.fragment_tracker = defaultdict(dict)
        self.expected_requests = None
        self.request_size = None
        self.interval_ms = None
        self.received_unique_requests = set()
        self.test_in_progress = False
        self.is_running = False
        self.test_start_time = None
        
        # Create results directory if it doesn't exist
        if not os.path.exists("results"):
            os.makedirs("results")
    
    def start(self):
        """Start the UDP server and listen for incoming packets"""
        # Create UDP socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((self.udp_ip, self.udp_port))
        
        # Set a timeout to check for completion
        self.sock.settimeout(0.5)
        
        print(f"UDP server listening on {self.udp_ip}:{self.udp_port}")
        print("Waiting for client setup packet...")
        
        self.is_running = True
        
        try:
            while self.is_running:
                try:
                    # Receive data
                    data, addr = self.sock.recvfrom(self.buffer_size)
                    recv_time = time.time()
                    
                    # Check if this is a setup packet
                    if len(data) >= 4 and data[0:1] == struct.pack('!B', self.SETUP_PACKET):
                        self.handle_setup_packet(data, addr)
                    else:
                        # Only process data packets if a test is in progress
                        if self.test_in_progress:
                            self.process_packet(data, addr, recv_time)
                            
                            # Check if we've received all expected requests
                            if len(self.received_unique_requests) >= self.expected_requests:
                                print(f"Received all {self.expected_requests} expected requests. Test complete.")
                                self.save_final_results()
                                self.test_in_progress = False
                                print("Waiting for next test setup...")
                        else:
                            print(f"Received data packet from {addr} but no test is in progress. Ignoring.")
                
                except socket.timeout:
                    # Just a timeout for checking stop conditions
                    continue
        
        except KeyboardInterrupt:
            print("Server shutting down")
            if self.test_in_progress and self.results:
                self.save_final_results()
        finally:
            self.close()
    
    def handle_setup_packet(self, data, addr):
        """Handle a setup packet from the client"""
        try:
            # Setup packet format:
            # byte 0: packet type (0 for setup)
            # bytes 1-4: request count (uint32)
            # bytes 5-8: request size (uint32)
            # bytes 9-12: interval ms (uint32)
            if len(data) < 13:
                print(f"Setup packet too small. Ignoring.")
                return
                
            _, request_count, request_size, interval_ms = struct.unpack('!BIII', data[:13])
            
            # If a test is already in progress, finish it
            if self.test_in_progress and self.results:
                print("Previous test in progress. Saving results before starting new test.")
                self.save_final_results()
            
            # Reset for new test
            self.expected_requests = request_count
            self.request_size = request_size
            self.interval_ms = interval_ms
            self.results = {}
            self.fragment_tracker = defaultdict(dict)
            self.received_unique_requests = set()
            self.test_in_progress = True
            self.test_start_time = time.time()
            
            print(f"Starting new test with parameters:")
            print(f"  Request count: {request_count}")
            print(f"  Request size: {request_size} bytes")
            print(f"  Interval: {interval_ms} ms")
            
            # Send acknowledgment back to client
            ack_packet = struct.pack('!B', self.SETUP_PACKET)
            self.sock.sendto(ack_packet, addr)
            print(f"Sent setup acknowledgment to {addr}")
            
        except Exception as e:
            print(f"Error processing setup packet: {e}")
    
    def process_packet(self, data, addr, recv_time):
        """Process received UDP packet"""
        try:
            # Check if this is a fragmented request
            if len(data) >= 2 and data[0:1] == struct.pack('!B', self.FRAGMENT_PACKET):
                self.handle_fragmented_packet(data, addr, recv_time)
            else:
                self.handle_single_packet(data, addr, recv_time)
        
        except Exception as e:
            print(f"Error processing packet: {e}")
    
    def handle_fragmented_packet(self, data, addr, recv_time):
        """Handle a fragmented packet"""
        # Fragmented packet format:
        # byte 0: packet type (2 for fragment)
        # bytes 1-4: request_id (uint32)
        # bytes 5-12: send_time (uint64)
        # bytes 13-16: total_size (uint32)
        # byte 17: fragment_idx (uint8)
        # byte 18: total_fragments (uint8)
        header_format = '!BIQIBB'
        header_size = struct.calcsize(header_format)
        
        if len(data) < header_size:
            print(f"Fragment too small to contain header. Skipping.")
            return
        
        packet_type, request_id, send_time, request_size, fragment_idx, total_fragments = struct.unpack(
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
                
                if fragment_idx % 10 == 0:  # Only print every 10th fragment to reduce output
                    print(f"Received fragment {fragment_idx + 1}/{total_fragments} for request {request_id}")
        
        # Check if we have all fragments
        if request_id in self.fragment_tracker and len(self.fragment_tracker[request_id]['received_fragments']) == self.fragment_tracker[request_id]['total_fragments']:
            # All fragments received, move to results
            self.results[request_id] = {
                'latency': self.fragment_tracker[request_id]['latency'],
                'size': self.fragment_tracker[request_id]['size']
            }
            
            # Mark this request as received for tracking
            self.received_unique_requests.add(request_id)
            
            print(f"Completed request {request_id}, all {total_fragments} fragments received")
            
            # Clean up tracker
            del self.fragment_tracker[request_id]
    
    def handle_single_packet(self, data, addr, recv_time):
        """Handle a single (non-fragmented) packet"""
        # Standard single-packet format:
        # byte 0: packet type (1 for data)
        # bytes 1-4: request_id (uint32)
        # bytes 5-12: send_time (uint64)
        # bytes 13-16: size (uint32)
        header_format = '!BIQI'
        header_size = struct.calcsize(header_format)
        
        if len(data) < header_size:
            print(f"Packet too small to contain header. Skipping.")
            return
            
        packet_type, request_id, send_time, request_size = struct.unpack(header_format, data[:header_size])
        
        # Calculate one-way latency (ms)
        latency = (recv_time - send_time/1000) * 1000
        
        # Store result
        self.results[request_id] = {
            "latency": latency,
            "size": request_size
        }
        
        # Mark this request as received for tracking
        self.received_unique_requests.add(request_id)
        
        # Only print every 10th packet to reduce output
        if request_id % 10 == 0 or request_id == 1 or request_id == self.expected_requests:
            print(f"Received request {request_id}/{self.expected_requests} from {addr}, latency: {latency:.2f} ms")
    
    def save_final_results(self):
        """Save final results when server is stopping"""
        if not self.results:
            print("No results to save")
            return
            
        timestamp = datetime.now().strftime("%Y%m%d%H%M%S")
        filename = f"results/udp-{self.expected_requests}-{self.request_size}-{timestamp}.txt"
        
        # Calculate statistics if we have results
        latencies = [result['latency'] for result in self.results.values()]
        avg_latency = sum(latencies) / len(latencies)
        min_latency = min(latencies)
        max_latency = max(latencies)
        std_dev = 0
        if len(latencies) > 1:
            variance = sum((x - avg_latency) ** 2 for x in latencies) / len(latencies)
            std_dev = variance ** 0.5
        
        with open(filename, 'w') as f:
            # Format column headers with fixed width
            f.write(f"{'Request_ID':<12} {'Latency_ms':<12}\n")
            
            # Write data with fixed width columns for better readability
            for req_id in sorted(self.results.keys()):
                latency = self.results[req_id]['latency']
                f.write(f"{req_id:<12} {latency:<12.2f}\n")
        
        print(f"Results saved to {filename}")
        print(f"Test summary:")
        print(f"  Received {len(self.received_unique_requests)}/{self.expected_requests} requests")
        print(f"  Test duration: {time.time() - self.test_start_time:.2f} seconds")
        print(f"  Average latency: {avg_latency:.2f} ms")
        print(f"  Minimum latency: {min_latency:.2f} ms")
        print(f"  Maximum latency: {max_latency:.2f} ms")
        print(f"  Standard deviation: {std_dev:.2f} ms")
    
    def close(self):
        """Close the server socket"""
        if self.sock:
            self.sock.close()
            self.sock = None


def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='UDP Latency Server')
    parser.add_argument('--ip', default='0.0.0.0', help='IP address to bind to')
    parser.add_argument('--port', type=int, default=8000, help='Port to listen on')
    
    args = parser.parse_args()
    
    # Create and start the UDP server
    server = UDPLatencyServer(
        ip=args.ip,
        port=args.port
    )
    server.start()


if __name__ == "__main__":
    main()
