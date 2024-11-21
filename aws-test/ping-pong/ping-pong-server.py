import socket

def run_server(host="0.0.0.0", port=10050):
    """
    Runs the UDP server to listen for messages and echo them back to the client.

    Args:
        host (str): The IP address to bind the server to (default: "0.0.0.0").
        port (int): The UDP port to bind the server to (default: 10050).
    """
    # Create a UDP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.bind((host, port))
    
    print(f"UDP server is running on {host}:{port}...")
    
    while True:
        # Wait for a message from the client
        data, client_address = server_socket.recvfrom(1024)  # Buffer size: 1024 bytes
        print(f"Received message from {client_address}: {data.decode()}")
        
        # Echo the message back to the client
        server_socket.sendto(data, client_address)
        print(f"Echoed message back to {client_address}")

if __name__ == "__main__":
    run_server()