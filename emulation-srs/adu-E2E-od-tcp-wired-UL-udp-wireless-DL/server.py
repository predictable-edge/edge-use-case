import socket
import argparse
import struct
import threading

def recv_exact(sock, n):
    """Helper function to receive exact number of bytes"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)

class RequestHandler:
    def __init__(self, request_id, total_bytes, client_address):
        self.request_id = request_id
        self.total_bytes = total_bytes
        self.bytes_received = 0
        self.client_address = client_address
        self.lock = threading.Lock()

    def add_bytes(self, num_bytes):
        with self.lock:
            self.bytes_received += num_bytes

    def is_complete(self):
        with self.lock:
            return self.bytes_received >= self.total_bytes

def handle_client_connection(client_socket, client_address, destination_ip, destination_port):
    """Handle a single client connection"""
    print(f"New connection from {client_address}")
    
    # Create a UDP socket for sending responses
    send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"Created UDP socket for responses to {destination_ip}:{destination_port}")

    current_request = None
    
    try:
        while True:
            # First read the header (8 bytes: request_id and total_bytes)
            header_data = recv_exact(client_socket, 8)
            if not header_data:
                break
                
            request_id, total_bytes = struct.unpack('!II', header_data)
            
            current_request = RequestHandler(request_id, total_bytes, client_address)
            
            # Read the request data
            bytes_remaining = total_bytes
            while bytes_remaining > 0:
                buffer_size = min(4096, bytes_remaining)
                data = recv_exact(client_socket, buffer_size)
                if not data:
                    raise Exception("Connection closed before receiving all data")
                
                current_request.add_bytes(len(data))
                bytes_remaining -= len(data)

            # Send a small response packet with just the request_id via UDP
            try:
                response = struct.pack('!I', request_id)
                send_socket.sendto(response, (destination_ip, destination_port))
                print(f"Sent UDP response for request {request_id}")
            except Exception as e:
                print(f"Error sending response: {e}")
                continue

    except Exception as e:
        print(f"Error handling client: {e}")
    finally:
        print(f"Closing connection from {client_address}")
        client_socket.close()
        send_socket.close()

def server_main(destination_ip, destination_port):
    """Main server function"""
    # Create TCP socket for receiving data
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('', 10000))
        server_socket.listen(5)
        print("Server listening on port 10000 for incoming TCP connections.")

        while True:
            client_socket, client_address = server_socket.accept()
            # Start a new thread for each client
            client_thread = threading.Thread(
                target=handle_client_connection,
                args=(client_socket, client_address, destination_ip, destination_port)
            )
            client_thread.start()

    except Exception as e:
        print(f"Server error: {e}")
    finally:
        server_socket.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='TCP Server')
    parser.add_argument('--dest-ip', type=str, required=True, help='Destination IP to send response')
    parser.add_argument('--dest-port', type=int, required=True, help='Destination port to send response')
    args = parser.parse_args()

    server_main(args.dest_ip, args.dest_port)