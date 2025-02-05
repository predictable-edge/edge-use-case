import socket
import argparse
import struct
import threading
from collections import defaultdict
import time

MAX_UDP_SIZE = 1300

class UETracker:
    def __init__(self, rnti_str, client_port):
        self.rnti = rnti_str
        self.client_port = client_port
        self.packets = defaultdict(lambda: {'total': 0, 'received': set()})
        self.lock = threading.Lock()

    def add_packet(self, request_id, seq_num, total_packets):
        with self.lock:
            self.packets[request_id]['total'] = total_packets
            self.packets[request_id]['received'].add(seq_num)
            is_complete = len(self.packets[request_id]['received']) == total_packets
            
            if is_complete:
                del self.packets[request_id]
            
            return is_complete

class Server:
    def __init__(self, listen_port, response_ip):
        self.listen_port = listen_port
        self.response_ip = response_ip
        
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        self.ue_trackers = {}
        self.tracker_lock = threading.Lock()

    def handle_request(self, data, client_address):
        try:
            # Unpack header
            header = struct.unpack('!IIII4sI', data[:24])
            request_id, seq_num, total_packets, response_port, rnti_bytes, latency_req = header
            rnti_str = rnti_bytes.decode().strip()
            
            client_key = (client_address[0], client_address[1])
            
            # Handle registration packet
            if request_id == 0:
                with self.tracker_lock:
                    if client_key not in self.ue_trackers:
                        print(f"Registering new UE - RNTI: {rnti_str}, Port: {response_port}")
                        self.ue_trackers[client_key] = UETracker(rnti_str, response_port)
                    
                    # Send registration acknowledgment
                    response = struct.pack('!I4s', 0, rnti_str.encode().ljust(4))
                    self.socket.sendto(response, (self.response_ip, response_port))
                return
            
            # Handle normal requests
            with self.tracker_lock:
                if client_key not in self.ue_trackers:
                    print(f"Received request from unregistered UE - RNTI: {rnti_str}")
                    return
                
                tracker = self.ue_trackers[client_key]
            
            # Process packet
            if tracker.add_packet(request_id, seq_num, total_packets):
                response = struct.pack('!I4s', request_id, rnti_str.encode().ljust(4))
                self.socket.sendto(response, (self.response_ip, tracker.client_port))
                if request_id % 100 == 0:
                    print(f"Completed request {request_id} from RNTI {rnti_str}")
                
        except Exception as e:
            print(f"Error handling request: {e}")

    def run(self):
        try:
            self.socket.bind(('', self.listen_port))
            print(f"Server listening on port {self.listen_port}")
            print(f"Sending responses to IP {self.response_ip}")
            
            while True:
                data, client_address = self.socket.recvfrom(MAX_UDP_SIZE + 100)
                threading.Thread(
                    target=self.handle_request,
                    args=(data, client_address)
                ).start()
                
        except Exception as e:
            print(f"Server error: {e}")
        finally:
            self.socket.close()

def main():
    parser = argparse.ArgumentParser(description='Simple UDP Server')
    parser.add_argument('--listen-port', type=int, default=10000,
                      help='Port to listen on (default: 10000)')
    parser.add_argument('--response-ip', type=str, required=True,
                      help='IP address to send responses to')
    
    args = parser.parse_args()
    
    server = Server(
        listen_port=args.listen_port,
        response_ip=args.response_ip
    )
    server.run()

if __name__ == "__main__":
    main() 