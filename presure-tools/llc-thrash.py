import numpy as np
import time
import argparse
import os

def get_llc_size():
   """
   Get system Last Level Cache (L3 Cache) size
   Returns size in MB
   """
   # Method 1: Try to get from lscpu
   try:
       lscpu_output = os.popen('lscpu').read()
       for line in lscpu_output.split('\n'):
           if 'L3 cache' in line:
               # Format is usually "L3 cache:                   XXXK/M"
               size_str = line.split(':')[1].strip()
               if 'M' in size_str:
                   return int(size_str.split('M')[0])
               elif 'K' in size_str:
                   return int(size_str.split('K')[0]) // 1024
   except:
       pass

   # Method 2: Try to read directly from /sys/devices
   try:
       index = 3  # L3 cache
       cache_size_file = f"/sys/devices/system/cpu/cpu0/cache/index{index}/size"
       with open(cache_size_file, 'r') as f:
           size_str = f.read().strip()
           if 'M' in size_str:
               return int(size_str.split('M')[0])
           elif 'K' in size_str:
               return int(size_str.split('K')[0]) // 1024
   except:
       pass

   # If all methods fail, return default value
   print("Warning: Could not detect LLC size, using default 16MB")
   return 16

def cache_thrashing_benchmark(size_ratio=1.0, iterations=1000):
   """
   Perform cache thrashing benchmark
   Args:
       size_ratio (float): Array size ratio relative to LLC size
       iterations (int): Number of iterations to access the array
   """
   llc_size_mb = get_llc_size()
   array_size_mb = llc_size_mb * size_ratio
   
   # Calculate array size in elements
   array_size = int((array_size_mb * 1024 * 1024) // np.dtype(np.int32).itemsize)
   array = np.zeros(array_size, dtype=np.int32)
   
   print(f"System LLC size: {llc_size_mb}MB")
   print(f"Running benchmark with array size: {array_size_mb}MB ({array_size} elements)")
   print(f"Array size ratio to LLC: {size_ratio:.2f}")

   start_time = time.time()
   for iteration in range(iterations):
       array += 1
       if iteration % (iterations // 10) == 0:
           print(f"Progress: {iteration/iterations*100:.1f}%")
           
   elapsed_time = time.time() - start_time
   print(f"Benchmark completed in {elapsed_time:.3f} seconds")
   print(f"Average time per iteration: {elapsed_time/iterations*1000:.3f} ms")
   print(f"Memory bandwidth: {(array_size_mb * iterations) / elapsed_time:.2f} MB/s")

if __name__ == "__main__":
   parser = argparse.ArgumentParser(description="Cache-thrashing microbenchmark")
   parser.add_argument("--ratio", type=float, default=1.0,
                     help="Array size ratio relative to LLC size (default: 1.0)")
   parser.add_argument("--iterations", type=int, default=1000,
                     help="Number of iterations to run (default: 1000)")
   parser.add_argument("--cpu", type=int, default=0,
                     help="CPU core to pin the process (default: 0)")
   args = parser.parse_args()
   cpu_mask = format(1 << args.cpu, 'x')

   # Pin process to specified CPU core
   os.system(f"taskset -p {cpu_mask} {os.getpid()} > /dev/null 2>&1")
   
   # Run benchmark
   cache_thrashing_benchmark(args.ratio, args.iterations)