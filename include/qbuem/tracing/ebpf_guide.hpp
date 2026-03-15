#pragma once

/**
 * @file qbuem/tracing/ebpf_guide.hpp
 * @brief eBPF 트레이싱 가이드 — io_uring tracepoints, tcp_sendmsg kprobe
 * @defgroup qbuem_ebpf eBPF Tracing Guide
 * @ingroup qbuem_tracing
 *
 * @note 이 헤더는 구현 코드 없이 eBPF 트레이싱 접근법을 문서화합니다.
 *       실제 eBPF 프로그램은 bpftrace(1) 또는 libbpf를 사용해 별도 실행합니다.
 *
 * ## eBPF 트레이싱 개요
 *
 * qbuem-stack 성능 분석을 위한 두 가지 주요 eBPF 접근법:
 *
 * ### 1. io_uring Tracepoints
 *
 * Linux 커널 5.0+에서 io_uring은 다음 tracepoint를 제공합니다:
 * ```
 * io_uring:io_uring_create         — io_uring 초기화
 * io_uring:io_uring_submit_sqe     — SQE 제출
 * io_uring:io_uring_complete       — CQE 완료
 * io_uring:io_uring_queue_async_work — 커널 비동기 워크 큐 진입
 * io_uring:io_uring_poll_arm        — POLL_ADD SQE 등록
 * io_uring:io_uring_task_work_run   — task_work 실행 (completion)
 * ```
 *
 * #### bpftrace 예시: SQE/CQE 레이턴시 측정
 * ```bpftrace
 * #!/usr/bin/env bpftrace
 * // io_uring SQE 제출 → CQE 완료 레이턴시
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
 * #### bpftrace 예시: 초당 SQE 처리량
 * ```bpftrace
 * tracepoint:io_uring:io_uring_complete { @cqe_per_sec = count(); }
 * interval:s:1 { print(@cqe_per_sec); clear(@cqe_per_sec); }
 * ```
 *
 * ### 2. tcp_sendmsg kprobe
 *
 * TCP 송신 레이턴시 및 바이트 수 측정:
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
 * ### 3. MSG_ZEROCOPY 완료 추적
 *
 * `send(..., MSG_ZEROCOPY)` 완료는 errqueue에서 확인하는데,
 * eBPF로 errqueue 반환 시점을 추적할 수 있습니다:
 * ```bpftrace
 * kprobe:skb_copy_ubufs / kprobe:skb_zcopy_clear {
 *     @zerocopy_completions = count();
 * }
 * ```
 *
 * ### 4. 실행 방법
 *
 * ```bash
 * # bpftrace 설치 (Ubuntu)
 * sudo apt install bpftrace
 *
 * # io_uring 레이턴시 프로파일 (qbuem 프로세스 대상)
 * sudo bpftrace -p $(pgrep qbuem_server) /path/to/iouring_latency.bt
 *
 * # 또는 시스템 전체
 * sudo bpftrace /path/to/iouring_latency.bt
 *
 * # perf로 io_uring tracepoint 확인
 * sudo perf list | grep io_uring
 * sudo perf stat -e 'io_uring:*' -p $(pgrep qbuem_server)
 * ```
 *
 * ### 5. PerfCounters와 결합
 *
 * qbuem의 `PerfCounters` (core/numa.hpp)와 eBPF를 결합하면:
 * 1. `PerfCounters::start()` → 처리 시작
 * 2. 실제 처리
 * 3. `PerfCounters::stop()` → cycles, IPC, LLC-miss 측정
 * 4. eBPF → 커널 레벨 io_uring 지연 측정
 * 5. 두 소스를 결합해 전체 레이턴시 분해(breakdown) 가능
 *
 * @{
 */

// 이 헤더는 구현 코드 없이 가이드 문서만 포함합니다.
// 실제 eBPF 프로그램은 별도 .bt 또는 BPF C 파일로 관리하세요.

/** @} */
