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

def recv_all(sock, n):
    """Helper function to receive exact number of bytes."""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)

def send_requests(server_ip, server_port, num_requests, bytes_per_request, interval_ms, send_times, lock, multiplier):
    """Function to send TCP requests from host machine."""
    try:
        # Create and connect TCP socket on host
        send_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        send_socket.connect((server_ip, server_port))
        print(f"Connected to server {server_ip}:{server_port} from host")
    except Exception as e:
        print(f"Failed to connect to server: {e}")
        return

    try:
        for request_id in range(1, num_requests + 1):
            with lock:
                send_times[request_id] = time.time()

            if request_id % 100 == 0 or request_id == 1:
                print(f"Sending request {request_id}")

            try:
                # Send header with multiplier
                header = struct.pack('!III', request_id, bytes_per_request, multiplier)
                send_socket.sendall(header)
                
                # Send the data
                data = b'\0' * bytes_per_request
                send_socket.sendall(data)

            except Exception as e:
                print(f"Failed to send request {request_id}: {e}")
                break

            if request_id != num_requests:
                time.sleep(interval_ms / 1000.0)

        print("Completed sending all requests")
    finally:
        send_socket.close()

def receive_responses(listen_port, num_requests, bytes_per_request, send_times, lock, result_dir):
    """Function to receive TCP responses in ue2 namespace."""
    try:
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        recv_socket.bind(('', listen_port))
        recv_socket.listen(1)
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
        client_socket, client_address = recv_socket.accept()
        print(f"Accepted connection from {client_address}")

        with open(latency_file_path, 'w') as f:
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while len(completed_requests) < num_requests:
                try:
                    # Receive header
                    header_data = recv_all(client_socket, 8)
                    if not header_data:
                        break

                    request_id, bytes_size = struct.unpack('!II', header_data)
                    
                    # Receive complete response data
                    response_data = recv_all(client_socket, bytes_size)
                    if not response_data:
                        break

                    # Calculate and record latency
                    with lock:
                        if request_id in send_times:
                            latency = (time.time() - send_times[request_id]) * 1000
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

                except Exception as e:
                    print(f"Error receiving response: {e}")
                    continue

    except Exception as e:
        print(f"Error in receive loop: {e}")
    finally:
        try:
            client_socket.close()
        except:
            pass
        recv_socket.close()

    print(f"All {num_requests} requests have been processed")
    print(f"Latency results saved to {latency_file_path}")

def client_main(args):
    """Main function to handle sending and receiving TCP messages."""
    send_times = {}
    send_times_lock = threading.Lock()

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-vs-tcp-multiple-wired-UL', timestamp)

    send_thread = threading.Thread(target=send_requests, args=(
        args.server_ip,
        args.server_port,
        args.num_requests,
        args.bytes_per_request,
        args.interval,
        send_times,
        send_times_lock,
        args.response_multiplier
    ))
    recv_thread = threading.Thread(target=receive_responses, args=(
        args.listen_port,
        args.num_requests,
        args.bytes_per_request * args.response_multiplier,
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
    parser = argparse.ArgumentParser(description='TCP Client')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-requests', type=int, required=True, help='Number of requests to send')
    parser.add_argument('--bytes-per-request', type=int, required=True, help='Number of bytes per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    parser.add_argument('--response-multiplier', type=int, default=1, help='Multiplier for response size (default: 1)')
    args = parser.parse_args()

    client_main(args)