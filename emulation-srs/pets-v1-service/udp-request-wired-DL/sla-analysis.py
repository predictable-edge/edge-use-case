import os
import re

def parse_folder_name(folder_name):
    """
    Parse folder name to extract UE configurations
    Example: ue1-20-16-50-ue2-110-100-100
    Returns: Dictionary mapping UE ID to its latency requirement
    """
    # Split by '-' and process in groups of 4 (ueX-size-interval-latency)
    parts = folder_name.split('-')
    ue_latency = {}
    i = 0
    while i < len(parts):
        if parts[i].startswith('ue'):
            ue_id = int(parts[i][2:])  # Extract number from 'ueX'
            latency_req = int(parts[i+3])  # Latency requirement is the 4th value
            ue_latency[ue_id] = latency_req
            i += 4
        else:
            i += 1
    return ue_latency

def analyze_latency_file(file_path, latency_requirement):
    """
    Analyze latency values from a single UE's file
    Returns: Tuple of (total_requests, satisfied_requests)
    """
    total_requests = 0
    satisfied_requests = 0
    
    with open(file_path, 'r') as f:
        # Skip header lines
        for line in f:
            if line.strip().startswith('Request ID'):
                break
        
        # Process latency values
        for line in f:
            if line.strip() and not line.startswith('==='):
                parts = line.split()
                if len(parts) >= 2:
                    latency = float(parts[1])
                    total_requests += 1
                    if latency <= latency_requirement:
                        satisfied_requests += 1
    
    return total_requests, satisfied_requests

def main(folder_path):
    """
    Main function to analyze latency requirements satisfaction for all UEs
    Args:
        folder_path: Path to the result folder
    """
    # Get folder name from path
    folder_name = os.path.basename(folder_path)
    
    # Parse UE configurations from folder name
    ue_latency = parse_folder_name(folder_name)
    
    print("\nLatency Requirement Satisfaction Analysis:")
    print("----------------------------------------")
    
    # Analyze each UE's latency file
    for ue_id, latency_req in ue_latency.items():
        file_path = os.path.join(folder_path, f'latency_rnti{ue_id}.txt')
        if os.path.exists(file_path):
            total, satisfied = analyze_latency_file(file_path, latency_req)
            satisfaction_rate = (satisfied / total * 100) if total > 0 else 0
            
            print(f"\nUE {ue_id}:")
            print(f"Latency Requirement: {latency_req} ms")
            print(f"Total Requests: {total}")
            print(f"Satisfied Requests: {satisfied}")
            print(f"Satisfaction Rate: {satisfaction_rate:.2f}%")
        else:
            print(f"\nUE {ue_id}: File not found")

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("Usage: python script.py <folder_path>")
        sys.exit(1)
    
    folder_path = sys.argv[1]
    main(folder_path)