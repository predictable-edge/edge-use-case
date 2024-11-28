import socket
import argparse
import time
import threading
import traceback
from collections import deque

# Global variables for shared data
send_queue = deque()    # Queue of request indices sent but not yet acknowledged
start_times = {}        # Dictionary to store start times for each request
latency_results = {}    # Dictionary to store latency results
request_timeouts = {}   # Dictionary to store expected timeout times for each request
sending_done = threading.Event()  # Event to signal when sending is complete
data_lock = threading.Lock()      # Lock for thread-safe operations on shared data

def parse_arguments():
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(description='Ping Pong Test UDP Client')
    parser.add_argument('server_ip', help='Server IP address')
    parser.add_argument('num_requests', type=int, help='Number of requests to send')
    parser.add_argument('packets_per_request', type=int, help='Number of data packets per request')
    parser.add_argument('interval_ms', type=float, help='Interval between requests in milliseconds')
    parser.add_argument('--port', type=int, default=10050, help='Server port (default: 10050)')
    parser.add_argument('--packet_size', type=int, default=512, help='Data packet size in bytes (default: 512)')
    parser.add_argument('--bind_port', type=int, help='Local port to bind the client socket to')
    parser.add_argument('--response_timeout', type=float, default=5.0, help='Maximum time to wait for a response in seconds (default: 5.0)')
    return parser.parse_args()

def send_requests(client_socket, server_address, num_requests, packets_per_request, interval_ms, packet_size, response_timeout):
    """Thread function to send requests to the server."""
    for request_index in range(num_requests):
        request_start_time = time.time()

        # Send data packets for the current request
        for sequence_number in range(packets_per_request):
            # Construct data packet with three fields
            data = bytearray(packet_size)
            data[0:4] = request_index.to_bytes(4, byteorder='big')
            data[4:8] = packets_per_request.to_bytes(4, byteorder='big')
            data[8:12] = sequence_number.to_bytes(4, byteorder='big')
            # Send data packet to server
            try:
                client_socket.sendto(data, server_address)
            except socket.error as e:
                print(f"Socket error during send: {e}")
                continue

        # Record start time and add request index to send queue
        with data_lock:
            start_times[request_index] = request_start_time
            send_queue.append(request_index)
            # Set the expected timeout time for this request
            request_timeouts[request_index] = request_start_time + response_timeout

        # Calculate elapsed time and adjust sleep accordingly
        elapsed_time_ms = (time.time() - request_start_time) * 1000
        remaining_time_ms = interval_ms - elapsed_time_ms
        if remaining_time_ms > 0:
            time.sleep(remaining_time_ms / 1000.0)
        else:
            print(f"Warning: Sending request {request_index} took longer than the interval of {interval_ms} ms")

    # Signal that sending is done
    sending_done.set()
    print("Sending done.")

def receive_responses(client_socket, response_timeout):
    """Thread function to receive responses from the server."""
    while True:
        with data_lock:
            # Check if all requests have been processed
            if sending_done.is_set() and not send_queue:
                break

            # Remove requests that have timed out
            current_time = time.time()
            timed_out_requests = [req_index for req_index in list(send_queue) if request_timeouts.get(req_index, 0) <= current_time]
            for req_index in timed_out_requests:
                print(f"Request {req_index} timed out.")
                send_queue.remove(req_index)
                del start_times[req_index]
                del request_timeouts[req_index]

        try:
            # Receive response from server
            response, server = client_socket.recvfrom(1024)  # Buffer size can be larger

            if len(response) < 4:
                print("Received incomplete response from server. Ignoring.")
                continue

            # Unpack request_index from response
            request_index = int.from_bytes(response[0:4], byteorder='big')

            with data_lock:
                if request_index in start_times:
                    # Record end time
                    end_time = time.time()
                    # Calculate latency in milliseconds
                    latency = (end_time - start_times[request_index]) * 1000
                    latency_results[request_index] = latency
                    print(f"Request {request_index} latency: {latency:.2f} ms")
                    # Remove the request from the queue
                    try:
                        send_queue.remove(request_index)
                    except ValueError:
                        pass  # Already removed or not present
                    # Remove the start time and timeout as they're no longer needed
                    del start_times[request_index]
                    del request_timeouts[request_index]
                else:
                    print(f"Received response for unknown or timed-out request {request_index}")
        except socket.timeout:
            # No data received within timeout
            continue
        except socket.error as e:
            print(f"Socket error during receive: {e}")
            break
        except Exception as e:
            print(f"An unexpected error occurred: {e}")
            traceback.print_exc()
            break

    print("Receiving done.")

def write_latency_results():
    """Writes the latency results to a file."""
    with open('latency.txt', 'w') as output_file:
        # Write header with formatted alignment
        output_file.write(f"{'Index':<10}{'Latency(ms)':<15}\n")
        for request_index in sorted(latency_results.keys()):
            latency = latency_results[request_index]
            output_file.write(f"{request_index:<10}{latency:<15.2f}\n")

def main():
    # Parse command-line arguments
    args = parse_arguments()

    server_ip = args.server_ip
    num_requests = args.num_requests
    packets_per_request = args.packets_per_request
    interval_ms = args.interval_ms  # Interval in milliseconds
    port = args.port
    packet_size = args.packet_size
    bind_port = args.bind_port
    response_timeout = args.response_timeout

    # Define server address
    server_address = (server_ip, port)

    # Create UDP socket
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Optional: Bind the client socket to a specific local port
    if bind_port:
        client_socket.bind(('', bind_port))
        print(f"Client socket bound to local port {bind_port}")
    else:
        # Bind to an ephemeral port and retrieve it
        client_socket.bind(('', 0))
        print(f"Client socket bound to local port {client_socket.getsockname()[1]}")

    # Print the local socket address
    local_address = client_socket.getsockname()
    print(f"Client socket is using local address {local_address}")

    # Set a timeout for the socket to prevent blocking indefinitely
    client_socket.settimeout(1.0)  # Timeout in seconds

    # Start the sending and receiving threads
    sender_thread = threading.Thread(target=send_requests, args=(
        client_socket,
        server_address,
        num_requests,
        packets_per_request,
        interval_ms,
        packet_size,
        response_timeout
    ))

    receiver_thread = threading.Thread(target=receive_responses, args=(
        client_socket,
        response_timeout
    ))

    sender_thread.start()
    receiver_thread.start()

    # Wait for both threads to finish
    sender_thread.join()
    receiver_thread.join()

    # Close the socket
    client_socket.close()

    # Write latency results to the output file
    write_latency_results()
    print("Latency results written to latency.txt")

if __name__ == '__main__':
    main()