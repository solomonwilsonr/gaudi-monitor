"""
hpu_benchmark.py — Multi-HPU training benchmark for Intel Gaudi
Runs ResNet-50 training with synthetic data across all available HPUs.
Uses torch.multiprocessing so each HPU gets its own process.

Run inside Habana Docker container:
  python3 hpu_benchmark.py [--num-hpus N] [--batch-size N] [--steps N]
"""

import argparse
import time
import torch
import torch.nn as nn
import torch.multiprocessing as mp

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--num-hpus",   type=int, default=8,   help="Number of HPUs to use (default: 8)")
    p.add_argument("--batch-size", type=int, default=64,  help="Batch size per HPU (default: 64)")
    p.add_argument("--steps",      type=int, default=100, help="Training steps per HPU (default: 100)")
    p.add_argument("--model",      type=str, default="resnet50", choices=["resnet50", "resnet101", "vgg16"],
                   help="Model to benchmark (default: resnet50)")
    return p.parse_args()

def build_model(name):
    import torchvision.models as models
    return {
        "resnet50":  models.resnet50,
        "resnet101": models.resnet101,
        "vgg16":     models.vgg16,
    }[name](weights=None)

def train_on_hpu(rank, args, result_queue):
    try:
        import habana_frameworks.torch.core as htcore
    except ImportError:
        print(f"[HPU {rank}] ERROR: habana_frameworks not found — are you inside the Habana container?")
        result_queue.put((rank, 0, 0))
        return

    device = torch.device(f"hpu:{rank}")

    model = build_model(args.model).to(device)
    optimizer = torch.optim.SGD(model.parameters(), lr=0.1, momentum=0.9)
    criterion = nn.CrossEntropyLoss()

    # Synthetic ImageNet-sized data
    images = torch.randn(args.batch_size, 3, 224, 224, device=device)
    labels = torch.randint(0, 1000, (args.batch_size,), device=device)

    model.train()

    # Warmup
    for _ in range(3):
        out = model(images)
        loss = criterion(out, labels)
        loss.backward()
        optimizer.step()
        optimizer.zero_grad()
        htcore.mark_step()

    # Timed benchmark
    torch.hpu.synchronize(device)
    t0 = time.time()

    for step in range(args.steps):
        out = model(images)
        loss = criterion(out, labels)
        loss.backward()
        optimizer.step()
        optimizer.zero_grad()
        htcore.mark_step()

        if (step + 1) % 10 == 0:
            torch.hpu.synchronize(device)
            elapsed = time.time() - t0
            imgs_per_sec = args.batch_size * (step + 1) / elapsed
            print(f"  [HPU {rank}] step {step+1:3d}/{args.steps}  "
                  f"loss={loss.item():.4f}  "
                  f"throughput={imgs_per_sec:.1f} img/s")

    torch.hpu.synchronize(device)
    total_time = time.time() - t0
    throughput = args.batch_size * args.steps / total_time

    result_queue.put((rank, throughput, total_time))

def main():
    args = parse_args()

    print(f"\n{'='*60}")
    print(f"  Gaudi HPU Benchmark")
    print(f"  Model:      {args.model}")
    print(f"  HPUs:       {args.num_hpus}")
    print(f"  Batch size: {args.batch_size} per HPU")
    print(f"  Steps:      {args.steps}")
    print(f"  Total imgs: {args.batch_size * args.steps * args.num_hpus:,} across all HPUs")
    print(f"{'='*60}\n")

    result_queue = mp.Queue()
    processes = []

    t_start = time.time()

    for rank in range(args.num_hpus):
        p = mp.Process(target=train_on_hpu, args=(rank, args, result_queue))
        p.start()
        processes.append(p)

    for p in processes:
        p.join()

    total_wall = time.time() - t_start

    print(f"\n{'='*60}")
    print(f"  Results")
    print(f"{'='*60}")

    results = []
    while not result_queue.empty():
        results.append(result_queue.get())
    results.sort()

    total_throughput = 0
    for rank, throughput, elapsed in results:
        print(f"  HPU {rank}: {throughput:7.1f} img/s  ({elapsed:.1f}s)")
        total_throughput += throughput

    print(f"{'─'*60}")
    print(f"  Total:  {total_throughput:7.1f} img/s across {args.num_hpus} HPUs")
    print(f"  Wall time: {total_wall:.1f}s")
    print(f"{'='*60}\n")

if __name__ == "__main__":
    mp.set_start_method("spawn")
    main()
