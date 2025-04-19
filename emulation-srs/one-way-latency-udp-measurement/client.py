import socket
import time
import struct
import argparse
import sys
import math

class UDPLatencyClient:
    """UDP client that sends requests to measure one-way latency"""
    
    # Maximum safe UDP packet size (to avoid fragmentation at IP layer)
    MAX_UDP_PACKET_SIZE = 65000  # Actually 65507, but using a more conservative value
    
    # Packet type constants
    SETUP_PACKET = 0
    DATA_PACKET = 1
    FRAGMENT_PACKET = 2
    
    def __init__(self, server_ip, port=8000, buffer_size=65536):
        """Initialize the UDP client with server address"""
        self.server_ip = server_ip
        self.server_port = port
        self.server_address = (server_ip, port)
        self.sock = None
    
    def initialize_socket(self):
        """Create and configure the UDP socket"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Set socket options (optional)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        # Set a timeout for receiving setup acknowledgment
        self.sock.settimeout(5.0)
    
    def close(self):
        """Close the socket"""
        if self.sock:
            self.sock.close()
            self.sock = None
    
    def setup_test(self, request_count, interval_ms, request_size):
        """Send a setup packet to the server and wait for acknowledgment"""
        if not self.sock:
            self.initialize_socket()
            
        # Create setup packet
        # byte 0: packet type (0 for setup)
        # bytes 1-4: request count (uint32)
        # bytes 5-8: request size (uint32)
        # bytes 9-12: interval ms (uint32)
        setup_packet = struct.pack('!BIII', self.SETUP_PACKET, request_count, request_size, interval_ms)
        
        print(f"Sending setup packet to {self.server_ip}:{self.server_port}")
        print(f"  Request count: {request_count}")
        print(f"  Request size: {request_size} bytes")
        print(f"  Interval: {interval_ms} ms")
        
        # Send setup packet and wait for acknowledgment
        max_retries = 3
        for attempt in range(max_retries):
            try:
                self.sock.sendto(setup_packet, self.server_address)
                
                # Wait for acknowledgment
                data, addr = self.sock.recvfrom(1024)
                
                if data and data[0:1] == struct.pack('!B', self.SETUP_PACKET):
                    print(f"Received setup acknowledgment from server")
                    return True
                else:
                    print(f"Received unexpected response from server. Retrying...")
            
            except socket.timeout:
                print(f"Timeout waiting for server acknowledgment (attempt {attempt+1}/{max_retries})")
        
        print("Failed to get server acknowledgment. Aborting test.")
        return False
    
    def run_test(self, request_count, interval_ms, request_size):
        """Run the latency test with the given parameters"""
        # Validate arguments
        if request_size < 16:
            print("Request size must be at least 16 bytes to hold header information")
            return False
        
        # First, setup the test with the server
        if not self.setup_test(request_count, interval_ms, request_size):
            return False
            
        # Calculate fragments needed if size is larger than max UDP packet
        fragment_size = self.MAX_UDP_PACKET_SIZE - 24  # Accounting for headers
        fragments_needed = math.ceil(request_size / fragment_size)
        
        if fragments_needed > 1:
            print(f"Request size exceeds maximum UDP packet size. Splitting into {fragments_needed} fragments.")
        
        print(f"Starting UDP latency test...")
        test_start_time = time.time()
        
        try:
            for i in range(request_count):
                request_id = i + 1
                timestamp_ms = int(time.time() * 1000)  # Current time in milliseconds
                is_last_request = i == request_count - 1
                
                if fragments_needed <= 1:
                    self.send_single_packet(request_id, timestamp_ms, request_size, is_last_request)
                else:
                    self.send_fragmented_packet(request_id, timestamp_ms, request_size, fragments_needed, is_last_request)
                
                # Wait for the specified interval unless it's the last packet
                if not is_last_request:
                    time.sleep(interval_ms / 1000)
                    
                # Print progress every 10 packets
                if request_id % 10 == 0 or request_id == 1 or request_id == request_count:
                    print(f"Sent request {request_id}/{request_count}")
            
            test_duration = time.time() - test_start_time
            print(f"Test completed. Sent {request_count} requests in {test_duration:.2f} seconds.")
            print(f"Average send rate: {request_count / test_duration:.2f} packets/second")
            return True
            
        except Exception as e:
            print(f"Error during test: {e}")
            return False
    
    def send_single_packet(self, request_id, timestamp_ms, request_size, is_last_request):
        """Send a single UDP packet"""
        # Prepare header: packet_type, request_id, timestamp, request_size
        header = struct.pack('!BIQI', self.DATA_PACKET, request_id, timestamp_ms, request_size)
        
        # Calculate padding size
        padding_size = request_size - len(header)
        padding = b'X' * max(0, padding_size)
        
        # Combine all parts
        packet = header + padding
        
        # Send packet
        self.sock.sendto(packet, self.server_address)
    
    def send_fragmented_packet(self, request_id, timestamp_ms, request_size, fragments_needed, is_last_request):
        """Send a request fragmented into multiple UDP packets"""
        fragment_size = self.MAX_UDP_PACKET_SIZE - 24  # Accounting for headers
        remaining_size = request_size - 16  # Excluding the main header
        
        # Send each fragment
        for frag_idx in range(fragments_needed):
            is_last_fragment = frag_idx == fragments_needed - 1
            
            # Fragment header: packet_type, request_id, timestamp, total_size, fragment_idx, total_fragments
            frag_header = struct.pack('!BIQIBB', 
                                     self.FRAGMENT_PACKET,
                                     request_id, 
                                     timestamp_ms,
                                     request_size,
                                     frag_idx, 
                                     fragments_needed)
            
            # Calculate this fragment's payload size
            if is_last_fragment:
                frag_payload_size = remaining_size
            else:
                frag_payload_size = min(fragment_size, remaining_size)
            
            # Create padding for this fragment
            padding = b'X' * frag_payload_size
            
            # Combine fragment parts
            fragment = frag_header + padding
            
            # Send fragment
            self.sock.sendto(fragment, self.server_address)
            
            # Update remaining size
            remaining_size -= frag_payload_size
            
            # Small delay between fragments to avoid overwhelming the network
            if not (is_last_fragment and is_last_request):
                time.sleep(0.001)


def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='UDP Latency Measurement Client')
    parser.add_argument('server_ip', help='Server IP address')
    parser.add_argument('request_count', type=int, help='Number of requests to send')
    parser.add_argument('interval', type=int, help='Interval between requests in milliseconds')
    parser.add_argument('request_size', type=int, help='Size of each request in bytes')
    
    args = parser.parse_args()
    
    # Create client and run test
    client = UDPLatencyClient(args.server_ip)
    
    try:
        client.run_test(args.request_count, args.interval, args.request_size)
    finally:
        client.close()


if __name__ == "__main__":
    main()
