import torch
import time
import argparse
from threading import Thread

class GPUStressTest:
    def __init__(self, target_util=50, gpu_id=0):
        # Initialize CUDA context first
        torch.cuda.init()
        torch.cuda.set_device(gpu_id)
        torch.zeros(1, device=f'cuda:{gpu_id}').normal_()
        
        self.target_util = target_util
        self.device = f'cuda:{gpu_id}'
        self.running = False
        
        # Matrix size for computation
        self.size = 1800
        self.tensor1 = torch.randn(self.size, self.size, device=self.device, dtype=torch.float32)
        self.tensor2 = torch.randn(self.size, self.size, device=self.device, dtype=torch.float32)
        self.result = torch.zeros(self.size, self.size, device=self.device, dtype=torch.float32)
        
        # Warmup
        for _ in range(5):
            self.compute_cycle()

    def compute_cycle(self):
        for _ in range(2):
            torch.mm(self.tensor1, self.tensor2, out=self.result)
            torch.mm(self.result, self.tensor1, out=self.tensor2)
        torch.cuda.synchronize()

    def run_with_target_utilization(self):
        self.running = True
        
        if self.target_util == 100:
            # For 100% utilization, compute continuously
            while self.running:
                self.compute_cycle()
        else:
            # For other utilization levels, use time-based control
            work_ratio = self.target_util / 100.0
            cycle_time = 0.05  # 50ms cycle time
            
            while self.running:
                cycle_start = time.time()
                
                # Computation phase
                compute_end_time = cycle_start + (cycle_time * work_ratio)
                while time.time() < compute_end_time and self.running:
                    self.compute_cycle()
                
                # Sleep phase
                sleep_time = cycle_start + cycle_time - time.time()
                if sleep_time > 0 and self.running:
                    time.sleep(sleep_time)

    def start(self):
        self.thread = Thread(target=self.run_with_target_utilization)
        self.thread.start()

    def stop(self):
        self.running = False
        if hasattr(self, 'thread'):
            self.thread.join()
        torch.cuda.empty_cache()

def main():
    parser = argparse.ArgumentParser(description='GPU SM Utilization Controller')
    parser.add_argument('--utilization', type=int, default=50,
                      help='Target GPU SM utilization (0-100)')
    parser.add_argument('--gpu', type=int, default=0,
                      help='GPU device ID to use')
    parser.add_argument('--duration', type=int, default=60,
                      help='Duration of the test in seconds')
    
    args = parser.parse_args()
    
    if not 0 <= args.utilization <= 100:
        raise ValueError("Utilization must be between 0 and 100")
        
    print(f"Starting GPU stress test with {args.utilization}% target utilization")
    print(f"Running on GPU {args.gpu} for {args.duration} seconds")
    print("Initializing CUDA..." + (" (Full load mode)" if args.utilization == 100 else ""))
    
    try:
        stress_test = GPUStressTest(args.utilization, args.gpu)
        stress_test.start()
        
        start_time = time.time()
        while time.time() - start_time < args.duration:
            time.sleep(1)
            
        stress_test.stop()
        print("\nStress test completed successfully")
        
    except KeyboardInterrupt:
        print("\nTest interrupted by user")
        if 'stress_test' in locals():
            stress_test.stop()
    except Exception as e:
        print(f"\nError: {e}")

if __name__ == "__main__":
    main()