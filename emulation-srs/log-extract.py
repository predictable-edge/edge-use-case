import sys
import re
import os

def is_ip_line(line):
    return '[IP]' in line

def is_data_line(line):
    line = line.strip()
    if line == '...':
        return True
    if not line.startswith('00'):
        return False
    parts = line.split(':')
    if len(parts) != 2:
        return False
    hex_addr = parts[0]
    return len(hex_addr) == 4 and all(c in '0123456789abcdefABCDEF' for c in hex_addr)

def count_ip_lines(file_path):
    """Count number of [IP] lines in file"""
    try:
        with open(file_path, 'r') as f:
            count = sum(1 for line in f if '[IP]' in line)
        return count
    except FileNotFoundError:
        print(f"Error: File {file_path} not found")
        return 0
    except Exception as e:
        print(f"Error: {str(e)}")
        return 0

def find_blocks(lines):
    blocks = []
    i = 0
    current_pattern = None
    pattern_lines = []
    block_lines = []
    
    while i < len(lines):
        if is_ip_line(lines[i]):
            # Get complete pattern (IP line + data lines)
            temp_pattern = []
            j = i
            while j < len(lines) and (is_ip_line(lines[j]) or is_data_line(lines[j])):
                temp_pattern.append(lines[j])
                j += 1
            
            if current_pattern is None:
                current_pattern = temp_pattern
                pattern_lines = [i]
                block_lines.extend(temp_pattern)
            elif temp_pattern == current_pattern:
                pattern_lines.append(i)
                block_lines.extend(temp_pattern)
            else:
                if block_lines:
                    blocks.append((pattern_lines.copy(), block_lines.copy()))
                current_pattern = temp_pattern
                pattern_lines = [i]
                block_lines = temp_pattern.copy()
            i = j
        else:
            if block_lines:
                blocks.append((pattern_lines.copy(), block_lines.copy()))
                current_pattern = None
                pattern_lines = []
                block_lines = []
            i += 1
    
    if block_lines:
        blocks.append((pattern_lines.copy(), block_lines.copy()))
    
    return blocks

def extract_ip_blocks(file_path, target_index, range_lines):
    try:
        with open(file_path, 'r') as f:
            lines = f.readlines()
        
        blocks = find_blocks(lines)
        
        try:
            index = int(target_index)
            range_val = int(range_lines)
            
            if index >= len(blocks):
                print(f"Error: Block index {index} not found")
                return
            
            block_starts, block_content = blocks[index]
            pattern_size = len(block_content) // len(block_starts)
            block_start = block_starts[0]
            block_end = block_starts[-1] + pattern_size
            
            # Calculate exact range
            extract_start = block_start - range_val
            if extract_start < 0:
                extract_start = 0
            extract_end = block_end + range_val
            if extract_end > len(lines):
                extract_end = len(lines)
            
            output_dir = os.path.dirname(os.path.abspath(file_path))
            output_file = os.path.join(output_dir, f'extract-{index}-{range_val}.log')
            
            with open(output_file, 'w') as f:
                f.writelines(lines[extract_start:extract_end])
            
        except ValueError:
            print("Error: Invalid parameters")
            
    except FileNotFoundError:
        print(f"Error: File {file_path} not found")
    except Exception as e:
        print(f"Error: {str(e)}")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python script.py file-name index range")
    else:
        extract_ip_blocks(sys.argv[1], sys.argv[2], sys.argv[3])
        print(count_ip_lines(sys.argv[1]))