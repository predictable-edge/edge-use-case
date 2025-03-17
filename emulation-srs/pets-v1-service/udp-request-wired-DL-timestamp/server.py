import socket
import argparse
import struct
import threading
from collections import defaultdict
import time
from queue import Queue
import concurrent.futures
import os

MAX_UDP_SIZE = 1300
HEADER_SIZE = 24    # 4 + 4 + 4 + 4 + 4 + 4 bytes

class UETracker:
    def __init__(self, rnti_str, client_port, latency_req, request_size, controller_ip, controller_port):
        """
        Initialize UE tracker
        Args:
            rnti_str: string, RNTI value in either decimal or hex format
            client_port: int, port to send responses to
            latency_req: int, latency requirement in ms
            request_size: int, total request size in bytes
            controller_ip: str, IP address of the controller
            controller_port: int, port of the controller
        """
        # Initialize timing parameters first
        self.registration_wait = 0.5  # Reduced from 2.0 to 0.5 seconds
        self.registration_time = None
        self.last_request_time = time.time()
        
        # Initialize UE parameters
        self.rnti = rnti_str  # Store RNTI as string
        self.client_port = client_port
        self.latency_req = latency_req  # Latency requirement in ms
        self.request_size = request_size  # Total request size in bytes
        
        # Initialize tracking structures
        self.packets = defaultdict(lambda: {'total': 0, 'received': set()})
        self.lock = threading.Lock()
        self.request_count = 0
        self.current_request = None
        
        # Initialize controller connection
        self.controller_ip = controller_ip
        self.controller_port = controller_port
        self.controller_socket = None
        self.registered = False  # Track if UE is registered with controller
        
        # Connect to controller
        self.connect_to_controller()
        
        # Add request tracking
        self.request_stats = {
            'total_requests': 0,
            'completed_requests': 0,
            'failed_requests': 0,
            'avg_processing_time': 0
        }
        
        # Add rate limiting
        self.last_controller_notify = 0
        self.notify_interval = 0.1  # 100ms minimum between notifications
        
        self.notify_request_complete = True  # Flag to enable DONE notifications
        
        # Add handshake and timestamp tracking
        self.timestamp_s0 = 0        # Server timestamp when receiving first handshake
        self.timestamp_c0 = 0        # Client timestamp from first handshake
        self.t1 = 0                  # RTT value from client
        self.request_timestamps = {} # Dictionary to store timestamps for each request
    
    def connect_to_controller(self):
        """Connect to the controller and register"""
        if self.controller_socket is not None:
            return  # Already connected

        try:
            # First connect and send application registration
            self.controller_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.controller_socket.connect((self.controller_ip, self.controller_port))
            self.controller_socket.send(b"tutti_server")
            time.sleep(0.1)  # Wait for registration to be processed
            
            # Then register the UE
            if not self.registered:
                self._register_ue()
                self.registered = True
                self.registration_time = time.time()
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
            # Don't sleep here, just mark registration time
            self.registration_time = time.time()
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

        # Check if enough time has passed since registration
        if self.registration_time and time.time() - self.registration_time < self.registration_wait:
            return

        # Send request notification to controller
        msg = f"REQUEST|{self.rnti}|{request_id}"
        try:
            self.controller_socket.send(msg.encode('utf-8'))
        except Exception as e:
            print(f"Failed to notify controller: {e}")
            if self.controller_socket:
                self.controller_socket.close()
            self.controller_socket = None
            self.registered = False
            self.registration_time = None

    def notify_request_completion(self, request_id):
        """Notify controller that a request is complete"""
        if not self.controller_socket or not self.registered:
            return

        try:
            msg = f"DONE|{self.rnti}|{request_id}"
            self.controller_socket.send(msg.encode('utf-8'))
        except Exception as e:
            print(f"Failed to notify request completion: {e}")
            if self.controller_socket:
                self.controller_socket.close()
            self.controller_socket = None
            self.registered = False

    def add_packet(self, request_id, seq_num, total_packets):
        with self.lock:
            # If this is a new request, store it
            if self.current_request != request_id:
                self.current_request = request_id
            
            self.packets[request_id]['total'] = total_packets
            self.packets[request_id]['received'].add(seq_num)
            is_complete = len(self.packets[request_id]['received']) == total_packets
            
            if is_complete:
                # Notify controller of request completion
                self.notify_request_completion(request_id)
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

class FileTransferHandler(threading.Thread):
    """Handle file transfer for a single client connection"""
    def __init__(self, client_socket, client_address):
        super().__init__()
        self.client_socket = client_socket
        self.client_address = client_address
        self.daemon = True

    def run(self):
        try:
            # Wait for registration info from client
            registration = self.client_socket.recv(1024).decode().strip()
            if not registration:
                print(f"Empty registration from {self.client_address}")
                return
                
            # Parse registration: RNTI|request_id|file_size_kb|latency_req
            try:
                parts = registration.split('|')
                if len(parts) < 4:
                    print(f"Invalid registration format: {registration}")
                    return
                    
                rnti = parts[0]
                request_id = int(parts[1])
                file_size_kb = int(parts[2])
                latency_req = int(parts[3])
                
                # Calculate total file size in bytes
                file_size_bytes = file_size_kb * 1024
                
                print(f"File transfer registration from RNTI {rnti}, request {request_id}, size {file_size_kb}KB")
                
                # Receive file data
                bytes_received = 0
                start_time = time.time()
                
                while bytes_received < file_size_bytes:
                    buffer_size = min(4096, file_size_bytes - bytes_received)
                    data = self.client_socket.recv(buffer_size)
                    if not data:
                        break
                    bytes_received += len(data)
                    
                end_time = time.time()
                transfer_time = (end_time - start_time) * 1000  # in ms
                
                print(f"Completed file transfer for RNTI {rnti}, request {request_id}: {bytes_received} bytes in {transfer_time:.2f}ms")
                
                # Send completion response
                response = f"DONE|{request_id}|{end_time}\n"
                self.client_socket.sendall(response.encode())
                
            except Exception as e:
                print(f"Error processing file transfer: {e}")
                self.client_socket.sendall(f"ERROR|{str(e)}\n".encode())
                
        except Exception as e:
            print(f"Error in file transfer handler: {e}")
        finally:
            self.client_socket.close()

class FileServer(threading.Thread):
    """TCP server for file transfers"""
    def __init__(self, base_port):
        super().__init__()
        self.base_port = base_port  # e.g., 20000
        self.running = True
        self.daemon = True
        
        # Create the listening socket
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(('', self.base_port))
        self.socket.listen(5)
        
        print(f"File server listening on port {self.base_port}")

    def run(self):
        try:
            while self.running:
                try:
                    client_socket, client_address = self.socket.accept()
                    print(f"Accepted file transfer connection from {client_address}")
                    
                    # Create a handler thread for this client
                    handler = FileTransferHandler(client_socket, client_address)
                    handler.start()
                    
                except Exception as e:
                    if self.running:
                        print(f"Error accepting file transfer connection: {e}")
                    time.sleep(0.1)
                    
        except Exception as e:
            print(f"Error in file server: {e}")
        finally:
            self.socket.close()
            
    def stop(self):
        self.running = False
        try:
            self.socket.close()
        except:
            pass

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
        
        # Add request queue and worker pool
        self.request_queue = Queue()
        self.num_workers = 8  # Number of worker threads
        self.worker_pool = concurrent.futures.ThreadPoolExecutor(
            max_workers=self.num_workers,
            thread_name_prefix="RequestWorker"
        )
        
        # Start worker threads
        for _ in range(self.num_workers):
            self.worker_pool.submit(self._process_request_queue)
            
        # Start file server for TCP transfers (no controller communication)
        self.file_server = FileServer(base_port=20000)  # Use port 20000 as the base port
        self.file_server.start()
    
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
    
    def _process_request_queue(self):
        """Worker thread to process requests from queue"""
        while True:
            try:
                data, client_address = self.request_queue.get()
                self.handle_request(data, client_address)
            except Exception as e:
                print(f"Error in worker thread: {e}")
            finally:
                self.request_queue.task_done()

    def handle_request(self, data, client_address):
        try:
            client_key = (client_address[0], client_address[1])
            self.ue_last_active[client_key] = time.time()
            
            # Check if this is a handshake message (which has a different format)
            if len(data) >= 12 and data[:4] == b'HSHK':
                handshake_type = struct.unpack('!I', data[4:8])[0]
                
                if handshake_type == 1:  # First handshake from client
                    # Extract client timestamp and RNTI
                    timestamp_c0, rnti_str = struct.unpack('!d4s', data[8:20])
                    rnti_str = rnti_str.decode().strip()
                    
                    # Record server timestamp
                    timestamp_s0 = time.time()
                    
                    with self.tracker_lock:
                        if client_key in self.ue_trackers:
                            tracker = self.ue_trackers[client_key]
                            tracker.timestamp_s0 = timestamp_s0
                            tracker.timestamp_c0 = timestamp_c0
                    
                    # Send response without server timestamp
                    response = struct.pack('!4sI4s', b'HSHK', 2, rnti_str.encode().ljust(4))
                    self.socket.sendto(response, (self.response_ip, client_address[1]))
                    return
                    
                elif handshake_type == 3:  # RTT message from client
                    # Extract client RTT and RNTI
                    rtt, rnti_str = struct.unpack('!d4s', data[8:20])
                    rnti_str = rnti_str.decode().strip()
                    
                    # Store RTT in tracker
                    with self.tracker_lock:
                        if client_key in self.ue_trackers:
                            tracker = self.ue_trackers[client_key]
                            tracker.t1 = rtt  # Store full RTT value
                    
                    # Acknowledge RTT received
                    response = struct.pack('!4sI4s', b'HSHK', 4, rnti_str.encode().ljust(4))
                    self.socket.sendto(response, (self.response_ip, client_address[1]))
                    return
            
            # If not a handshake, process as regular packet
            # First check if it's a packet with timestamp (32 bytes header)
            if len(data) >= 32:
                # Unpack header with timestamp: request_id, seq_num, total_packets, response_port, rnti_str, latency_req, timestamp_c1
                header = struct.unpack('!IIII4sId', data[:32])
                request_id, seq_num, total_packets, response_port, rnti_bytes, latency_req, timestamp_c1 = header
                rnti_str = rnti_bytes.decode().strip()
                
                # Record server receive timestamp
                timestamp_s1 = time.time()
                
                # Store packet timestamp
                client_key = (client_address[0], client_address[1])
                with self.tracker_lock:
                    if client_key in self.ue_trackers:
                        tracker = self.ue_trackers[client_key]
                        if request_id not in tracker.request_timestamps:
                            tracker.request_timestamps[request_id] = {}
                        tracker.request_timestamps[request_id][seq_num] = timestamp_s1
                
                # Handle registration packet (request_id = 0)
                if request_id == 0:
                    with self.tracker_lock:
                        if client_key not in self.ue_trackers:
                            print(f"Registering new UE - RNTI: {rnti_str}, Response Port: {response_port}")
                            tracker = UETracker(
                                rnti_str=rnti_str,
                                client_port=response_port,
                                latency_req=latency_req,
                                request_size=total_packets * (MAX_UDP_SIZE + 28),
                                controller_ip=self.controller_ip,
                                controller_port=self.controller_port
                            )
                            
                            if rnti_str not in self.registered_rntis:
                                tracker.connect_to_controller()
                                if tracker.registered:
                                    self.registered_rntis.add(rnti_str)
                            else:
                                tracker.registered = True
                            
                            self.ue_trackers[client_key] = tracker
                        
                        # Send registration acknowledgment
                        response = struct.pack('!I4sB', 0, rnti_str.encode().ljust(4), 0)  # 0 indicates registration response
                        self.socket.sendto(response, (self.response_ip, response_port))
                        print(f"Sent registration confirmation to RNTI {rnti_str}")
                        return
                
                # Handle normal requests
                with self.tracker_lock:
                    if client_key not in self.ue_trackers:
                        print(f"Received request from unregistered UE - RNTI: {rnti_str}")
                        return
                    
                    tracker = self.ue_trackers[client_key]
                
                # Notify controller about new request when receiving first packet (seq_num = 0)
                if seq_num == 0:
                    tracker.notify_request(request_id, seq_num)
                    
                    # Calculate transmission delay for this request
                    transmission_delay = 0
                    with self.tracker_lock:
                        if tracker.timestamp_s0 > 0 and timestamp_c1 > 0 and tracker.timestamp_c0 > 0 and tracker.t1 > 0:
                            # Use the specified formula: t2 = t1 + (timestamp_s1-timestamp_s0) - (timestamp_c1-timestamp_c0)
                            time_diff_server = timestamp_s1 - tracker.timestamp_s0
                            time_diff_client = timestamp_c1 - tracker.timestamp_c0
                            transmission_delay = tracker.t1 + (time_diff_server - time_diff_client)
                    
                    # Send response for first packet with transmission delay information
                    response = struct.pack('!I4sBd', request_id, rnti_str.encode().ljust(4), 1, transmission_delay)  # 1 indicates first packet response
                    self.socket.sendto(response, (self.response_ip, tracker.client_port))
                
                # Process packet
                if tracker.add_packet(request_id, seq_num, total_packets):
                    # Calculate transmission delay for the full request
                    transmission_delay = 0
                    with self.tracker_lock:
                        if tracker.timestamp_s0 > 0 and timestamp_c1 > 0 and tracker.timestamp_c0 > 0 and tracker.t1 > 0:
                            # Use the specified formula: t2 = t1 + (timestamp_s1-timestamp_s0) - (timestamp_c1-timestamp_c0)
                            time_diff_server = timestamp_s1 - tracker.timestamp_s0
                            time_diff_client = timestamp_c1 - tracker.timestamp_c0
                            transmission_delay = tracker.t1 + (time_diff_server - time_diff_client)
                    
                    response = struct.pack('!I4sBd', request_id, rnti_str.encode().ljust(4), 2, transmission_delay)  # 2 indicates complete request response
                    self.socket.sendto(response, (self.response_ip, tracker.client_port))
                    if request_id % 100 == 0:
                        print(f"Completed request {request_id} from RNTI {rnti_str}")
                    
                return
                
            # If we're here, this is a regular packet without timestamp (original format)
            # Unpack header: request_id, seq_num, total_packets, response_port, rnti_str, latency_req
            header = struct.unpack('!IIII4sI', data[:24])
            request_id, seq_num, total_packets, response_port, rnti_bytes, latency_req = header
            rnti_str = rnti_bytes.decode().strip()
            
            # Handle registration packet (request_id = 0)
            if request_id == 0:
                with self.tracker_lock:
                    if client_key not in self.ue_trackers:
                        print(f"Registering new UE - RNTI: {rnti_str}, Response Port: {response_port}")
                        tracker = UETracker(
                            rnti_str=rnti_str,
                            client_port=response_port,
                            latency_req=latency_req,
                            request_size=total_packets * (MAX_UDP_SIZE + 28),
                            controller_ip=self.controller_ip,
                            controller_port=self.controller_port
                        )
                        
                        if rnti_str not in self.registered_rntis:
                            tracker.connect_to_controller()
                            if tracker.registered:
                                self.registered_rntis.add(rnti_str)
                        else:
                            tracker.registered = True
                        
                        self.ue_trackers[client_key] = tracker
                    
                    # Send registration acknowledgment
                    response = struct.pack('!I4s', 0, rnti_str.encode().ljust(4))
                    self.socket.sendto(response, (self.response_ip, response_port))
                    print(f"Sent registration confirmation to RNTI {rnti_str}")
                    return
            
            # Handle normal requests
            with self.tracker_lock:
                if client_key not in self.ue_trackers:
                    print(f"Received request from unregistered UE - RNTI: {rnti_str}")
                    return
                
                tracker = self.ue_trackers[client_key]
            
            # Notify controller about new request when receiving first packet (seq_num = 0)
            if seq_num == 0:
                tracker.notify_request(request_id, seq_num)
            
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
            print(f"UDP Server listening on port {self.listen_port}")
            print(f"Sending responses to IP {self.response_ip}")
            
            while True:
                data, client_address = self.socket.recvfrom(MAX_UDP_SIZE + 100)
                # Put request in queue instead of creating new thread
                self.request_queue.put((data, client_address))
                
        except Exception as e:
            print(f"Server error: {e}")
        finally:
            self.worker_pool.shutdown(wait=True)
            self.socket.close()
            self.file_server.stop()
            for tracker in self.ue_trackers.values():
                tracker.cleanup()

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