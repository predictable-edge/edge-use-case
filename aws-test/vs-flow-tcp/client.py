import socket
import argparse
import time
import threading
from collections import defaultdict

def parse_arguments():
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(description='Ping Pong Test Client')
    parser.add_argument('server_ip', help='Server IP address')
    parser.add_argument('num_requests', type=int, help='Number of requests to send')
    parser.add_argument('packets_per_request', type=int, help='Number of data packets per request')
    parser.add_argument('interval_ms', type=float, help='Interval between requests in milliseconds')
    parser.add_argument('--port', type=int, default=10000, help='Server port (default: 10000)')
    parser.add_argument('--packet_size', type=int, default=1500, help='Data packet size in bytes (default: 1500)')
    return parser.parse_args()

def send_requests(client_socket, num_requests, packets_per_request, interval_ms, packet_size, start_times, sending_done, data_lock):
    """Thread function to send requests to the server."""
    for request_index in range(num_requests):
        # Record start time for the request
        with data_lock:
            start_times[request_index] = time.time()
        # Send data packets for the current request
        for sequence_number in range(packets_per_request):
            # Construct data packet with three fields
            data = bytearray(packet_size)
            data[0:4] = request_index.to_bytes(4, byteorder='big')
            data[4:8] = packets_per_request.to_bytes(4, byteorder='big')
            data[8:12] = sequence_number.to_bytes(4, byteorder='big')
            # Send data packet
            client_socket.sendall(data)
        # Wait for the specified interval before sending the next request
        time.sleep(interval_ms / 1000.0)  # Convert milliseconds to seconds
    # Signal that sending is done
    sending_done.set()

def receive_responses(client_socket, num_requests, packets_per_request, start_times, latency_results, sending_done, data_lock):
    """Thread function to receive responses from the server."""
    # Dictionary to keep track of received sequence numbers per request index
    received_packets = defaultdict(set)
    # Buffer to store incoming data
    buffer = b''
    expected_packet_size = 1500  # Must match the server's packet size

    while not sending_done.is_set() or len(latency_results) < num_requests:
        try:
            # Receive data from server
            data = client_socket.recv(4096)
            if not data:
                # No data received, possibly the connection is closed
                break
            buffer += data
            # Process all complete packets in the buffer
            while len(buffer) >= expected_packet_size:
                # Extract one complete packet
                packet = buffer[:expected_packet_size]
                buffer = buffer[expected_packet_size:]
                
                # Extract fields from response
                if len(packet) < 12:
                    # Invalid packet size, skip
                    continue
                request_index = int.from_bytes(packet[0:4], byteorder='big')
                total_packets = int.from_bytes(packet[4:8], byteorder='big')
                sequence_number = int.from_bytes(packet[8:12], byteorder='big')

                with data_lock:
                    # Add sequence number to the set of received packets for this request
                    received_packets[request_index].add(sequence_number)

                    # Check if all packets for this request have been received
                    if len(received_packets[request_index]) == total_packets:
                        # Record end time
                        end_time = time.time()
                        # Calculate latency in milliseconds
                        latency = (end_time - start_times[request_index]) * 1000
                        latency_results[request_index] = latency
                        print(f"Request {request_index} latency: {latency:.2f} ms")
                        # Remove the request from received_packets to free memory
                        del received_packets[request_index]
        except socket.timeout:
            # No data received within timeout
            with data_lock:
                if sending_done.is_set() and len(latency_results) >= num_requests:
                    # All responses received, exit the loop
                    break
                else:
                    # Continue waiting for responses
                    continue
        except socket.error as e:
            print(f"Socket error: {e}")
            break

def write_latency_results(latency_results):
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

    # Create TCP socket
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Connect to the server
    try:
        client_socket.connect((server_ip, port))
    except socket.error as e:
        print(f"Failed to connect to server: {e}")
        return
    # Set a timeout for the socket to prevent blocking indefinitely
    client_socket.settimeout(2.0)  # Timeout in seconds

    # Shared data structures for communication between threads
    start_times = {}        # Dictionary to store start times for each request
    latency_results = {}    # Dictionary to store latency results

    # Event to signal when sending is complete
    sending_done = threading.Event()

    # Lock for thread-safe operations on shared data
    data_lock = threading.Lock()

    # Start the sending and receiving threads
    sender_thread = threading.Thread(target=send_requests, args=(
        client_socket,
        num_requests,
        packets_per_request,
        interval_ms,
        packet_size,
        start_times,
        sending_done,
        data_lock
    ))

    receiver_thread = threading.Thread(target=receive_responses, args=(
        client_socket,
        num_requests,
        packets_per_request,
        start_times,
        latency_results,
        sending_done,
        data_lock
    ))

    sender_thread.start()
    receiver_thread.start()

    # Wait for both threads to finish
    sender_thread.join()
    receiver_thread.join()

    # Close the socket
    client_socket.close()

    # Write latency results to the output file
    write_latency_results(latency_results)

if __name__ == '__main__':
    main()