import socket

def recv_all(conn, length):
    """Receive exactly 'length' bytes from the socket."""
    data = b''
    while len(data) < length:
        packet = conn.recv(length - len(data))
        if not packet:
            # Connection closed or no data received
            return None
        data += packet
    return data

def start_server(port=10000):
    """Starts the TCP server on the specified port."""
    # Create a TCP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Bind the socket to all available interfaces on the specified port
    server_socket.bind(('', port))
    # Listen for incoming connections
    server_socket.listen(1)
    print(f"Server listening on port {port}")
    
    while True:
        # Accept a connection from the client
        conn, addr = server_socket.accept()
        print(f"Connected by {addr}")

        # Dictionary to keep track of received packets for each request
        requests = {}
        
        while True:
            # Receive data packet from client
            data = recv_all(conn, 1400)
            if data is None:
                print("Connection closed by client.")
                break  # Exit the inner loop and wait for a new connection
            # Unpack data packet fields
            request_index = int.from_bytes(data[0:4], byteorder='big')
            total_packets = int.from_bytes(data[4:8], byteorder='big')
            sequence_number = int.from_bytes(data[8:12], byteorder='big')
            
            # Initialize the set for the request index if not present
            if request_index not in requests:
                requests[request_index] = set()
            # Add the sequence number to the set
            requests[request_index].add(sequence_number)
            
            # Check if all packets for the request have been received
            if len(requests[request_index]) == total_packets:
                # Send response to client
                response = b'OK'
                conn.sendall(response)
                print(f"Responded to request {request_index}")
                # Remove the request from the dictionary
                del requests[request_index]

        # Close the connection
        conn.close()
        print(f"Connection with {addr} closed.\nWaiting for new connection...")

if __name__ == '__main__':
    start_server()