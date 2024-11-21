import socket
import time
import argparse

def ping_pong_test(server_ip, port=10050, num_tests=10, interval=1.0):
    """
    Conducts a ping-pong test to a UDP server and measures RTT.

    Args:
        server_ip (str): Public IP address of the UDP server.
        port (int): UDP port of the server (default: 10050).
        num_tests (int): Number of ping-pong tests to perform.
        interval (float): Interval between consecutive tests in seconds.
    """
    # Create a UDP socket
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client_socket.settimeout(5)  # Timeout for receiving server responses (in seconds)
    message = "ping"
    
    print(f"Starting ping-pong test to {server_ip}:{port} for {num_tests} tests with {interval}s interval...")
    
    rtts = []  # List to store RTT measurements

    for i in range(num_tests):
        try:
            # Record the start time
            start_time = time.time()

            # Send the ping message
            client_socket.sendto(message.encode(), (server_ip, port))

            # Wait for the pong response
            data, server_address = client_socket.recvfrom(1024)
            
            # Record the end time
            end_time = time.time()

            # Calculate RTT in milliseconds
            rtt = (end_time - start_time) * 1000
            rtts.append(rtt)
            
            print(f"Test {i+1}: Received '{data.decode()}' from {server_address} (RTT: {rtt:.2f} ms)")
        except socket.timeout:
            print(f"Test {i+1}: Request timed out")
        
        # Wait for the specified interval before the next test
        time.sleep(interval)
    
    # Print statistics
    if rtts:
        print("\nPing-Pong Test Completed")
        print(f"Average RTT: {sum(rtts)/len(rtts):.2f} ms")
        print(f"Minimum RTT: {min(rtts):.2f} ms")
        print(f"Maximum RTT: {max(rtts):.2f} ms")
    else:
        print("No responses received. Check server configuration or network connectivity.")

    # Close the socket
    client_socket.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="UDP Ping-Pong Client")
    parser.add_argument("server_ip", type=str, help="Public IP address of the server")
    parser.add_argument("--port", type=int, default=10050, help="Server UDP port (default: 10050)")
    parser.add_argument("--num_tests", type=int, default=10, help="Number of tests to perform (default: 10)")
    parser.add_argument("--interval", type=float, default=1.0, help="Interval between tests in seconds (default: 1.0)")
    args = parser.parse_args()

    ping_pong_test(args.server_ip, args.port, args.num_tests, args.interval)