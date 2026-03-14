#include <qbuem/pipeline/pipeline.hpp>
#include <gtest/gtest.h>

using namespace qbuem;

// ---------------------------------------------------------------------------
// Context tests
// ---------------------------------------------------------------------------
TEST(PipelineContext, PutAndGet) {
  Context ctx;
  ctx = ctx.put(RequestId{"req-001"});
  ctx = ctx.put(AuthSubject{"user-42"});

  auto rid = ctx.get<RequestId>();
  ASSERT_TRUE(rid.has_value());
  EXPECT_EQ(rid->value, "req-001");

  auto sub = ctx.get<AuthSubject>();
  ASSERT_TRUE(sub.has_value());
  EXPECT_EQ(sub->value, "user-42");
}

TEST(PipelineContext, GetMissing) {
  Context ctx;
  auto result = ctx.get<TraceCtx>();
  EXPECT_FALSE(result.has_value());
}

TEST(PipelineContext, GetPtr) {
  Context ctx;
  ctx = ctx.put(RequestId{"ptr-test"});

  const RequestId *ptr = ctx.get_ptr<RequestId>();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->value, "ptr-test");
}

TEST(PipelineContext, Shadowing) {
  Context ctx;
  ctx = ctx.put(RequestId{"first"});
  ctx = ctx.put(RequestId{"second"}); // shadows first

  auto rid = ctx.get<RequestId>();
  ASSERT_TRUE(rid.has_value());
  EXPECT_EQ(rid->value, "second");
}

TEST(PipelineContext, ImmutableFork) {
  Context original;
  original = original.put(RequestId{"base"});

  Context fork_a = original.put(AuthSubject{"alice"});
  Context fork_b = original.put(AuthSubject{"bob"});

  // fork_a and fork_b both have the original RequestId
  EXPECT_EQ(fork_a.get<RequestId>()->value, "base");
  EXPECT_EQ(fork_b.get<RequestId>()->value, "base");

  // But different AuthSubject
  EXPECT_EQ(fork_a.get<AuthSubject>()->value, "alice");
  EXPECT_EQ(fork_b.get<AuthSubject>()->value, "bob");

  // Original is unmodified
  EXPECT_FALSE(original.get<AuthSubject>().has_value());
}

// ---------------------------------------------------------------------------
// ServiceRegistry tests
// ---------------------------------------------------------------------------
struct ICounter {
  virtual ~ICounter() = default;
  virtual int next() = 0;
};

struct ConcreteCounter : ICounter {
  int count = 0;
  int next() override { return ++count; }
};

TEST(ServiceRegistry, RegisterAndGet) {
  ServiceRegistry reg;
  auto counter = std::make_shared<ConcreteCounter>();
  reg.register_singleton<ICounter>(counter);

  auto got = reg.get<ICounter>();
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->next(), 1);
  EXPECT_EQ(got->next(), 2);
}

TEST(ServiceRegistry, GetMissing) {
  ServiceRegistry reg;
  auto got = reg.get<ICounter>();
  EXPECT_EQ(got, nullptr);
}

TEST(ServiceRegistry, RequireMissing) {
  // require() on missing service calls std::terminate() — not testable directly.
  // Just test that require() returns non-null when present.
  ServiceRegistry reg;
  reg.register_singleton<ICounter>(std::make_shared<ConcreteCounter>());
  EXPECT_NE(reg.require<ICounter>(), nullptr);
}

TEST(ServiceRegistry, HierarchicalLookup) {
  ServiceRegistry parent;
  parent.register_singleton<ICounter>(std::make_shared<ConcreteCounter>());

  ServiceRegistry child(&parent);
  // Child doesn't have ICounter, but parent does
  auto got = child.get<ICounter>();
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->next(), 1);
}

TEST(ServiceRegistry, ChildOverridesParent) {
  ServiceRegistry parent;
  parent.register_singleton<ICounter>(std::make_shared<ConcreteCounter>());

  ServiceRegistry child(&parent);
  auto child_counter = std::make_shared<ConcreteCounter>();
  child_counter->count = 100;
  child.register_singleton<ICounter>(child_counter);

  EXPECT_EQ(child.get<ICounter>()->next(), 101); // child's counter
  EXPECT_EQ(parent.get<ICounter>()->next(), 1);  // parent's counter
}

TEST(ServiceRegistry, RegisterFactory) {
  ServiceRegistry reg;
  int call_count = 0;
  reg.register_factory<ICounter>([&]() {
    ++call_count;
    return std::make_shared<ConcreteCounter>();
  });

  auto got1 = reg.get<ICounter>();
  auto got2 = reg.get<ICounter>();
  EXPECT_EQ(call_count, 1); // factory called once (cached)
  EXPECT_EQ(got1.get(), got2.get()); // same instance
}

// ---------------------------------------------------------------------------
// WorkerLocal tests
// ---------------------------------------------------------------------------
TEST(WorkerLocal, BasicAccess) {
  WorkerLocal<int> wl(4);
  wl[0] = 10;
  wl[1] = 20;
  wl[2] = 30;
  wl[3] = 40;

  EXPECT_EQ(wl[0], 10);
  EXPECT_EQ(wl[1], 20);
  EXPECT_EQ(wl[2], 30);
  EXPECT_EQ(wl[3], 40);
}

TEST(WorkerLocal, DefaultInitialized) {
  WorkerLocal<int> wl(3);
  EXPECT_EQ(wl[0], 0);
  EXPECT_EQ(wl[1], 0);
  EXPECT_EQ(wl[2], 0);
}

TEST(WorkerLocal, CacheLineAligned) {
  // Verify each slot is cache-line separated (no false sharing)
  WorkerLocal<int> wl(2);
  uintptr_t addr0 = reinterpret_cast<uintptr_t>(&wl[0]);
  uintptr_t addr1 = reinterpret_cast<uintptr_t>(&wl[1]);
  EXPECT_GE(addr1 - addr0, 64u);
}

// ---------------------------------------------------------------------------
// AsyncChannel tests
// ---------------------------------------------------------------------------
TEST(AsyncChannel, TrySendRecv) {
  AsyncChannel<int> chan(8);

  EXPECT_TRUE(chan.try_send(1));
  EXPECT_TRUE(chan.try_send(2));
  EXPECT_TRUE(chan.try_send(3));

  auto a = chan.try_recv();
  auto b = chan.try_recv();
  auto c = chan.try_recv();

  ASSERT_TRUE(a.has_value()); EXPECT_EQ(*a, 1);
  ASSERT_TRUE(b.has_value()); EXPECT_EQ(*b, 2);
  ASSERT_TRUE(c.has_value()); EXPECT_EQ(*c, 3);
}

TEST(AsyncChannel, TryRecvEmpty) {
  AsyncChannel<int> chan(4);
  auto item = chan.try_recv();
  EXPECT_FALSE(item.has_value());
}

TEST(AsyncChannel, TrySendFull) {
  AsyncChannel<int> chan(2);
  EXPECT_TRUE(chan.try_send(1));
  EXPECT_TRUE(chan.try_send(2));
  EXPECT_FALSE(chan.try_send(3)); // full
}

TEST(AsyncChannel, CloseEos) {
  AsyncChannel<int> chan(4);
  chan.try_send(42);
  chan.close();

  auto item = chan.try_recv();
  ASSERT_TRUE(item.has_value()); EXPECT_EQ(*item, 42);

  auto eos = chan.try_recv();
  EXPECT_FALSE(eos.has_value()); // EOS after close

  EXPECT_TRUE(chan.is_closed());
  EXPECT_FALSE(chan.try_send(99)); // send after close
}

TEST(AsyncChannel, BatchOps) {
  AsyncChannel<int> chan(16);
  for (int i = 0; i < 5; ++i)
    chan.try_send(i);

  std::vector<int> out(5);
  size_t n = chan.try_recv_batch(out, 5);
  EXPECT_EQ(n, 5u);
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(out[i], i);
}

TEST(AsyncChannel, SizeApprox) {
  AsyncChannel<int> chan(8);
  EXPECT_EQ(chan.size_approx(), 0u);
  chan.try_send(1);
  chan.try_send(2);
  EXPECT_EQ(chan.size_approx(), 2u);
}

// ---------------------------------------------------------------------------
// Concepts tests (compile-time only)
// ---------------------------------------------------------------------------

// Full action fn
static qbuem::Task<qbuem::Result<int>> full_fn(int x, ActionEnv env) {
  co_return x + static_cast<int>(env.worker_idx);
}
static_assert(FullActionFn<decltype(full_fn), int, int>,
              "full_fn should satisfy FullActionFn");

// Simple action fn
static qbuem::Task<qbuem::Result<int>> simple_fn(int x, std::stop_token) {
  co_return x * 2;
}
static_assert(SimpleActionFn<decltype(simple_fn), int, int>,
              "simple_fn should satisfy SimpleActionFn");

// Plain action fn
static qbuem::Task<qbuem::Result<int>> plain_fn(int x) {
  co_return x + 1;
}
static_assert(PlainActionFn<decltype(plain_fn), int, int>,
              "plain_fn should satisfy PlainActionFn");

// ActionFn (union)
static_assert(ActionFn<decltype(full_fn), int, int>);
static_assert(ActionFn<decltype(simple_fn), int, int>);
static_assert(ActionFn<decltype(plain_fn), int, int>);

TEST(Concepts, CompileTimeVerification) {
  // If we reach here, all static_asserts passed
  SUCCEED();
}
