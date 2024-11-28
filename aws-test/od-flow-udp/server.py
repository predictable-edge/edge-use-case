import socket
import threading
import traceback

def recv_all(data, expected_length):
    """
    Checks if the received data is at least of the expected length.
    For UDP, each recvfrom call retrieves one datagram.
    """
    return len(data) >= expected_length

def handle_datagram(data, client_address, server_socket, clients_lock, clients_data):
    """
    Handles an incoming UDP datagram from a client.
    """
    if not recv_all(data, 12):
        print(f"Received incomplete packet from {client_address}. Ignoring.")
        return

    # Unpack data packet fields
    request_index = int.from_bytes(data[0:4], byteorder='big')
    total_packets = int.from_bytes(data[4:8], byteorder='big')
    sequence_number = int.from_bytes(data[8:12], byteorder='big')

    client_ip = client_address[0]  # Use only IP address as key

    with clients_lock:
        if client_ip not in clients_data:
            clients_data[client_ip] = {}
        client_requests = clients_data[client_ip]

        if request_index not in client_requests:
            client_requests[request_index] = set()

        client_requests[request_index].add(sequence_number)

        # Check if all packets for the request have been received
        if len(client_requests[request_index]) == total_packets:
            # Send response back to client with the request_index
            response = request_index.to_bytes(4, byteorder='big')
            server_socket.sendto(response, client_address)
            # Remove the completed request
            del client_requests[request_index]


def start_server(port=10050):
    """Starts the UDP server on the specified port."""
    # Create a UDP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Bind the socket to all available interfaces on the specified port
    server_socket.bind(('', port))
    print(f"UDP Server listening on port {port}")

    # Dictionary to keep track of received packets for each client and request
    # Structure: { client_ip: { request_index: set(sequence_numbers) } }
    clients_data = {}
    clients_lock = threading.Lock()

    while True:
        try:
            # Receive data from client
            data, client_address = server_socket.recvfrom(2048)  # Buffer size is 2048 bytes
            # Handle the received datagram in a separate thread
            threading.Thread(target=handle_datagram, args=(data, client_address, server_socket, clients_lock, clients_data)).start()
        except KeyboardInterrupt:
            print("\nServer shutting down.")
            break
        except Exception as e:
            print(f"An error occurred: {e}")
            traceback.print_exc()

    # Close the socket
    server_socket.close()

if __name__ == '__main__':
    start_server()