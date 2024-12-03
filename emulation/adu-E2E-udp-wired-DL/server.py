import socket
import argparse
import struct
import threading

MAX_UDP_SIZE = 1400  # Maximum UDP payload size

def handle_client_connection(client_address, request_data, destination_ip, destination_port):
    """Handle a single request"""
    request_id, = struct.unpack('!I', request_data[:4])
    print(f"Received request {request_id} from {client_address}")

    # Create UDP socket for response
    send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    try:
        # Send response with request_id
        response = struct.pack('!I', request_id)
        send_socket.sendto(response, (destination_ip, destination_port))
        print(f"Sent response for request {request_id}")
    except Exception as e:
        print(f"Error sending response: {e}")
    finally:
        send_socket.close()

def server_main(listen_port, dest_ip, dest_port):
    """Main server function"""
    # Create UDP socket for receiving data
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('', listen_port))
        print(f"Server listening on port {listen_port} for incoming UDP packets.")

        while True:
            try:
                data, client_address = server_socket.recvfrom(MAX_UDP_SIZE + 100)  # Extra space for headers
                # Start a new thread for handling the response
                client_thread = threading.Thread(
                    target=handle_client_connection,
                    args=(client_address, data, dest_ip, dest_port)
                )
                client_thread.start()
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