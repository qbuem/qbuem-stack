/**
 * @file shm_channel_example.cpp
 * @brief SHMChannel<T> + SHMBus — shared memory channel usage example.
 *
 * Coverage:
 * - SHMChannel<T>::create / try_send / try_recv / close / is_open
 * - SHMChannel<T>::size_approx / capacity
 * - SHMBus declare / try_publish / subscribe (LOCAL_ONLY)
 * - ISubscription try_recv / topic / scope
 * - calc_segment_size verification
 */

#include <qbuem/shm/shm_bus.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <cassert>
#include <cstring>
#include <qbuem/compat/print.hpp>

using namespace qbuem::shm;
using std::println;

// ─── 1. Direct SHMChannel usage (anonymous shm within single process) ─────────

struct SensorReading {
    float  temperature{0.f};
    float  humidity{0.f};
    uint32_t sensor_id{0};
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

static void demo_shm_channel() {
    println("[SHMChannel] create + try_send/try_recv demo");

    // Create channel (ring buffer with 8 slots — shm_open name: /test_sensor_ch)
    auto res = SHMChannel<SensorReading>::create("test_sensor_ch", 8);
    assert(res && "SHMChannel::create failed");

    auto& ch = *res;
    assert(ch->is_open());
    assert(ch->capacity() >= 8u);
    assert(ch->size_approx() == 0u);

    // try_send 3 items
    for (uint32_t i = 0; i < 3; ++i) {
        SensorReading r{20.f + static_cast<float>(i), 55.f, i};
        bool ok = ch->try_send(r);
        assert(ok);
    }
    println("[SHMChannel] sent 3, size_approx={}", ch->size_approx());

    // try_recv 3 items
    for (uint32_t i = 0; i < 3; ++i) {
        auto item = ch->try_recv();
        assert(item.has_value());
        const SensorReading* p = *item;
        assert(p->sensor_id == i);
        println("[SHMChannel] recv id={} temp={:.1f}", p->sensor_id, p->temperature);
    }
    assert(ch->size_approx() == 0u);

    // try_recv on empty channel → nullopt
    assert(!ch->try_recv().has_value());

    // Close channel
    ch->close();
    assert(!ch->is_open());
    assert(!ch->try_send(SensorReading{}));  // closed → false

    // Cleanup: remove /dev/shm entry via SHMChannel::unlink()
    auto ul = SHMChannel<SensorReading>::unlink("test_sensor_ch");
    assert(ul.has_value());
    println("[SHMChannel] OK");
}

// ─── 2. SHMBus (LOCAL_ONLY topic) ─────────────────────────────────────────────

struct OrderEvent {
    int    order_id;
    double price;
    int    qty;
};
static_assert(std::is_trivially_copyable_v<OrderEvent>);

static void demo_shm_bus() {
    println("[SHMBus] declare + try_publish + subscribe demo");

    SHMBus bus;

    // Declare topic
    bool ok = bus.declare<OrderEvent>("trading.orders", TopicScope::LOCAL_ONLY, 64);
    assert(ok);
    assert(bus.topic_count() == 1u);
    assert(bus.has_topic("trading.orders"));

    // Duplicate declaration → false
    assert(!bus.declare<OrderEvent>("trading.orders", TopicScope::LOCAL_ONLY, 64));

    // Subscribe
    auto sub = bus.subscribe<OrderEvent>("trading.orders");
    assert(sub != nullptr);
    assert(sub->topic() == "trading.orders");
    assert(sub->scope() == TopicScope::LOCAL_ONLY);

    // Publish
    OrderEvent ev1{1001, 250.5, 100};
    assert(bus.try_publish("trading.orders", ev1));

    OrderEvent ev2{1002, 251.0, 200};
    assert(bus.try_publish("trading.orders", ev2));

    // Subscriber receive
    auto r1 = sub->try_recv();
    assert(r1.has_value());
    assert((*r1)->order_id == 1001);
    println("[SHMBus] recv order_id={} price={:.1f}",
                (*r1)->order_id, (*r1)->price);

    auto r2 = sub->try_recv();
    assert(r2.has_value());
    assert((*r2)->order_id == 1002);

    // Empty channel
    assert(!sub->try_recv().has_value());

    println("[SHMBus] OK");
}

// ─── 3. calc_segment_size verification ────────────────────────────────────────

static void demo_calc_segment_size() {
    println("[calc_segment_size] verification demo");

    // Minimum size is at least one page (4096 bytes)
    size_t s = calc_segment_size(4, 8, false);
    assert(s >= 4096u);
    assert(s % 4096u == 0u);  // page-aligned

    // Larger capacity → larger size
    size_t s1 = calc_segment_size(64,  64, false);
    size_t s2 = calc_segment_size(128, 64, false);
    assert(s2 > s1);

    // Adding envelope increases size by at least capacity*128B
    size_t sw  = calc_segment_size(16, 64, false);
    size_t swe = calc_segment_size(16, 64, true);
    assert(swe >= sw + 16 * 128u);

    println("[calc_segment_size] s={} s1={} s2={} with_env={}",
                s, s1, s2, swe);
    println("[calc_segment_size] OK");
}

int main() {
    demo_shm_channel();
    demo_shm_bus();
    demo_calc_segment_size();
    println("shm_channel_example: ALL OK");
    return 0;
}
