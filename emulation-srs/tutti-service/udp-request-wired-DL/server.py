import socket
import argparse
import struct
import threading
from collections import defaultdict
import time

MAX_UDP_SIZE = 1400

class UETracker:
    def __init__(self, rnti, client_port, controller_ip, controller_port):
        self.rnti = rnti
        self.client_port = client_port
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
        self.current_request = None
    
    def _register_ue(self):
        """Register UE with the controller"""
        msg = f"NEW_UE|{self.rnti}|20|1024"  # Latency and size requirements
        self.controller_socket.send(msg.encode('utf-8'))
    
    def notify_request(self, request_id, seq_num):
        """Notify controller of new request"""
        # Only notify on the first packet of each request
        if seq_num == 0:
            current_time = time.time()
            if current_time - self.last_request_time >= 0.1:  # 100ms minimum interval
                msg = f"REQUEST|{self.rnti}|{request_id}"
                try:
                    self.controller_socket.send(msg.encode('utf-8'))
                    self.last_request_time = current_time
                except Exception as e:
                    print(f"Failed to notify controller: {e}")
    
    def add_packet(self, request_id, seq_num, total_packets):
        with self.lock:
            # If this is a new request, store it
            if self.current_request != request_id:
                self.current_request = request_id
            
            self.packets[request_id]['total'] = total_packets
            self.packets[request_id]['received'].add(seq_num)
            is_complete = len(self.packets[request_id]['received']) == total_packets
            
            if is_complete:
                del self.packets[request_id]
                self.current_request = None
            
            return is_complete

class Server:
    def __init__(self, listen_port, response_ip, controller_ip, controller_port):
        self.listen_port = listen_port
        self.response_ip = response_ip  # IP address to send responses from
        self.controller_ip = controller_ip
        self.controller_port = controller_port
        
        # Socket for receiving requests
        self.recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        # Socket for sending responses
        self.send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.send_socket.bind((self.response_ip, 0))  # Bind to response IP with random port
        
        self.ue_trackers = {}
        self.active_ues = set()
        self.ue_last_active = {}
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
    
    def handle_request(self, data, client_address):
        try:
            # Unpack header: request_id, seq_num, total_packets, rnti, response_port
            header = struct.unpack('!IIIII', data[:20])
            request_id, seq_num, total_packets, rnti, response_port = header
            
            client_key = (client_address[0], client_address[1])
            self.ue_last_active[client_key] = time.time()
            
            # Get or create UE tracker
            if client_key not in self.ue_trackers:
                print(f"New UE connected - RNTI: {rnti}, Response Port: {response_port}")
                self.ue_trackers[client_key] = UETracker(
                    rnti=rnti,
                    client_port=response_port,
                    controller_ip=self.controller_ip,
                    controller_port=self.controller_port
                )
            
            tracker = self.ue_trackers[client_key]
            tracker.notify_request(request_id, seq_num)
            
            # Process packet
            if tracker.add_packet(request_id, seq_num, total_packets):
                # Send response when all packets received
                response = struct.pack('!II', request_id, tracker.rnti)
                self.send_socket.sendto(response, (client_address[0], tracker.client_port))
                print(f"Completed request {request_id} from RNTI {rnti} ({total_packets} packets)")
                
        except Exception as e:
            print(f"Error handling request: {e}")

    def run(self):
        try:
            self.recv_socket.bind(('', self.listen_port))
            print(f"Server listening on port {self.listen_port}")
            print(f"Sending responses from IP {self.response_ip}")
            
            while True:
                data, client_address = self.recv_socket.recvfrom(MAX_UDP_SIZE + 100)
                thread = threading.Thread(
                    target=self.handle_request,
                    args=(data, client_address)
                )
                thread.start()
                
        except Exception as e:
            print(f"Server error: {e}")
        finally:
            self.recv_socket.close()
            self.send_socket.close()
            for tracker in self.ue_trackers.values():
                tracker.controller_socket.close()

def main():
    parser = argparse.ArgumentParser(description='Tutti UDP Server')
    parser.add_argument('--listen-port', type=int, default=10000,
                      help='Port to listen on (default: 10000)')
    parser.add_argument('--response-ip', type=str, required=True,
                      help='IP address to send responses from')
    parser.add_argument('--controller-ip', type=str, default='127.0.0.1',
                      help='Tutti controller IP (default: 127.0.0.1)')
    parser.add_argument('--controller-port', type=int, default=5557,
                      help='Tutti controller port (default: 5557)')
    
    args = parser.parse_args()
    
    server = Server(
        listen_port=args.listen_port,
        response_ip=args.response_ip,
        controller_ip=args.controller_ip,
        controller_port=args.controller_port
    )
    server.run()

if __name__ == "__main__":
    main() 