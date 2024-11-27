import sys

def filter_latency(file_path, start_line, threshold):
    try:
        with open(file_path, 'r') as file:
            lines = file.readlines()

        # Extract data starting from the specified line
        filtered_lines = lines[start_line - 1:]  # Adjust for 0-indexed
        results = []
        last_frame = None

        for line in filtered_lines:
            try:
                # Parse the line, assuming it follows the format: Frame, E2E latency
                parts = line.strip().split()
                frame = int(parts[0])  # Convert frame number to integer
                latency = float(parts[1])  # Convert latency to float (ms)
                
                # Check if latency exceeds the threshold
                if latency > threshold:
                    # Check if we need a newline due to non-continuous frames
                    if last_frame is not None and frame != last_frame + 1:
                        results.append("")  # Add a newline (empty string to separate groups)
                    
                    results.append(f"Frame: {frame}, E2E latency: {latency} ms")
                    last_frame = frame
            except (IndexError, ValueError):
                # Handle lines that don't match the expected format
                continue

        # Output results
        if results:
            print(f"Lines with E2E latency exceeding {threshold} ms:")
            for result in results:
                if result == "":
                    print()  # Print a newline
                else:
                    print(result)
        else:
            print(f"No lines found with E2E latency exceeding {threshold} ms.")
    except FileNotFoundError:
        print(f"Error: The file '{file_path}' was not found.")
    except Exception as e:
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    # Get command-line arguments
    if len(sys.argv) != 4:
        print("Usage: python script.py <file_path> <start_line> <threshold>")
    else:
        file_path = sys.argv[1]
        start_line = int(sys.argv[2])
        threshold = float(sys.argv[3])
        filter_latency(file_path, start_line, threshold)