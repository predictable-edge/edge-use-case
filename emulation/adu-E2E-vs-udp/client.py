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

class ResponseTracker:
    def __init__(self, total_packets):
        self.total_packets = total_packets
        self.received_packets = set()
        self.lock = threading.Lock()

    def add_packet(self, seq_num):
        with self.lock:
            self.received_packets.add(seq_num)

    def is_complete(self):
        with self.lock:
            return len(self.received_packets) == self.total_packets

def send_requests(server_ip, server_port, num_requests, num_packets, interval_ms, send_times, lock):
    """
    Function to send UDP requests to the server from ue1 namespace.
    Each request consists of multiple packets sent without intervals.
    """
    try:
        enter_netns('ue1')
        print("Entered network namespace: ue1")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send_socket.bind(('', 0))
        send_port = send_socket.getsockname()[1]
        print(f"Sending from local port {send_port} in ue1.")
    except Exception as e:
        print(f"Failed to create/send socket: {e}")
        return

    for request_id in range(1, num_requests + 1):
        # Record the send time for this request with thread safety
        with lock:
            send_times[request_id] = time.time()

        if request_id % 100 == 0 or request_id == 1:
            print(f"Sending request {request_id}.")

        for seq_num in range(1, num_packets + 1):
            header = struct.pack('!III', request_id, num_packets, seq_num)
            payload = header + b'\0' * (1400 - 12)
            try:
                send_socket.sendto(payload, (server_ip, server_port))
            except Exception as e:
                print(f"Failed to send packet {seq_num} for request {request_id}: {e}")

        if request_id != num_requests:
            time.sleep(interval_ms / 1000.0)

    send_socket.close()
    print("Completed sending all requests.")

def receive_responses(listen_port, num_requests, num_packets, send_times, lock, result_dir):
    """
    Function to receive server responses and calculate latency in ue2 namespace.
    """
    try:
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_socket.bind(('', listen_port))
        print(f"Listening for responses on port {listen_port} in ue2.")
    except Exception as e:
        print(f"Failed to create/receive socket: {e}")
        return

    try:
        os.makedirs(result_dir, exist_ok=True)
    except Exception as e:
        print(f"Failed to create directory '{result_dir}': {e}")
        return

    latency_file_path = os.path.join(result_dir, 'latency.txt')
    response_trackers = {}

    try:
        with open(latency_file_path, 'w') as f:
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while True:
                try:
                    data, addr = recv_socket.recvfrom(1400)  # Match the packet size
                    if len(data) < 12:
                        print(f"Received response too short from {addr}. Ignoring.")
                        continue

                    # Unpack the header from the response
                    request_id, total_packets, seq_num = struct.unpack('!III', data[:12])

                    # Initialize tracker for new requests
                    if request_id not in response_trackers:
                        response_trackers[request_id] = ResponseTracker(total_packets)

                    # Track this packet
                    tracker = response_trackers[request_id]
                    tracker.add_packet(seq_num)

                    # If we've received all packets for this request, calculate latency
                    if tracker.is_complete():
                        with lock:
                            if request_id in send_times:
                                send_time = send_times[request_id]
                                latency = (time.time() - send_time) * 1000  # Convert to ms
                                label = f"{request_id}"
                                latency_str = f"{latency:.2f} ms"
                                line = f"{label:<15}{latency_str:>15}"
                                f.write(line + '\n')
                                f.flush()  # Ensure writing to file immediately
                                
                                # Remove the tracker and send_time after processing
                                del response_trackers[request_id]
                                del send_times[request_id]

                                # Log progress
                                if request_id % 100 == 0 or request_id == 1:
                                    print(f"Completed request {request_id} processing")

                                # Check if we're done with all requests
                                if len(send_times) == 0:
                                    print("All responses received and processed.")
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
    send_times = {}
    send_times_lock = threading.Lock()

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-od-udp', timestamp)

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
        args.num_packets,
        send_times,
        send_times_lock,
        result_dir
    ))

    send_thread.start()
    recv_thread.start()

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