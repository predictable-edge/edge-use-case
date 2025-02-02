import socket
import argparse
import struct
import threading
from collections import defaultdict
import time

MAX_UDP_SIZE = 1400

class UETracker:
    def __init__(self, rnti, client_port, latency_req, request_size, controller_ip, controller_port):
        self.rnti = rnti
        self.client_port = client_port
        self.latency_req = latency_req  # Latency requirement in ms
        self.request_size = request_size  # Total request size in bytes
        self.packets = defaultdict(lambda: {'total': 0, 'received': set()})
        self.lock = threading.Lock()
        
        self.controller_ip = controller_ip
        self.controller_port = controller_port
        self.controller_socket = None
        self.registered = False  # Track if UE is registered with controller
        self.connect_to_controller()
        
        self.last_request_time = time.time()
        self.request_count = 0
        self.current_request = None
        self.registration_time = None  # Track when UE was registered
        self.registration_wait = 2.0  # Wait 2 seconds after registration
    
    def connect_to_controller(self):
        """Connect to the controller and register"""
        if self.controller_socket is not None:
            return  # Already connected

        try:
            self.controller_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.controller_socket.connect((self.controller_ip, self.controller_port))
            self.controller_socket.send(b"tutti_server")
            if not self.registered:
                self._register_ue()
                self.registered = True
                self.registration_time = time.time()  # Record registration time
        except Exception as e:
            print(f"Failed to connect to controller: {e}")
            if self.controller_socket:
                self.controller_socket.close()
            self.controller_socket = None

    def _register_ue(self):
        """Register UE with the controller"""
        if not self.controller_socket:
            return
            
        # Format: NEW_UE|rnti|ue_idx|latency|size
        msg = f"NEW_UE|{self.rnti}|0|{self.latency_req}|{self.request_size}"
        try:
            self.controller_socket.send(msg.encode('utf-8'))
            time.sleep(self.registration_wait)  # Wait for controller to process
        except Exception as e:
            print(f"Failed to register UE: {e}")
            self.controller_socket.close()
            self.controller_socket = None
            self.registered = False

    def notify_request(self, request_id, seq_num):
        """Notify controller of new request"""
        if not self.controller_socket and not self.registered:
            self.connect_to_controller()
            if not self.controller_socket:
                return

        # Wait for registration to be processed by controller
        if self.registration_time and time.time() - self.registration_time < self.registration_wait:
            return

        if seq_num == 0:
            current_time = time.time()
            if current_time - self.last_request_time >= 0.1:
                msg = f"REQUEST|{self.rnti}|{request_id}"
                try:
                    self.controller_socket.send(msg.encode('utf-8'))
                    self.last_request_time = current_time
                except Exception as e:
                    print(f"Failed to notify controller: {e}")
                    if self.controller_socket:
                        self.controller_socket.close()
                    self.controller_socket = None
                    self.registered = False
                    self.registration_time = None

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

    def cleanup(self):
        """Clean up resources"""
        if self.controller_socket:
            try:
                self.controller_socket.close()
            except:
                pass
            self.controller_socket = None
            self.registered = False

class Server:
    def __init__(self, listen_port, response_ip, controller_ip, controller_port):
        self.listen_port = listen_port
        self.response_ip = response_ip  # IP address to send responses to
        self.controller_ip = controller_ip
        self.controller_port = controller_port
        
        # Socket for receiving requests and sending responses
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        self.registered_rntis = set()  # Track which RNTIs have been registered
        self.ue_trackers = {}
        self.active_ues = set()
        self.ue_last_active = {}
        self.tracker_lock = threading.Lock()  # Add lock for thread safety
        self.cleanup_thread = threading.Thread(target=self._cleanup_inactive_ues, daemon=True)
        self.cleanup_thread.start()
    
    def _cleanup_inactive_ues(self):
        """Periodically clean up inactive UE trackers"""
        while True:
            current_time = time.time()
            inactive_threshold = 10.0  # 10 seconds of inactivity
            
            with self.tracker_lock:
                for client_key in list(self.ue_trackers.keys()):
                    if current_time - self.ue_last_active.get(client_key, 0) > inactive_threshold:
                        print(f"Removing inactive UE tracker for {client_key}")
                        tracker = self.ue_trackers.pop(client_key)
                        tracker.cleanup()  # Use the cleanup method
                        self.ue_last_active.pop(client_key)
                        # Don't remove from registered_rntis to prevent re-registration
            
            time.sleep(5)  # Check every 5 seconds
    
    def handle_request(self, data, client_address):
        try:
            # Unpack header: request_id, seq_num, total_packets, rnti, response_port, latency_req
            header = struct.unpack('!IIIIII', data[:24])
            request_id, seq_num, total_packets, rnti, response_port, latency_req = header
            
            client_key = (client_address[0], client_address[1])
            self.ue_last_active[client_key] = time.time()
            
            # Calculate total request size based on number of packets
            payload_size = MAX_UDP_SIZE - 24  # Header size is 24 bytes
            request_size = total_packets * payload_size
            
            # Handle registration packet (request_id = 0)
            if request_id == 0:
                with self.tracker_lock:
                    if client_key not in self.ue_trackers:
                        print(f"Registering new UE - RNTI: {rnti}, Response Port: {response_port}, Latency Req: {latency_req}ms")
                        tracker = UETracker(
                            rnti=rnti,
                            client_port=response_port,
                            latency_req=latency_req,
                            request_size=request_size,
                            controller_ip=self.controller_ip,
                            controller_port=self.controller_port
                        )
                        
                        if rnti not in self.registered_rntis:
                            tracker.connect_to_controller()
                            if tracker.registered:
                                self.registered_rntis.add(rnti)
                        else:
                            tracker.registered = True
                        
                        self.ue_trackers[client_key] = tracker
                    
                    # Send registration acknowledgment
                    response = struct.pack('!II', 0, rnti)
                    self.socket.sendto(response, (self.response_ip, response_port))
                    return
            
            # Handle normal requests
            with self.tracker_lock:
                if client_key not in self.ue_trackers:
                    print(f"Received request from unregistered UE - RNTI: {rnti}")
                    return
                
                tracker = self.ue_trackers[client_key]
            
            # Only notify if already registered
            if tracker.registered:
                tracker.notify_request(request_id, seq_num)
            
            # Process packet
            if tracker.add_packet(request_id, seq_num, total_packets):
                response = struct.pack('!II', request_id, tracker.rnti)
                self.socket.sendto(response, (self.response_ip, tracker.client_port))
                print(f"Completed request {request_id} from RNTI {rnti} ({total_packets} packets)")
                
        except Exception as e:
            print(f"Error handling request: {e}")

    def run(self):
        try:
            self.socket.bind(('', self.listen_port))
            print(f"Server listening on port {self.listen_port}")
            print(f"Sending responses to IP {self.response_ip}")
            
            while True:
                data, client_address = self.socket.recvfrom(MAX_UDP_SIZE + 100)
                thread = threading.Thread(
                    target=self.handle_request,
                    args=(data, client_address)
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
                      help='IP address to send responses to')
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