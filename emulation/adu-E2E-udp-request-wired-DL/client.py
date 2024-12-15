import socket
import argparse
import struct
import time
import os
import ctypes
import threading
from datetime import datetime

libc = ctypes.CDLL('libc.so.6', use_errno=True)
MAX_UDP_SIZE = 1400

def setns(fd, nstype):
    if libc.setns(fd, nstype) != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, os.strerror(errno))

def enter_netns(namespace_name):
    netns_path = f'/var/run/netns/{namespace_name}'
    try:
        fd = os.open(netns_path, os.O_RDONLY)
        setns(fd, 0)
        os.close(fd)
    except FileNotFoundError:
        print(f"Network namespace '{namespace_name}' does not exist.")
        raise
    except Exception as e:
        print(f"Failed to enter network namespace '{namespace_name}': {e}")
        raise

def send_requests(server_ip, server_port, num_requests, packets_per_request, interval_ms, send_times, lock):
    try:
        enter_netns('ue1')
        send_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"Started sending to {server_ip}:{server_port}")
    except Exception as e:
        print(f"Error: {e}")
        return

    payload = b'\0' * (MAX_UDP_SIZE - 12)

    try:
        for request_id in range(1, num_requests + 1):
            with lock:
                send_times[request_id] = time.time()

            for seq_num in range(packets_per_request):
                try:
                    header = struct.pack('!III', request_id, seq_num, packets_per_request)
                    data = header + payload
                    send_socket.sendto(data, (server_ip, server_port))
                except Exception as e:
                    print(f"Send error: {e}")
                    break

            if request_id != num_requests:
                time.sleep(interval_ms / 1000.0)

    except Exception as e:
        print(f"Error in send loop: {e}")
    finally:
        send_socket.close()
        print("Completed sending all requests")

def receive_responses(listen_port, num_requests, send_times, lock, result_dir):
    try:
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        recv_socket.bind(('', listen_port))
        print(f"Listening on port {listen_port}")
    except Exception as e:
        print(f"Socket error: {e}")
        return

    try:
        os.makedirs(result_dir, exist_ok=True)
    except Exception as e:
        print(f"Directory error: {e}")
        return

    latency_file_path = os.path.join(result_dir, 'latency.txt')
    completed_requests = set()

    try:
        with open(latency_file_path, 'w') as f:
            f.write(f"{'Label':<15}{'Latency':>15}\n")

            while len(completed_requests) < num_requests:
                try:
                    data, addr = recv_socket.recvfrom(64)
                    if len(data) < 4:
                        continue

                    request_id, = struct.unpack('!I', data[:4])
                    receive_time = time.time()

                    with lock:
                        if request_id in send_times:
                            send_time = send_times[request_id]
                            latency = (receive_time - send_time) * 1000
                            f.write(f"{request_id:<15}{latency:.2f} ms\n")
                            f.flush()

                            completed_requests.add(request_id)
                            del send_times[request_id]

                except Exception as e:
                    print(f"Receive error: {e}")
                    continue

    except Exception as e:
        print(f"Error in receive loop: {e}")
    finally:
        recv_socket.close()

    print(f"Processed {num_requests} requests")
    print(f"Results saved to {latency_file_path}")

def client_main(args):
    send_times = {}
    send_times_lock = threading.Lock()

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    result_dir = os.path.join('../result/adu-E2E-udp-request-wired-DL', timestamp)

    send_thread = threading.Thread(target=send_requests, args=(
        args.server_ip,
        args.server_port,
        args.num_requests,
        args.packets_per_request,
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

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='UDP Client')
    parser.add_argument('--server-ip', type=str, required=True, help='Server IP address')
    parser.add_argument('--server-port', type=int, default=10000, help='Server port number (default: 10000)')
    parser.add_argument('--listen-port', type=int, default=10001, help='Port to listen for responses (default: 10001)')
    parser.add_argument('--num-requests', type=int, required=True, help='Number of requests to send')
    parser.add_argument('--packets-per-request', type=int, required=True, help='Number of packets per request')
    parser.add_argument('--interval', type=int, required=True, help='Interval between requests in milliseconds')
    args = parser.parse_args()

    client_main(args)