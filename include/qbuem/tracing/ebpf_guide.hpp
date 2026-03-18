#pragma once

/**
 * @file qbuem/tracing/ebpf_guide.hpp
 * @brief eBPF tracing guide — io_uring tracepoints, tcp_sendmsg kprobe
 * @defgroup qbuem_ebpf eBPF Tracing Guide
 * @ingroup qbuem_tracing
 *
 * @note This header documents eBPF tracing approaches without implementation code.
 *       Actual eBPF programs are run separately using bpftrace(1) or libbpf.
 *
 * ## eBPF Tracing Overview
 *
 * Two main eBPF approaches for qbuem-stack performance analysis:
 *
 * ### 1. io_uring Tracepoints
 *
 * On Linux kernel 5.0+, io_uring provides the following tracepoints:
 * ```
 * io_uring:io_uring_create         — io_uring initialization
 * io_uring:io_uring_submit_sqe     — SQE submission
 * io_uring:io_uring_complete       — CQE completion
 * io_uring:io_uring_queue_async_work — kernel async work queue entry
 * io_uring:io_uring_poll_arm        — POLL_ADD SQE registration
 * io_uring:io_uring_task_work_run   — task_work execution (completion)
 * ```
 *
 * #### bpftrace example: SQE/CQE latency measurement
 * ```bpftrace
 * #!/usr/bin/env bpftrace
 * // io_uring SQE submission → CQE completion latency
 * tracepoint:io_uring:io_uring_submit_sqe {
 *     @submit_time[args->user_data] = nsecs;
 * }
 * tracepoint:io_uring:io_uring_complete {
 *     if (@submit_time[args->user_data]) {
 *         @latency_us = hist((nsecs - @submit_time[args->user_data]) / 1000);
 *         delete(@submit_time[args->user_data]);
 *     }
 * }
 * ```
 *
 * #### bpftrace example: SQE throughput per second
 * ```bpftrace
 * tracepoint:io_uring:io_uring_complete { @cqe_per_sec = count(); }
 * interval:s:1 { print(@cqe_per_sec); clear(@cqe_per_sec); }
 * ```
 *
 * ### 2. tcp_sendmsg kprobe
 *
 * Measuring TCP send latency and byte count:
 * ```bpftrace
 * #!/usr/bin/env bpftrace
 * kprobe:tcp_sendmsg {
 *     @start[tid] = nsecs;
 * }
 * kretprobe:tcp_sendmsg {
 *     if (@start[tid]) {
 *         @send_latency_us = hist((nsecs - @start[tid]) / 1000);
 *         delete(@start[tid]);
 *     }
 * }
 * ```
 *
 * ### 3. MSG_ZEROCOPY completion tracking
 *
 * `send(..., MSG_ZEROCOPY)` completion is confirmed via errqueue;
 * eBPF can track the point when errqueue returns:
 * ```bpftrace
 * kprobe:skb_copy_ubufs / kprobe:skb_zcopy_clear {
 *     @zerocopy_completions = count();
 * }
 * ```
 *
 * ### 4. How to run
 *
 * ```bash
 * # Install bpftrace (Ubuntu)
 * sudo apt install bpftrace
 *
 * # io_uring latency profile (targeting qbuem process)
 * sudo bpftrace -p $(pgrep qbuem_server) /path/to/iouring_latency.bt
 *
 * # Or system-wide
 * sudo bpftrace /path/to/iouring_latency.bt
 *
 * # Check io_uring tracepoints with perf
 * sudo perf list | grep io_uring
 * sudo perf stat -e 'io_uring:*' -p $(pgrep qbuem_server)
 * ```
 *
 * ### 5. Combining with PerfCounters
 *
 * Combining qbuem's `PerfCounters` (core/numa.hpp) with eBPF:
 * 1. `PerfCounters::start()` → processing begins
 * 2. Actual processing
 * 3. `PerfCounters::stop()` → measure cycles, IPC, LLC-miss
 * 4. eBPF → measure kernel-level io_uring latency
 * 5. Combine both sources to achieve full latency breakdown
 *
 * @{
 */

// This header contains guide documentation only, without implementation code.
// Manage actual eBPF programs as separate .bt or BPF C files.

/** @} */
