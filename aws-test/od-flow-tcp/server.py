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
    # Allow address reuse
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Bind the socket to all available interfaces on the specified port
    server_socket.bind(('', port))
    # Listen for incoming connections
    server_socket.listen(1)
    print(f"Server listening on port {port}")
    
    while True:
        # Accept a connection from the client
        conn, addr = server_socket.accept()
        print(f"Connected by {addr}")

        # First, receive 4 bytes indicating the payload size
        header = recv_all(conn, 4)
        if header is None:
            print("Failed to receive payload size. Connection closed.")
            conn.close()
            continue

        # Unpack payload size (unsigned int, big endian)
        payload_size = int.from_bytes(header, byteorder='big')
        print(f"Expecting payload of size: {payload_size} bytes")

        while True:
            # Receive the payload data
            data = recv_all(conn, payload_size)
            if data is None:
                print("Connection closed by client.")
                break  # Exit the inner loop and wait for a new connection

            # Here you can process the received data as needed
            # For this example, we simply acknowledge receipt

            # Send 4-byte 'OKAY' response to client
            response = b'OKAY'
            try:
                conn.sendall(response)
                print(f"Responded with 'OKAY' for payload of size {payload_size}")
            except BrokenPipeError:
                print("Failed to send response. Client may have closed the connection.")
                break

        # Close the connection
        conn.close()
        print(f"Connection with {addr} closed.\nWaiting for new connection...")

if __name__ == '__main__':
    start_server()