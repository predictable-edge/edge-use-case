import socket
import argparse
import struct
import threading

# Class to handle each client's request
class RequestHandler:
    def __init__(self, total_packets, client_address):
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
            if len(data) < 8:
                print(f"Received packet too short from {addr}. Ignoring.")
                continue

            # Unpack the first 8 bytes: 4 bytes for total_packets and 4 bytes for sequence number
            total_packets, seq_num = struct.unpack('!II', data[:8])

            # If it's a new request from this client, create a RequestHandler
            if addr not in requests:
                requests[addr] = RequestHandler(total_packets, addr)
                print(f"New request from {addr} with {total_packets} packets.")

            handler = requests[addr]
            handler.add_packet(seq_num)
            print(f"Received packet {seq_num}/{total_packets} from {addr}.")

            # If all packets are received, send a response
            if handler.is_complete():
                response = b'All packets received'
                send_socket.sendto(response, (destination_ip, destination_port))
                print(f"Sent response to {destination_ip}:{destination_port}.")
                del requests[addr]  # Remove the completed request
        except Exception as e:
            print(f"An error occurred: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Server')
    parser.add_argument('--dest-ip', type=str, required=True, help='Destination IP to send response')
    parser.add_argument('--dest-port', type=int, required=True, help='Destination port to send response')
    args = parser.parse_args()

    server_main(args.dest_ip, args.dest_port)