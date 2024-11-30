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

def send_requests(server_ip, server_port, num_requests, bytes_per_request, interval_ms, send_times, lock):
    """Function to send TCP requests from host machine.
    
    Args:
        server_ip: IP address of the server
        server_port: Port number of the server
        num_requests: Total number of requests to send
        bytes_per_request: Size of each request in bytes
        interval_ms: Time interval between requests in milliseconds
        send_times: Dictionary to store send timestamps
        lock: Threading lock for synchronization
    """
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
            # Record send time with thread safety
            with lock:
                send_times[request_id] = time.time()

            if request_id % 100 == 0 or request_id == 1:
                print(f"Sending request {request_id}.")

            try:
                # Send header with request_id and total bytes
                header = struct.pack('!II', request_id, bytes_per_request)
                send_socket.sendall(header)
                
                # Send data in chunks for better memory management
                remaining_bytes = bytes_per_request
                while remaining_bytes > 0:
                    chunk_size = min(4096, remaining_bytes)
                    data = b'\0' * chunk_size
                    send_socket.sendall(data)
                    remaining_bytes -= chunk_size

            except Exception as e:
                print(f"Failed to send request {request_id}: {e}")
                break

            # Wait before sending next request
            if request_id != num_requests:
                time.sleep(interval_ms / 1000.0)

    except Exception as e:
        print(f"Error in send loop: {e}")
    finally:
        send_socket.close()
        print("Completed sending all requests.")

def receive_responses(listen_port, num_requests, send_times, lock, result_dir):
    """Function to receive TCP responses in ue2 namespace and calculate latency.
    
    Args:
        listen_port: Port to listen for responses
        num_requests: Total number of requests to expect
        send_times: Dictionary containing request send timestamps
        lock: Threading lock for synchronization
        result_dir: Directory to store results
    """
    try:
        # Enter ue2 namespace for receiving responses
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        # Create and bind receiving socket in ue2 namespace
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        recv_socket.bind(('', listen_port))
        recv_socket.listen(1)
        print(f"Listening for responses on port {listen_port} in ue2")
    except Exception as e:
        print(f"Failed to create receiving socket: {e}")
        return

    # Create result directory if it doesn't exist
    try:
        os.makedirs(result_dir, exist_ok=True)
    except Exception as e:
        print(f"Failed to create directory '{result_dir}': {e}")
        return

    latency_file_path = os.path.join(result_dir, 'latency.txt')
    completed_requests = set()

    try:
        # Wait for connection from server
        client_socket, addr = recv_socket.accept()
        print(f"Accepted connection from {addr}")

        with open(latency_file_path, 'w') as f:
            # Write header for latency file
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while len(completed_requests) < num_requests:
                try:
                    # Receive request_id (4 bytes)
                    data = client_socket.recv(4)
                    if not data:
                        break

                    request_id, = struct.unpack('!I', data)
                    receive_time = time.time()

                    # Calculate and record latency
                    with lock:
                        if request_id in send_times:
                            send_time = send_times[request_id]
                            latency = (receive_time - send_time) * 1000  # Convert to milliseconds
                            label = f"{request_id}"
                            latency_str = f"{latency:.2f} ms"
                            line = f"{label:<15}{latency_str:>15}"
                            f.write(line + '\n')
                            f.flush()  # Ensure immediate write to file

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
        try:
            client_socket.close()
        except:
            pass
        recv_socket.close()

    print(f"All {num_requests} requests have been processed")
    print(f"Latency results saved to {latency_file_path}")

def client_main(args):
    """Main function to handle sending and receiving TCP messages.
    
    Args:
        args: Command line arguments
    """
    send_times = {}
    send_times_lock = threading.Lock()

    # Create result directory with timestamp
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-od-tcp', timestamp)

    # Start sending and receiving threads
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
    parser = argparse.ArgumentParser(description='TCP Client')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-requests', type=int, required=True, help='Number of requests to send')
    parser.add_argument('--bytes-per-request', type=int, required=True, help='Number of bytes per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    args = parser.parse_args()

    client_main(args)