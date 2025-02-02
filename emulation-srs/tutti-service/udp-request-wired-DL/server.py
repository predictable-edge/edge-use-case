import socket
import argparse
import struct
import threading
from collections import defaultdict
import time

MAX_UDP_SIZE = 1400

class UETracker:
    def __init__(self, rnti, controller_ip, controller_port):
        self.rnti = rnti
        self.packets = defaultdict(lambda: {'total': 0, 'received': set()})
        self.lock = threading.Lock()
        
        # Connect to tutti controller
        self.controller_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.controller_socket.connect((controller_ip, controller_port))
            # Register as an application
            self.controller_socket.send(b"tutti_server")
            # Register this UE
            self._register_ue()
        except Exception as e:
            print(f"Failed to connect to controller: {e}")
            self.controller_socket.close()
        
        self.last_request_time = time.time()
        self.request_count = 0
    
    def _register_ue(self):
        """Register UE with the controller"""
        msg = f"NEW_UE|{self.rnti}|20|1024"  # Latency and size requirements
        self.controller_socket.send(msg.encode('utf-8'))
    
    def notify_request(self, seq_num):
        """Notify controller of new request"""
        current_time = time.time()
        self.request_count += 1
        
        # Only send updates to controller periodically to avoid overwhelming it
        if current_time - self.last_request_time >= 0.1:  # 100ms minimum interval
            msg = f"REQUEST|{self.rnti}|{seq_num}"
            try:
                self.controller_socket.send(msg.encode('utf-8'))
                self.last_request_time = current_time
                self.request_count = 0
            except Exception as e:
                print(f"Failed to notify controller: {e}")
    
    def add_packet(self, request_id, seq_num, total_packets):
        with self.lock:
            self.packets[request_id]['total'] = total_packets
            self.packets[request_id]['received'].add(seq_num)
            is_complete = len(self.packets[request_id]['received']) == total_packets
            
            if is_complete:
                del self.packets[request_id]
            
            return is_complete

class Server:
    def __init__(self, listen_port, controller_ip, controller_port):
        self.listen_port = listen_port
        self.controller_ip = controller_ip
        self.controller_port = controller_port
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ue_trackers = {}  # Map of (client_ip, client_port) -> UETracker
        self.active_ues = set()  # Track active UEs
        self.ue_last_active = {}  # Track last activity time for each UE
        self.cleanup_thread = threading.Thread(target=self._cleanup_inactive_ues, daemon=True)
        self.cleanup_thread.start()
    
    def _cleanup_inactive_ues(self):
        """Periodically clean up inactive UE trackers"""
        while True:
            current_time = time.time()
            inactive_threshold = 10.0  # 10 seconds of inactivity
            
            with threading.Lock():
                for client_key in list(self.ue_trackers.keys()):
                    if current_time - self.ue_last_active.get(client_key, 0) > inactive_threshold:
                        print(f"Removing inactive UE tracker for {client_key}")
                        tracker = self.ue_trackers.pop(client_key)
                        tracker.controller_socket.close()
                        self.ue_last_active.pop(client_key)
            
            time.sleep(5)  # Check every 5 seconds
    
    def handle_request(self, data, client_address, response_ip, response_port):
        try:
            # Unpack header: request_id, seq_num, total_packets, rnti
            header = struct.unpack('!IIII', data[:16])
            request_id, seq_num, total_packets, rnti = header
            
            # Use client address as key
            client_key = (client_address[0], client_address[1])
            self.ue_last_active[client_key] = time.time()
            
            # Get or create UE tracker
            if client_key not in self.ue_trackers:
                print(f"New UE connected - RNTI: {rnti}")
                self.ue_trackers[client_key] = UETracker(
                    rnti=rnti,
                    controller_ip=self.controller_ip,
                    controller_port=self.controller_port
                )
            
            tracker = self.ue_trackers[client_key]
            tracker.notify_request(seq_num)
            
            # Process packet
            if tracker.add_packet(request_id, seq_num, total_packets):
                # Send response when all packets received
                response = struct.pack('!II', request_id, tracker.rnti)
                self.socket.sendto(response, (response_ip, response_port))
                
        except Exception as e:
            print(f"Error handling request: {e}")

    def run(self, response_ip, response_port):
        try:
            self.socket.bind(('', self.listen_port))
            print(f"Server listening on port {self.listen_port}")
            
            while True:
                data, client_address = self.socket.recvfrom(MAX_UDP_SIZE + 100)
                thread = threading.Thread(
                    target=self.handle_request,
                    args=(data, client_address, response_ip, response_port)
                )
                thread.start()
                
        except Exception as e:
            print(f"Server error: {e}")
        finally:
            self.socket.close()
            for tracker in self.ue_trackers.values():
                tracker.controller_socket.close()

def main():
    parser = argparse.ArgumentParser(description='Tutti UDP Server')
    parser.add_argument('--listen-port', type=int, default=10000,
                      help='Port to listen on (default: 10000)')
    parser.add_argument('--response-ip', type=str, required=True,
                      help='IP to send responses to')
    parser.add_argument('--response-port', type=int, required=True,
                      help='Port to send responses to')
    parser.add_argument('--controller-ip', type=str, default='127.0.0.1',
                      help='Tutti controller IP (default: 127.0.0.1)')
    parser.add_argument('--controller-port', type=int, default=5557,
                      help='Tutti controller port (default: 5557)')
    
    args = parser.parse_args()
    
    server = Server(args.listen_port, args.controller_ip, args.controller_port)
    server.run(args.response_ip, args.response_port)

if __name__ == "__main__":
    main() 