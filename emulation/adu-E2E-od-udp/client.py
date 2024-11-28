import socket
import argparse
import struct
import time
import os
import ctypes
import threading
from datetime import datetime

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
    try:
        fd = os.open(netns_path, os.O_RDONLY)
        setns(fd, 0)  # CLONE_NEWNET is 0
        os.close(fd)
    except FileNotFoundError:
        print(f"Network namespace '{namespace_name}' does not exist.")
        raise
    except Exception as e:
        print(f"Failed to enter network namespace '{namespace_name}': {e}")
        raise

def send_requests(server_ip, server_port, num_requests, num_packets, interval_ms, send_times, lock):
    """
    Function to send UDP requests to the server from ue1 namespace.
    Each request consists of multiple packets sent without intervals.
    """
    try:
        # Switch to ue1 network namespace
        enter_netns('ue1')
        print("Entered network namespace: ue1")
    except Exception as e:
        print(f"Error: {e}")
        return

    # Create a UDP socket for sending packets
    try:
        send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send_socket.bind(('', 0))  # Bind to an ephemeral port
        send_port = send_socket.getsockname()[1]
        print(f"Sending from local port {send_port} in ue1.")
    except Exception as e:
        print(f"Failed to create/send socket: {e}")
        return

    for request_id in range(1, num_requests + 1):
        # Record the send time for this request with thread safety
        with lock:
            send_times[request_id] = time.time()

        # Print sending request at intervals of 100 or first request
        if request_id % 100 == 0 or request_id == 1:
            print(f"Sending request {request_id}.")

        for seq_num in range(1, num_packets + 1):
            # Pack request_id, total_packets, and sequence number into the first 12 bytes
            header = struct.pack('!III', request_id, num_packets, seq_num)
            # Fill the rest of the packet to make it 1400 bytes
            payload = header + b'\0' * (1400 - 12)
            try:
                send_socket.sendto(payload, (server_ip, server_port))
            except Exception as e:
                print(f"Failed to send packet {seq_num} for request {request_id}: {e}")

        # Wait for interval before sending the next request
        if request_id != num_requests:
            time.sleep(interval_ms / 1000.0)

    send_socket.close()
    print("Completed sending all requests.")

def receive_responses(listen_port, num_requests, send_times, lock, result_dir):
    """
    Function to receive server responses and calculate latency in ue2 namespace.
    """
    try:
        # Switch to ue2 network namespace
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    # Create a UDP socket for receiving responses
    try:
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_socket.bind(('', listen_port))
        print(f"Listening for responses on port {listen_port} in ue2.")
    except Exception as e:
        print(f"Failed to create/receive socket: {e}")
        return

    received_requests = set()

    # Create the result directory if it doesn't exist
    try:
        os.makedirs(result_dir, exist_ok=True)
    except Exception as e:
        print(f"Failed to create directory '{result_dir}': {e}")
        return

    # Define the path for latency.txt
    latency_file_path = os.path.join(result_dir, 'latency.txt')

    # Open the latency.txt file for writing
    try:
        with open(latency_file_path, 'w') as f:
            # Write the header
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while True:
                try:
                    data, addr = recv_socket.recvfrom(1024)
                    if len(data) < 4:
                        print(f"Received response too short from {addr}. Ignoring.")
                        continue

                    # Unpack the request_id from the response
                    (request_id,) = struct.unpack('!I', data[:4])
                    receive_time = time.time()

                    with lock:
                        if request_id in send_times:
                            send_time = send_times[request_id]
                            latency = (receive_time - send_time) * 1000  # Convert to ms
                            label = f"{request_id}"
                            latency_str = f"{latency:.2f} ms"
                            line = f"{label:<15}{latency_str:>15}"
                            f.write(line + '\n')
                            received_requests.add(request_id)
                        else:
                            print(f"Received unknown request_id {request_id} from {addr}")

                    # Stop if all responses are received
                    if len(received_requests) == num_requests:
                        print("All responses received.")
                        break
                except Exception as e:
                    print(f"An error occurred while receiving responses: {e}")
                    break

    except Exception as e:
        print(f"Failed to write to '{latency_file_path}': {e}")
        return

    recv_socket.close()
    print(f"Latency results saved to {latency_file_path}")

def client_main(args):
    """
    Main function to handle sending and receiving UDP packets.
    """
    # Shared dictionary to store send times
    send_times = {}
    send_times_lock = threading.Lock()  # Lock for thread-safe access

    # Generate a timestamp for the result directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-od-udp', timestamp)

    # Start threads for sending and receiving
    send_thread = threading.Thread(target=send_requests, args=(
        args.server_ip,
        args.server_port,
        args.num_requests,
        args.num_packets,
        args.interval,
        send_times,
        send_times_lock
    ))
    recv_thread = threading.Thread(target=receive_responses, args=(
        args.listen_port,
        args.num_requests,
        send_times,
        send_times_lock,
        result_dir
    ))

    # Start the threads
    send_thread.start()
    recv_thread.start()

    # Wait for both threads to finish
    send_thread.join()
    recv_thread.join()

    print(f"Latency results saved to {os.path.abspath(result_dir)}/latency.txt")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Client')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-requests', type=int, required=True, help='Number of requests to send')
    parser.add_argument('--num-packets', type=int, required=True, help='Number of data packets per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    args = parser.parse_args()

    client_main(args)