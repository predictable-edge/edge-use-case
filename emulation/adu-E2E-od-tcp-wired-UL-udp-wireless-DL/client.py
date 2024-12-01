def receive_responses(listen_port, num_requests, send_times, lock, result_dir):
    """Function to receive UDP responses in ue2 namespace and calculate latency."""
    try:
        # Enter ue2 namespace for receiving responses
        enter_netns('ue2')
        print("Entered network namespace: ue2")
    except Exception as e:
        print(f"Error: {e}")
        return

    try:
        # Create and bind UDP receiving socket in ue2 namespace
        recv_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        recv_socket.bind(('', listen_port))
        print(f"Listening for UDP responses on port {listen_port} in ue2")
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
        with open(latency_file_path, 'w') as f:
            # Write header for latency file
            header = f"{'Label':<15}{'Latency':>15}"
            f.write(header + '\n')

            while len(completed_requests) < num_requests:
                try:
                    # Receive request_id (4 bytes) via UDP
                    data, addr = recv_socket.recvfrom(4)
                    if len(data) < 4:
                        print(f"Received response too short from {addr}")
                        continue

                    request_id, = struct.unpack('!I', data)
                    receive_time = time.time()

                    # Calculate and record latency
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