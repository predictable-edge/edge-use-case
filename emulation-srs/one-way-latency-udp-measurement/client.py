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
    
    def close(self):
        """Close the socket"""
        if self.sock:
            self.sock.close()
            self.sock = None
    
    def run_test(self, request_count, interval_ms, request_size):
        """Run the latency test with the given parameters"""
        # Validate arguments
        if request_size < 16:
            print("Request size must be at least 16 bytes to hold header information")
            return False
        
        # Calculate fragments needed if size is larger than max UDP packet
        fragment_size = self.MAX_UDP_PACKET_SIZE - 24  # Accounting for headers
        fragments_needed = math.ceil(request_size / fragment_size)
        
        if fragments_needed > 1:
            print(f"Request size exceeds maximum UDP packet size. Splitting into {fragments_needed} fragments.")
        
        # Initialize socket if not already done
        if not self.sock:
            self.initialize_socket()
        
        print(f"Starting UDP latency test:")
        print(f"Server: {self.server_ip}:{self.server_port}")
        print(f"Request count: {request_count}")
        print(f"Interval: {interval_ms} ms")
        print(f"Request size: {request_size} bytes")
        
        try:
            for i in range(request_count):
                request_id = i + 1
                timestamp_ms = int(time.time() * 1000)  # Current time in milliseconds
                is_last_request = i == request_count - 1
                
                if fragments_needed <= 1:
                    self.send_single_packet(request_id, timestamp_ms, request_size, is_last_request, request_count)
                else:
                    self.send_fragmented_packet(request_id, timestamp_ms, request_size, fragments_needed, 
                                               is_last_request, request_count)
                
                # Wait for the specified interval unless it's the last packet
                if not is_last_request:
                    time.sleep(interval_ms / 1000)
            
            print(f"Test completed. Sent {request_count} requests.")
            return True
            
        except Exception as e:
            print(f"Error during test: {e}")
            return False
    
    def send_single_packet(self, request_id, timestamp_ms, request_size, is_last_request, request_count):
        """Send a single UDP packet"""
        # Prepare header: request_id, timestamp, request_size
        header = struct.pack('!IQI', request_id, timestamp_ms, request_size)
        
        # Add marker for last packet
        if is_last_request:
            footer = b"FINAL" + struct.pack('!I', request_count)
        else:
            footer = b""
            
        # Calculate padding size
        padding_size = request_size - len(header) - len(footer)
        padding = b'X' * max(0, padding_size)
        
        # Combine all parts
        packet = header + footer + padding
        
        # Send packet
        self.sock.sendto(packet, self.server_address)
        print(f"Sent request {request_id}, size: {len(packet)} bytes")
    
    def send_fragmented_packet(self, request_id, timestamp_ms, request_size, fragments_needed, is_last_request, request_count):
        """Send a request fragmented into multiple UDP packets"""
        fragment_size = self.MAX_UDP_PACKET_SIZE - 24  # Accounting for headers
        remaining_size = request_size - 16  # Excluding the main header
        
        # Send each fragment
        for frag_idx in range(fragments_needed):
            is_last_fragment = frag_idx == fragments_needed - 1
            
            # Fragment header: request_id, timestamp, total_size, fragment_idx, total_fragments
            frag_header = struct.pack('!IQIIB', 
                                     request_id, 
                                     timestamp_ms,
                                     request_size,
                                     frag_idx, 
                                     fragments_needed)
            
            # Add marker for the last fragment of the last request
            if is_last_fragment and is_last_request:
                footer = b"FINAL" + struct.pack('!I', request_count)
            else:
                footer = b""
            
            # Calculate this fragment's payload size
            if is_last_fragment:
                frag_payload_size = remaining_size
            else:
                frag_payload_size = min(fragment_size, remaining_size)
            
            # Create padding for this fragment
            padding = b'X' * frag_payload_size
            
            # Combine fragment parts
            fragment = frag_header + footer + padding
            
            # Send fragment
            self.sock.sendto(fragment, self.server_address)
            
            print(f"Sent request {request_id}, fragment {frag_idx+1}/{fragments_needed}, size: {len(fragment)} bytes")
            
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
