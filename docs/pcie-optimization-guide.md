# qbuem-stack: PCIe & Hardware-Native Optimization Guide

This guide details the "Extreme Performance" strategies for direct hardware interaction, targeting sub-microsecond latency and PCIe-line-rate throughput.

## 1. User-Space PCIe (VFIO)

### 1.1 The VFIO Advantage
- **Kernel Bypass**: Use the Linux VFIO driver to expose PCIe devices directly to user-space.
- **MMIO Access**: Map Base Address Registers (BAR) using `mmap()`. `qbuem::embedded::PCIeDevice` provides a type-safe wrapper for this.
- **Interrupts via eventfd**: MSI-X interrupts are mapped to `eventfd`, allowing the `IReactor` to monitor hardware events alongside sockets.

---

## 2. Peer-to-Peer DMA (P2PDMA)

The ultimate "Zero-Copy" where data never touches system RAM.
- **Strategy**: Enable `CONFIG_PCI_P2PDMA` and use `DMA-BUF`.
- **Use Case**: Direct data transfer from a 100GbE NIC to an NVMe drive or GPU.
- **Benefit**: Reduces memory bus contention and eliminates CPU/RAM latency.

---

## 3. CXL: The Cache-Coherent Future

### 3.1 Compute Express Link (CXL) Integration
- **Concept**: CXL.io (PCIe-like), CXL.cache (Coherent access), and CXL.mem (Memory pooling).
- **Strategy**: Leverage CXL 2.0+ memory pooling for "stranded memory" utilization.
- **Benefit**: Hardware-managed consistency between CPU and Accelerators (FPGA/NPU), removing software synchronization overhead.

---

## 4. Interrupt & CPU Affinity

### 4.1 MSI-X IRQ Affinity
- **Strategy**: Pin MSI-X interrupt lines to the same CPU cores assigned to the `qbuem::Reactor`.
- **Mechanism**: Disable `irqbalance` and manually set `/proc/irq/N/smp_affinity`.
- **Benefit**: Maximizes L3 cache locality. The core that receives the hardware completion signal is the same core that resumes the coroutine.

### 4.2 IOMMU Optimization
- **Strategy**: Use IOMMU "Passthrough" mode where possible, or pre-map large `HugePage` regions to the IOMMU translation table.
- **Benefit**: Reduces TLB miss penalties during high-frequency DMA operations.

---

## 5. Hardware Implementation Priorities

| Feature | Technique | Milestone |
| :--- | :--- | :--- |
| **User-space** | VFIO + BAR Mapping | v2.4.0 (Core Refinement) |
| **Locality** | MSI-X IRQ Affinity | v2.4.0 (Core Refinement) |
| **Zero-Copy** | P2PDMA (NIC -> NVMe) | v3.0.0 (Ultimate Stack) |
| **Coherency** | CXL Shared Memory | v3.0.0 (Ultimate Stack) |
