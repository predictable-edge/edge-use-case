import socket
import argparse
import struct
import threading

# Class to handle each client's request
class RequestHandler:
    def __init__(self, request_id, total_packets, client_address):
        self.request_id = request_id
        self.total_packets = total_packets
        self.client_address = client_address
        self.received_packets = set()
        self.lock = threading.Lock()

    def add_packet(self, seq_num):
        with self.lock:
            self.received_packets.add(seq_num)

    def is_complete(self):
        with self.lock:
            return len(self.received_packets) == self.total_packets

def server_main(destination_ip, destination_port):
    # Create a UDP socket for receiving data
    recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    recv_socket.bind(('', 10000))
    print("Server listening on port 10000 for incoming UDP packets.")

    # Create a UDP socket for sending responses
    send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_socket.bind(('', 10001))
    print(f"Server ready to send responses from port 10001 to {destination_ip}:{destination_port}.")

    # Dictionary to keep track of client requests
    requests = {}

    while True:
        try:
            data, addr = recv_socket.recvfrom(2048)  # Buffer size is 2048 bytes
            if len(data) < 12:
                print(f"Received packet too short from {addr}. Ignoring.")
                continue

            # Unpack the first 12 bytes: 4 bytes for request_id, 4 bytes for total_packets, 4 bytes for sequence number
            request_id, total_packets, seq_num = struct.unpack('!III', data[:12])

            # Use (addr, request_id) as the key to handle multiple requests from the same client
            key = (addr, request_id)

            # If it's a new request from this client, create a RequestHandler
            if key not in requests:
                requests[key] = RequestHandler(request_id, total_packets, addr)
                print(f"New request {request_id} from {addr} with {total_packets} packets.")

            handler = requests[key]
            handler.add_packet(seq_num)
            print(f"Received packet {seq_num}/{total_packets} for request {request_id} from {addr}.")

            # If all packets are received, send a response including the request_id
            if handler.is_complete():
                # Create response packet with request_id
                response = struct.pack('!I', request_id)
                send_socket.sendto(response, (destination_ip, destination_port))
                print(f"Sent response for request {request_id} to {destination_ip}:{destination_port}.")
                del requests[key]  # Remove the completed request
        except Exception as e:
            print(f"An error occurred: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Server')
    parser.add_argument('--dest-ip', type=str, required=True, help='Destination IP to send response')
    parser.add_argument('--dest-port', type=int, required=True, help='Destination port to send response')
    args = parser.parse_args()

    server_main(args.dest_ip, args.dest_port)