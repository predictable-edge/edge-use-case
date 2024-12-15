import socket
import argparse
import struct
import threading

MAX_UDP_SIZE = 1400  # Maximum UDP payload size

def handle_client_request(server_socket, data, client_address, destination_ip, destination_port):
    """Handle a single request and send response"""
    try:
        # Extract request_id from the first 4 bytes
        request_id, = struct.unpack('!I', data[:4])
        print(f"Received request {request_id} from {client_address}")

        # Send response with request_id
        response = struct.pack('!I', request_id)
        server_socket.sendto(response, (destination_ip, destination_port))
        print(f"Sent response for request {request_id}")

    except Exception as e:
        print(f"Error handling request: {e}")

def server_main(listen_port, dest_ip, dest_port):
    """Main server function"""
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('', listen_port))
        print(f"Server listening on port {listen_port} for incoming UDP packets")

        while True:
            try:
                data, client_address = server_socket.recvfrom(MAX_UDP_SIZE + 100)
                # Start a new thread for handling the response
                thread = threading.Thread(
                    target=handle_client_request,
                    args=(server_socket, data, client_address, dest_ip, dest_port)
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