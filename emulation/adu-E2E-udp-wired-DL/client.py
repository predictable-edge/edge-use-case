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

# Maximum UDP payload size
MAX_UDP_SIZE = 1400

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

def send_requests(server_ip, server_port, num_requests, bytes_per_request, interval_ms, send_times, lock):
    """Function to send UDP requests from ue1 namespace."""
    if bytes_per_request > MAX_UDP_SIZE:
        print(f"Warning: Requested size {bytes_per_request} exceeds maximum UDP size. Using {MAX_UDP_SIZE} instead.")
        bytes_per_request = MAX_UDP_SIZE

    try:
        enter_netns('ue1')
        print("Entered network namespace: ue1")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"Created UDP socket for sending to {server_ip}:{server_port} from ue1")
    except Exception as e:
        print(f"Failed to create socket: {e}")
        return

    try:
        for request_id in range(1, num_requests + 1):
            with lock:
                send_times[request_id] = time.time()

            if request_id % 100 == 0 or request_id == 1:
                print(f"Sending request {request_id}.")

            try:
                # Pack request_id at the start of the data
                data = struct.pack('!I', request_id) + b'\0' * (bytes_per_request - 4)
                send_socket.sendto(data, (server_ip, server_port))

            except Exception as e:
                print(f"Failed to send request {request_id}: {e}")
                break

            if request_id != num_requests:
                time.sleep(interval_ms / 1000.0)

    except Exception as e:
        print(f"Error in send loop: {e}")
    finally:
        send_socket.close()
        print("Completed sending all requests.")

def receive_responses(listen_port, num_requests, send_times, lock, result_dir):
    """Function to receive UDP responses in ue2 namespace."""
    try:
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        recv_socket.bind(('', listen_port))
        print(f"Listening for responses on port {listen_port} in ue2")
    except Exception as e:
        print(f"Failed to create receiving socket: {e}")
        return

    try:
        os.makedirs(result_dir, exist_ok=True)
    except Exception as e:
        print(f"Failed to create directory '{result_dir}': {e}")
        return

    latency_file_path = os.path.join(result_dir, 'latency.txt')
    completed_requests = set()

    try:
        with open(latency_file_path, 'w') as f:
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while len(completed_requests) < num_requests:
                try:
                    data, addr = recv_socket.recvfrom(64)  # Small buffer for response
                    if len(data) < 4:
                        print(f"Received response too short from {addr}")
                        continue

                    request_id, = struct.unpack('!I', data[:4])
                    receive_time = time.time()

                    with lock:
                        if request_id in send_times:
                            send_time = send_times[request_id]
                            latency = (receive_time - send_time) * 1000
                            label = f"{request_id}"
                            latency_str = f"{latency:.2f} ms"
                            line = f"{label:<15}{latency_str:>15}"
                            f.write(line + '\n')
                            f.flush()

                            completed_requests.add(request_id)
                            del send_times[request_id]

                            if request_id % 100 == 0 or request_id == 1:
                                print(f"Completed request {request_id} processing")
                                print(f"Processed {len(completed_requests)} out of {num_requests} requests")
                        else:
                            print(f"Received unknown request_id {request_id}")

                except Exception as e:
                    print(f"Error receiving response: {e}")
                    continue

    except Exception as e:
        print(f"Error in receive loop: {e}")
    finally:
        recv_socket.close()

    print(f"All {num_requests} requests have been processed")
    print(f"Latency results saved to {latency_file_path}")

def client_main(args):
    """Main function to handle sending and receiving messages."""
    send_times = {}
    send_times_lock = threading.Lock()

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-od-udp', timestamp)

    send_thread = threading.Thread(target=send_requests, args=(
        args.server_ip,
        args.server_port,
        args.num_requests,
        args.bytes_per_request,
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
    parser.add_argument('--bytes-per-request', type=int, required=True, help='Number of bytes per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    args = parser.parse_args()

    if args.bytes_per_request > MAX_UDP_SIZE:
        print(f"Warning: bytes-per-request ({args.bytes_per_request}) exceeds maximum UDP size ({MAX_UDP_SIZE}).")
        print(f"Setting to {MAX_UDP_SIZE} bytes.")
        args.bytes_per_request = MAX_UDP_SIZE

    client_main(args)