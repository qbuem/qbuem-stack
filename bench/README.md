# qbuem-stack Benchmarks

마이크로 벤치마크 모음입니다. 각 벤치마크는 `-O3 -march=native`로 빌드되어
실제 배포 환경의 성능을 측정합니다.

## 빌드

```sh
# 프로젝트 루트에서
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_http bench_router bench_arena bench_channel bench_pipeline -j$(nproc)
```

또는 모든 벤치마크를 한 번에:

```sh
cmake --build build --target run_bench
```

## 개별 실행

```sh
# HTTP 파서 처리량 (MB/s)
./build/bench/bench_http

# 라우터 룩업 레이턴시 (ns/req)
./build/bench/bench_router

# Arena 할당 속도 (ns/alloc)
./build/bench/bench_arena

# AsyncChannel 처리량 (ops/s)
./build/bench/bench_channel

# 3-스테이지 파이프라인 처리량 (items/s)
./build/bench/bench_pipeline
```

## 성능 목표 (v1.0)

| 벤치마크      | 지표          | 목표           | 설명                          |
|--------------|---------------|----------------|-------------------------------|
| HTTP 파서    | 처리량         | > 500 MB/s     | AVX2 활성화 시 ~1 GiB/s       |
| 라우터 룩업  | 레이턴시       | < 200 ns/req   | 1,000-라우트 Radix Tree       |
| Arena 할당   | 레이턴시       | < 5 ns/alloc   | bump-pointer, 힙 할당 없음    |
| AsyncChannel | 처리량         | > 10M ops/s    | MPMC ring-buffer              |
| Pipeline     | 처리량         | > 1M items/s   | 3-스테이지 체인               |

## 결과 해석 (p50 / p99)

벤치마크 출력에는 보통 다음 지표가 포함됩니다:

```
ops/sec   : 초당 처리 수 (높을수록 좋음)
p50 (ns)  : 중앙값 레이턴시. 일반적인 처리 속도를 나타냄.
p99 (ns)  : 99번째 백분위 레이턴시. 꼬리 레이턴시(tail latency).
            SLO 설계 시 p99를 기준으로 목표를 설정하세요.
p999 (ns) : 99.9번째 백분위. 극단적 지연 발생 빈도 확인용.
```

### 해석 가이드

- **p50 낮음 + p99 높음**: 꼬리 레이턴시 문제. GC pause, lock contention,
  or 시스템 인터럽트를 의심하세요.
- **p50 자체가 높음**: 핵심 처리 경로 병목. 프로파일러(perf, VTune)로
  핫스팟을 찾으세요.
- **ops/s 목표 미달**: CPU 코어 수, NUMA topology, 채널 용량(channel_cap)을
  조정해보세요.

## 측정 환경 기록 권장 사항

```
CPU:     Intel Xeon Gold 6254 @ 3.10GHz (18C/36T)
OS:      Ubuntu 22.04 LTS, kernel 5.15.0
Compiler: GCC 12.3, -O3 -march=native
Build:   Release (CMake)
Isolate: taskset -c 0 ./bench_xxx  (NUMA 고정)
```

재현성을 높이기 위해:
- `cpupower frequency-set -g performance`로 CPU 주파수를 고정하세요.
- `taskset -c <core>` 로 단일 코어에 고정하세요.
- 백그라운드 프로세스를 최소화하세요.
