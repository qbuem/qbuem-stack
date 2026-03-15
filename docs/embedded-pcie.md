# Architecture: Embedded Support (PCIe, UDS, & SHM)

This document specifies how `qbuem-stack` addresses the requirements of high-performance embedded systems, specifically focusing on PCIe integration and advanced IPC (UDS/SHM).

---

## 1. PCIe Integration Strategy

In embedded and high-end backend systems, PCIe is the primary bus for FPGAs, SmartNICs, and NVMe drives. `qbuem-stack` integrates PCIe via the **Reactor/VFIO Bridge**.

### 1.1 The VFIO Pattern (User-space PCIe)
To maintain zero-dependency and high performance, we avoid standard kernel drivers and use **VFIO (Virtual Function I/O)**.

- **BAR Mapping (MMIO)**: The stack provides a `PCIeDevice` helper to `mmap(2)` the device's BAR (Base Address Registers) directly into the process.
- **Interrupts via `eventfd`**: PCIe interrupts (MSI-X) are mapped to an `eventfd` by the VFIO driver. 
- **Reactor Integration**: The `IReactor` monitors the `eventfd`. When the hardware signals an interrupt (e.g., DMA complete), the Reactor wakes the relevant coroutine.

### 1.2 PCIe Workflow in qbuem-stack
```cpp
// Mapping an FPGA DMA completion interrupt to a pipeline
auto pcie = co_await PCIeDevice::open("/dev/vfio/12");
auto irq_fd = pcie.get_irq_eventfd(0);

// In a Pipeline Action
co_await reactor->register_event(irq_fd, EventType::Read, [&]() {
    // Hardware signaled completion!
    handle.resume();
});
```

---

## 2. Unix Domain Sockets (UDS) vs. SHM

For local IPC, `qbuem-stack` provides two tiers:

| Feature | `UnixSocket` (UDS) | `SHMChannel` (Shared Memory) |
| :--- | :--- | :--- |
| **Interface** | Standard `read/write` | Zero-copy Ring Buffer |
| **Performance** | ~20us latency | < 100ns latency |
| **Compatibility** | High (Any UNIX app) | Low (qbuem-native or spec-compliant) |
| **Use Case** | Legacy system integration | High-freq process communication |

---

## 3. Embedded Zero-Copy: The "D-Bus" of qbuem-stack

By combining **PCIe MMIO** and **SHM Messaging**, `qbuem-stack` can act as a high-performance system bus:

1.  **Ingress (PCIe)**: Hardware writes data directly to a pre-allocated SHM block via DMA.
2.  **Notification (Reactor)**: PCIe interrupt triggers `eventfd` -> Reactor wakes up.
3.  **Forwarding (SHM)**: The Reactor broadcasts the *pointer* to the SHM block to other processes via `SHMChannel`.
4.  **Result**: 0 user-space copies from hardware to multiple consumer processes.

---

## 4. Implementation Targets

- [ ] `qbuem::embedded::PCIeDevice` helper class.
- [ ] DMA Memory Pool (Contiguous memory allocation for hardware).
- [ ] UDS `Descriptor Passing` (SCM_RIGHTS) support for handing over SHM file descriptors between processes.

---
