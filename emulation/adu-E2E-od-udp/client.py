import socket
import argparse
import struct
import time
import os
import ctypes
import json

# Load libc for setns
libc = ctypes.CDLL('libc.so.6', use_errno=True)

def setns(fd, nstype):
    """Set the network namespace to the one represented by fd."""
    if libc.setns(fd, nstype) != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, os.strerror(errno))

def enter_netns(namespace_name):
    """Enter the network namespace with the given name."""
    netns_path = f'/var/run/netns/{namespace_name}'
    fd = os.open(netns_path, os.O_RDONLY)
    setns(fd, 0)  # CLONE_NEWNET is 0
    os.close(fd)

def send_requests(server_ip, server_port, num_requests, num_packets, interval_ms):
    """
    Function to send UDP requests to the server.
    Each request consists of multiple packets sent without intervals.
    """
    # Create a UDP socket for sending packets
    send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_socket.bind(('', 0))  # Bind to an ephemeral port
    send_port = send_socket.getsockname()[1]
    print(f"Sending from local port {send_port}.")

    send_times = {}

    for request_id in range(1, num_requests + 1):
        # Record the send time for this request
        send_times[request_id] = time.time()
        print(f"Sending request {request_id}.")

        for seq_num in range(1, num_packets + 1):
            # Pack request_id, total_packets, and sequence number into the first 12 bytes
            header = struct.pack('!III', request_id, num_packets, seq_num)
            # Fill the rest of the packet to make it 1500 bytes
            payload = header + b'\0' * (1500 - 12)
            try:
                send_socket.sendto(payload, (server_ip, server_port))
                print(f"Sent packet {seq_num}/{num_packets} for request {request_id} to {server_ip}:{server_port}")
            except Exception as e:
                print(f"Failed to send packet {seq_num} for request {request_id}: {e}")

        # Wait for interval before sending the next request
        if request_id != num_requests:
            time.sleep(interval_ms / 1000.0)

    # Write send times to a file for ue2 to access
    with open('send_times.json', 'w') as f:
        json.dump(send_times, f)

def receive_responses(listen_port):
    """
    Function to receive server responses and calculate latency.
    """
    # Create a UDP socket for receiving responses
    recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    recv_socket.bind(('', listen_port))
    print(f"Listening for responses on port {listen_port}.")

    # Load send times
    with open('send_times.json', 'r') as f:
        send_times = json.load(f)

    received_requests = set()

    while True:
        try:
            data, addr = recv_socket.recvfrom(1024)
            if len(data) < 4:
                print(f"Received response too short from {addr}. Ignoring.")
                continue

            # Unpack the request_id from the response
            (request_id,) = struct.unpack('!I', data[:4])
            receive_time = time.time()
            if str(request_id) in send_times:
                send_time = send_times[str(request_id)]
                latency = (receive_time - send_time) * 1000  # Convert to ms
                print(f"Received response for request {request_id} from {addr} at {receive_time:.6f}, latency: {latency:.2f} ms")
                received_requests.add(request_id)
                # Stop if all responses are received
                if len(received_requests) == len(send_times):
                    break
            else:
                print(f"Received unknown request_id {request_id} from {addr}")
        except Exception as e:
            print(f"An error occurred while receiving responses: {e}")

def client_main(args):
    """
    Main function to handle sending and receiving UDP packets.
    """
    # Switch to the specified network namespace
    try:
        enter_netns(args.namespace)
        print(f"Entered network namespace: {args.namespace}")
    except Exception as e:
        print(f"Failed to enter network namespace {args.namespace}: {e}")
        return

    if args.ue == 'ue1':
        send_requests(args.server_ip, args.server_port, args.num_requests, args.num_packets, args.interval)
    elif args.ue == 'ue2':
        receive_responses(args.listen_port)
    else:
        print("Invalid UE specified. Use 'ue1' or 'ue2'.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Client')
    parser.add_argument('--ue', type=str, required=True, choices=['ue1', 'ue2'], help='UE namespace to use (ue1 or ue2)')
    parser.add_argument('--namespace', type=str, required=True, help='Network namespace to switch to')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-requests', type=int, required=True, help='Number of requests to send')
    parser.add_argument('--num-packets', type=int, required=True, help='Number of data packets per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    args = parser.parse_args()

    client_main(args)