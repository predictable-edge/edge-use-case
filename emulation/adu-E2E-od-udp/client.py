import socket
import argparse
import struct
import time
import threading

def send_packets(server_ip, server_port, num_packets, interval_ms, send_socket):
    """
    Function to send UDP packets to the server.
    Each packet contains the total number of packets and a sequence number.
    """
    for seq in range(1, num_packets + 1):
        # Pack total_packets and sequence number into the first 8 bytes
        header = struct.pack('!II', num_packets, seq)
        # Fill the rest of the packet to make it 1500 bytes
        payload = header + b'\0' * (1500 - 8)
        try:
            send_socket.sendto(payload, (server_ip, server_port))
            print(f"Sent packet {seq}/{num_packets} to {server_ip}:{server_port}")
        except Exception as e:
            print(f"Failed to send packet {seq}: {e}")
        time.sleep(interval_ms / 1000.0)  # Sleep for the specified interval

def receive_response(listen_socket, latencies, num_packets):
    """
    Function to receive the server's response and calculate latency.
    """
    while True:
        try:
            data, addr = listen_socket.recvfrom(1024)
            if data:
                receive_time = time.time()
                latencies.append(receive_time)
                print(f"Received response from {addr} at {receive_time}")
                break  # Assuming one response per request
        except Exception as e:
            print(f"Failed to receive response: {e}")

def client_main(server_ip, server_port, listen_port, num_packets, interval_ms):
    """
    Main function to handle sending and receiving UDP packets.
    """
    latencies = []
    
    # Create a UDP socket for sending packets (ue1)
    send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_socket.bind(('', 0))  # Bind to an ephemeral port
    send_port = send_socket.getsockname()[1]
    print(f"Sending from local port {send_port}.")

    # Create a UDP socket for receiving responses (ue2)
    listen_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listen_socket.bind(('', listen_port))
    print(f"Listening for responses on port {listen_port}.")

    # Start the receive thread
    recv_thread = threading.Thread(target=receive_response, args=(listen_socket, latencies, num_packets))
    recv_thread.start()

    # Start sending packets
    send_packets(server_ip, server_port, num_packets, interval_ms, send_socket)

    # Wait for the receive thread to finish
    recv_thread.join()

    # Calculate and print latency if a response was received
    if latencies:
        send_time = time.time()  # Assuming send time is approximately the last packet send time
        latency = latencies[0] - send_time
        latency_ms = latency * 1000
        print(f"Latency: {latency_ms:.2f} ms")
    else:
        print("No response received.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Client')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-packets', type=int, required=True, help='Number of data packets per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between packets in milliseconds')
    args = parser.parse_args()

    client_main(args.server_ip, args.server_port, args.listen_port, args.num_packets, args.interval)