/**
 * @file shm_channel_example.cpp
 * @brief SHMChannel<T> + SHMBus — 공유 메모리 채널 사용 예시.
 *
 * 커버리지:
 * - SHMChannel<T>::create / try_send / try_recv / close / is_open
 * - SHMChannel<T>::size_approx / capacity
 * - SHMBus declare / try_publish / subscribe (LOCAL_ONLY)
 * - ISubscription try_recv / topic / scope
 * - calc_segment_size 검증
 */

#include <qbuem/shm/shm_bus.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace qbuem::shm;

// ─── 1. SHMChannel 직접 사용 (단일 프로세스 내 anonymous shm) ─────────────────

struct SensorReading {
    float  temperature{0.f};
    float  humidity{0.f};
    uint32_t sensor_id{0};
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

static void demo_shm_channel() {
    std::puts("[SHMChannel] create + try_send/try_recv demo");

    // 채널 생성 (링 버퍼 8슬롯 — shm_open 이름은 /test_sensor_ch)
    auto res = SHMChannel<SensorReading>::create("test_sensor_ch", 8);
    assert(res && "SHMChannel::create failed");

    auto& ch = *res;
    assert(ch->is_open());
    assert(ch->capacity() >= 8u);
    assert(ch->size_approx() == 0u);

    // try_send 3개
    for (uint32_t i = 0; i < 3; ++i) {
        SensorReading r{20.f + static_cast<float>(i), 55.f, i};
        bool ok = ch->try_send(r);
        assert(ok);
    }
    std::printf("[SHMChannel] sent 3, size_approx=%zu\n", ch->size_approx());

    // try_recv 3개
    for (uint32_t i = 0; i < 3; ++i) {
        auto item = ch->try_recv();
        assert(item.has_value());
        const SensorReading* p = *item;
        assert(p->sensor_id == i);
        std::printf("[SHMChannel] recv id=%u temp=%.1f\n", p->sensor_id, p->temperature);
    }
    assert(ch->size_approx() == 0u);

    // 빈 채널에서 try_recv → nullopt
    assert(!ch->try_recv().has_value());

    // 채널 닫기
    ch->close();
    assert(!ch->is_open());
    assert(!ch->try_send(SensorReading{}));  // closed → false

    // 정리: shm_open은 unlink 필요 (여기서는 데모만)
    ::shm_unlink("/test_sensor_ch");
    std::puts("[SHMChannel] OK");
}

// ─── 2. SHMBus (LOCAL_ONLY 토픽) ─────────────────────────────────────────────

struct OrderEvent {
    int    order_id;
    double price;
    int    qty;
};
static_assert(std::is_trivially_copyable_v<OrderEvent>);

static void demo_shm_bus() {
    std::puts("[SHMBus] declare + try_publish + subscribe demo");

    SHMBus bus;

    // 토픽 선언
    bool ok = bus.declare<OrderEvent>("trading.orders", TopicScope::LOCAL_ONLY, 64);
    assert(ok);
    assert(bus.topic_count() == 1u);
    assert(bus.has_topic("trading.orders"));

    // 중복 선언 → false
    assert(!bus.declare<OrderEvent>("trading.orders", TopicScope::LOCAL_ONLY, 64));

    // 구독
    auto sub = bus.subscribe<OrderEvent>("trading.orders");
    assert(sub != nullptr);
    assert(sub->topic() == "trading.orders");
    assert(sub->scope() == TopicScope::LOCAL_ONLY);

    // 발행
    OrderEvent ev1{1001, 250.5, 100};
    assert(bus.try_publish("trading.orders", ev1));

    OrderEvent ev2{1002, 251.0, 200};
    assert(bus.try_publish("trading.orders", ev2));

    // 구독자 수신
    auto r1 = sub->try_recv();
    assert(r1.has_value());
    assert((*r1)->order_id == 1001);
    std::printf("[SHMBus] recv order_id=%d price=%.1f\n",
                (*r1)->order_id, (*r1)->price);

    auto r2 = sub->try_recv();
    assert(r2.has_value());
    assert((*r2)->order_id == 1002);

    // 빈 채널
    assert(!sub->try_recv().has_value());

    std::puts("[SHMBus] OK");
}

// ─── 3. calc_segment_size 검증 ────────────────────────────────────────────────

static void demo_calc_segment_size() {
    std::puts("[calc_segment_size] verification demo");

    // 최소 크기는 페이지 크기(4096B) 이상
    size_t s = calc_segment_size(4, 8, false);
    assert(s >= 4096u);
    assert(s % 4096u == 0u);  // 페이지 정렬

    // 용량이 커질수록 크기도 커짐
    size_t s1 = calc_segment_size(64,  64, false);
    size_t s2 = calc_segment_size(128, 64, false);
    assert(s2 > s1);

    // envelope 추가 시 capacity*128B 이상 증가
    size_t sw  = calc_segment_size(16, 64, false);
    size_t swe = calc_segment_size(16, 64, true);
    assert(swe >= sw + 16 * 128u);

    std::printf("[calc_segment_size] s=%zu s1=%zu s2=%zu with_env=%zu\n",
                s, s1, s2, swe);
    std::puts("[calc_segment_size] OK");
}

int main() {
    demo_shm_channel();
    demo_shm_bus();
    demo_calc_segment_size();
    std::puts("shm_channel_example: ALL OK");
    return 0;
}
