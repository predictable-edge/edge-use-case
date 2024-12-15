import socket
import argparse
import struct
import threading
from collections import defaultdict

MAX_UDP_SIZE = 1400

class PacketTracker:
    def __init__(self):
        self.packets = {}
        self.lock = threading.Lock()
    
    def add_packet(self, request_id, seq_num, total_packets):
        with self.lock:
            if request_id not in self.packets:
                self.packets[request_id] = {'total': total_packets, 'received': set()}
            self.packets[request_id]['received'].add(seq_num)
            return len(self.packets[request_id]['received']) == self.packets[request_id]['total']

def handle_client_request(server_socket, data, client_address, destination_ip, destination_port, packet_tracker):
    try:
        request_id, seq_num, total_packets = struct.unpack('!III', data[:12])
        all_received = packet_tracker.add_packet(request_id, seq_num, total_packets)
        
        if all_received:
            response = struct.pack('!I', request_id)
            server_socket.sendto(response, (destination_ip, destination_port))
            with packet_tracker.lock:
                del packet_tracker.packets[request_id]

    except Exception as e:
        print(f"Error handling request: {e}")

def server_main(listen_port, dest_ip, dest_port):
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    packet_tracker = PacketTracker()
    
    try:
        server_socket.bind(('', listen_port))
        print(f"Server listening on port {listen_port}")

        while True:
            try:
                data, client_address = server_socket.recvfrom(MAX_UDP_SIZE + 100)
                thread = threading.Thread(
                    target=handle_client_request,
                    args=(server_socket, data, client_address, dest_ip, dest_port, packet_tracker)
                )
                thread.start()
            except Exception as e:
                print(f"Error receiving data: {e}")

    except Exception as e:
        print(f"Server error: {e}")
    finally:
        server_socket.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Server')
    parser.add_argument('--listen-port', type=int, default=10000, help='Port to listen on (default: 10000)')
    parser.add_argument('--dest-ip', type=str, required=True, help='Destination IP to send response')
    parser.add_argument('--dest-port', type=int, required=True, help='Destination port to send response')
    args = parser.parse_args()

    server_main(args.listen_port, args.dest_ip, args.dest_port)