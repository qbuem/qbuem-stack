# hardware_io

**Category:** Hardware
**File:** `hardware_io_example.cpp`
**Complexity:** Expert

## Overview

Demonstrates qbuem-stack's advanced hardware integration layer: `PCIeDevice` (VFIO userspace I/O), `RDMAChannel` (RDMA/RoCE zero-copy networking), `EBPFTracer` (kernel eBPF observability), `NVMeIO` (SPDK NVMe direct I/O), and `kTLS` (kernel-offloaded TLS encryption).

## Scenario

A high-performance storage server uses:
- PCIe VFIO to directly drive an NVMe card from userspace (bypass kernel driver).
- RDMA for zero-copy data transfer between nodes.
- eBPF to capture kernel-level I/O events without modifying the kernel.
- NVMe SPDK for submitting I/O commands directly to the NVMe submission queue.
- kTLS to offload TLS record encryption to the NIC.

## Architecture Diagram

```
  PCIe VFIO (userspace DMA)
  ──────────────────────────────────────────────────────────
  PCIeDevice::open(bdf="0000:01:00.0")
  ├─ PCIeDevice::map_bar(0)          → MMIO register space
  ├─ PCIeDevice::alloc_dma(size)     → DMA-able buffer
  └─ PCIeDevice::setup_msix(vec, cb) → MSI-X interrupt

  RDMA Channel
  ──────────────────────────────────────────────────────────
  RDMAChannel::create("mlx5_0")
  ├─ rdma.reg_mr(buf, size)  → Memory Region
  ├─ rdma.post_send(mr)      → one-sided RDMA Write
  └─ rdma.post_recv(mr)      → one-sided RDMA Read

  eBPF Tracer
  ──────────────────────────────────────────────────────────
  EBPFTracer::load("io_tracer.bpf.o")
  └─ EBPFTracer::attach_kprobe("blk_mq_submit_bio")
     → triggers callback on every block I/O submission

  NVMe / SPDK
  ──────────────────────────────────────────────────────────
  NVMeIO::open("0000:01:00.0")
  ├─ nvme.write(lba, buf, len) → Submit Write command
  └─ nvme.read(lba, buf, len)  → Submit Read command

  kTLS (Kernel TLS offload)
  ──────────────────────────────────────────────────────────
  kTLS::configure(fd, crypto_info)
  └─ sends encrypted data via kernel TLS_TX socket option
     (NIC offloads AES-GCM encryption)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `PCIeDevice::open(bdf)` | Open PCIe device via VFIO |
| `PCIeDevice::map_bar(n)` | Map BAR region (MMIO) |
| `PCIeDevice::alloc_dma(size)` | Allocate DMA-coherent buffer |
| `RDMAChannel::create(device)` | Open RDMA channel |
| `rdma.post_send(mr)` | Submit RDMA send |
| `EBPFTracer::load(path)` | Load eBPF object file |
| `tracer.attach_kprobe(func)` | Attach kprobe |
| `NVMeIO::open(bdf)` | Open NVMe device via SPDK |
| `nvme.write(lba, buf, len)` | DMA write to NVMe |
| `kTLS::configure(fd, info)` | Configure kernel TLS offload |

## Requirements

| Feature | Requirement |
|---------|------------|
| PCIe VFIO | `modprobe vfio-pci`, device bound to vfio-pci driver |
| RDMA | Mellanox / Intel RDMA-capable NIC, `rdma-core` |
| eBPF | Linux 5.8+, CAP_BPF or root |
| NVMe SPDK | SPDK library, NVMe device unbound from kernel driver |
| kTLS | Linux 4.13+, `modprobe tls`, TLS 1.3 |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hardware_io_example

# Requires appropriate hardware and privileges
sudo ./build/examples/10-hardware/hardware_io/hardware_io_example
```

## Notes

- This example is designed for bare-metal servers; cloud VMs typically do not expose PCIe pass-through or RDMA.
- Each subsystem (VFIO, RDMA, eBPF, SPDK, kTLS) can be used independently.
- Mock implementations are used when real hardware is unavailable, allowing compilation and functional testing without hardware.
