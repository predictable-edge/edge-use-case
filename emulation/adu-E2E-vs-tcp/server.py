import socket
import argparse
import struct
import threading
import time

def handle_client_connection(client_socket, client_address, destination_ip, destination_port):
    """Handle a single client connection"""
    print(f"New connection from {client_address}")
    
    # Create a socket for sending responses
    response_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        response_socket.connect((destination_ip, destination_port))
        print(f"Response connection established to {destination_ip}:{destination_port}")
    except Exception as e:
        print(f"Failed to establish response connection: {e}")
        client_socket.close()
        return

    try:
        while True:
            # First read the header (12 bytes: request_id, bytes_size)
            header_data = recv_all(client_socket, 8)
            if not header_data:
                break
                
            request_id, bytes_size = struct.unpack('!II', header_data)
            print(f"Receiving request {request_id} with {bytes_size} bytes")

            # Read the full request data
            request_data = recv_all(client_socket, bytes_size)
            if not request_data:
                break

            print(f"Received complete request {request_id}, sending response...")

            # Send response with same format and size
            try:
                # Send header first
                response_header = struct.pack('!II', request_id, bytes_size)
                response_socket.sendall(response_header)
                
                # Send response data
                response_data = b'\0' * bytes_size  # Fill with zeros
                response_socket.sendall(response_data)

                print(f"Sent response for request {request_id}")
            except Exception as e:
                print(f"Error sending response: {e}")
                break

    except Exception as e:
        print(f"Error handling client connection: {e}")
    finally:
        print(f"Closing connection from {client_address}")
        client_socket.close()
        response_socket.close()

def recv_all(sock, n):
    """Helper function to receive exact number of bytes"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)

def server_main(listen_port, dest_ip, dest_port):
    """Main server function"""
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('', listen_port))
        server_socket.listen(5)
        print(f"Server listening on port {listen_port}")

        while True:
            client_socket, client_address = server_socket.accept()
            # Start a new thread for each client
            client_thread = threading.Thread(
                target=handle_client_connection,
                args=(client_socket, client_address, dest_ip, dest_port)
            )
            client_thread.start()

    except Exception as e:
        print(f"Server error: {e}")
    finally:
        server_socket.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='TCP Server')
    parser.add_argument('--listen-port', type=int, default=10000, help='Port to listen on (default: 10000)')
    parser.add_argument('--dest-ip', type=str, required=True, help='Destination IP to send response')
    parser.add_argument('--dest-port', type=int, required=True, help='Destination port to send response')
    args = parser.parse_args()

    server_main(args.listen_port, args.dest_ip, args.dest_port)