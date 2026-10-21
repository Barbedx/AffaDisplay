// TxGate: when TwaiLink stops transmitting to a panel that is not there.
//
// With the panel unplugged, every frame we send is retransmitted by the
// controller for ever: ~1400 bus errors a second, each an interrupt on the
// key core. The gate has to shut that off within 2 s of silence, and it must
// never shut on a panel that is talking, or the box goes deaf to a live panel.
// Both promises are timing over many samples, which is what these tests hold.
#include <unity.h>

#include "link/TxGate.h"

using affa::TxGate;

void setUp() {}
void tearDown() {}

// TwaiLink feeds the gate every 100 ms; this does the same.
struct Bus {
  uint32_t now = 1000;
  uint32_t rx = 0;
  uint32_t txFailed = 0;
  uint32_t tec = 0;
  bool     mayTx = true;

  // Nobody ACKs: an error-passive transmitter stops counting ACK errors,
  // so TEC climbs to 128 and sits there.
  void nobodyAcks() { tec = TxGate::TEC_PASSIVE; }

  TxGate::Act step(TxGate& g, uint32_t ms = 100)
  {
    now += ms;
    TxGate::Sample s;
    s.nowMs = now;
    s.tec = tec;
    s.txFailed = txFailed;
    s.rxFrames = rx;
    s.mayTx = mayTx;
    return g.tick(s);
  }

  // Steps of 100 ms for `ms`, returning the first action that was not None.
  TxGate::Act run(TxGate& g, uint32_t ms)
  {
    for (uint32_t t = 0; t < ms; t += 100) {
      const TxGate::Act a = step(g);
      if (a != TxGate::Act::None) return a;
    }
    return TxGate::Act::None;
  }
};

static void prime(TxGate& g, Bus& b) { (void)b.step(g, 0); }

// ---- shutting --------------------------------------------------------------

static void test_a_silent_bus_that_nobody_acks_shuts_within_two_seconds()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  // Not before 2 s of silence...
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.run(g, TxGate::NO_RX_MS - 100));
  TEST_ASSERT_FALSE(g.shut());
  // ...and at 2 s, once.
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.step(g));
  TEST_ASSERT_TRUE(g.shut());
  TEST_ASSERT_EQUAL(1u, g.shuts());
}

static void test_a_talking_panel_is_never_gated_however_bad_the_counters()
{
  // A panel that floods us while our ACK path is broken (rxErr 129 on the
  // bench): error passive, but heard. Shutting TX would only add deafness.
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  for (int i = 0; i < 300; ++i) {
    b.rx += 50;
    b.txFailed += 1;
    TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.step(g));
  }
  TEST_ASSERT_FALSE(g.shut());
}

static void test_a_quiet_bus_with_a_healthy_controller_stays_open()
{
  // Nothing heard, but nothing failing either: a panel that only listens, or
  // a bus between our own sends. Silence alone is not a fault.
  TxGate g; Bus b; prime(g, b);
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.run(g, 10000));
  TEST_ASSERT_FALSE(g.shut());
}

static void test_failed_frames_count_as_nobody_acking()
{
  TxGate g; Bus b; prime(g, b);
  (void)b.run(g, TxGate::NO_RX_MS);
  b.txFailed = 3;
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.step(g));
}

static void test_a_driver_reinstall_resetting_the_counter_is_not_a_failure()
{
  TxGate g; Bus b;
  b.txFailed = 40;
  prime(g, b);
  (void)b.run(g, TxGate::NO_RX_MS);
  b.txFailed = 0;                  // the controller's count starts again
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.step(g));
  TEST_ASSERT_FALSE(g.shut());
}

static void test_the_first_sample_is_the_baseline_not_a_verdict()
{
  // Booting with no panel: the 2 s count from the first sample, not from 0.
  TxGate g; Bus b;
  b.now = 50000;
  b.nobodyAcks();
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.step(g, 0));
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.step(g));
  TEST_ASSERT_FALSE(g.shut());
}

// ---- opening ---------------------------------------------------------------

static void test_the_first_frame_heard_opens_it_at_once()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.run(g, 3000));
  // Plugged back in: the panel talks first.
  b.rx += 1;
  TEST_ASSERT_EQUAL((int)TxGate::Act::Open, (int)b.step(g));
  TEST_ASSERT_FALSE(g.shut());
  TEST_ASSERT_FALSE(g.probing());
  TEST_ASSERT_EQUAL(TxGate::BACKOFF_MIN_MS, g.backoffMs());
}

static void test_it_reprobes_with_a_backoff_that_doubles_to_thirty_seconds()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();                           // nobody, for the whole test
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.run(g, 3000));

  uint32_t expect = TxGate::BACKOFF_MIN_MS;
  for (int round = 0; round < 8; ++round) {
    // Shut for exactly the backoff, then a probe.
    const uint32_t shutAt = b.now;
    TxGate::Act a = TxGate::Act::None;
    while (a == TxGate::Act::None) a = b.step(g);
    TEST_ASSERT_EQUAL((int)TxGate::Act::Open, (int)a);
    TEST_ASSERT_TRUE(g.probing());
    TEST_ASSERT_UINT32_WITHIN(100, expect, b.now - shutAt);
    // The probe hears nothing and shuts again, for twice as long.
    a = TxGate::Act::None;
    while (a == TxGate::Act::None) a = b.step(g);
    TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)a);
    expect = expect * 2 > TxGate::BACKOFF_MAX_MS ? TxGate::BACKOFF_MAX_MS : expect * 2;
    TEST_ASSERT_EQUAL(expect, g.backoffMs());
  }
  TEST_ASSERT_EQUAL(TxGate::BACKOFF_MAX_MS, g.backoffMs());
}

static void test_a_probe_that_is_heard_keeps_it_open_and_resets_the_backoff()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  (void)b.run(g, 3000);                          // shut
  (void)b.run(g, 2000);                          // probe opens
  TEST_ASSERT_TRUE(g.probing());
  b.rx += 3;                                     // somebody answered
  b.tec = 0;
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.run(g, 10000));
  TEST_ASSERT_FALSE(g.shut());
  TEST_ASSERT_FALSE(g.probing());
  TEST_ASSERT_EQUAL(TxGate::BACKOFF_MIN_MS, g.backoffMs());
}

static void test_a_probe_whose_frames_were_acked_stays_open()
{
  // A listener that never talks but does ACK, one frame every 300 ms. Each
  // ACKed frame takes only 1 off TEC, so the node is still error passive
  // when the probe ends, and for a few seconds after. TEC is falling: open
  // is right, then and afterwards.
  TxGate g; Bus b; prime(g, b);
  b.tec = 140;
  (void)b.run(g, 3000);
  (void)b.run(g, 2000);
  TEST_ASSERT_TRUE(g.probing());
  for (int i = 1; i <= 70; ++i) {
    if (i % 3 == 0) --b.tec;
    TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.step(g));
  }
  TEST_ASSERT_TRUE(b.tec < TxGate::TEC_PASSIVE);
  TEST_ASSERT_FALSE(g.shut());
  TEST_ASSERT_FALSE(g.probing());
}

static void test_a_probe_whose_tec_climbs_from_zero_shuts_again()
{
  // The abort may leave the controller error active with TEC low; the probe
  // then climbs it 8 per unACKed frame. Nobody, even below error passive.
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  (void)b.run(g, 3000);
  b.tec = 0;
  (void)b.run(g, 2000);
  TEST_ASSERT_TRUE(g.probing());
  b.tec = 64;
  TxGate::Act a = TxGate::Act::None;
  for (int i = 0; i < 40 && a == TxGate::Act::None; ++i) a = b.step(g);
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)a);
}

static void test_a_probe_with_a_low_flat_tec_stays_open()
{
  // TEC low and flat through the probe: nothing was sent, or all of it was
  // ACKed. Nothing failed. (REC is not an input: stuck high from an old
  // fault, it cannot fall with nobody talking, and says nothing about us.)
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  (void)b.run(g, 3000);
  b.tec = 0;
  (void)b.run(g, 2000);
  TEST_ASSERT_TRUE(g.probing());
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.run(g, TxGate::PROBE_MS + 500));
  TEST_ASSERT_FALSE(g.shut());
  TEST_ASSERT_FALSE(g.probing());
}

static void test_a_probe_stuck_at_error_passive_shuts_early()
{
  // Panel unplugged: TEC sits at 128 and nothing is ACKed. Each probe is a
  // retransmit storm on the key core, so it ends after PROBE_STUCK_MS, not
  // after the whole PROBE_MS window.
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.run(g, 3000));
  TEST_ASSERT_EQUAL((int)TxGate::Act::Open, (int)b.run(g, 2000));
  TEST_ASSERT_TRUE(g.probing());
  const uint32_t openAt = b.now;
  TxGate::Act a = TxGate::Act::None;
  while (a == TxGate::Act::None && b.now - openAt < TxGate::PROBE_MS) a = b.step(g);
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)a);
  TEST_ASSERT_UINT32_WITHIN(100, TxGate::PROBE_STUCK_MS, b.now - openAt);
  TEST_ASSERT_TRUE(b.now - openAt < TxGate::PROBE_MS);
}

static void test_failed_frames_in_a_probe_shut_it_at_once()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  (void)b.run(g, 3000);
  b.tec = 0;
  TEST_ASSERT_EQUAL((int)TxGate::Act::Open, (int)b.run(g, 2000));
  b.txFailed += 1;
  TEST_ASSERT_EQUAL((int)TxGate::Act::Shut, (int)b.step(g));
}

static void test_listen_only_opens_it_and_forgets()
{
  TxGate g; Bus b; prime(g, b);
  b.nobodyAcks();
  (void)b.run(g, 3000);
  TEST_ASSERT_TRUE(g.shut());
  b.mayTx = false;                               // the web switched to listen-only
  TEST_ASSERT_EQUAL((int)TxGate::Act::Open, (int)b.step(g));
  TEST_ASSERT_EQUAL((int)TxGate::Act::None, (int)b.run(g, 10000));
  TEST_ASSERT_FALSE(g.shut());
}

int main(int, char**)
{
  UNITY_BEGIN();
  RUN_TEST(test_a_silent_bus_that_nobody_acks_shuts_within_two_seconds);
  RUN_TEST(test_a_talking_panel_is_never_gated_however_bad_the_counters);
  RUN_TEST(test_a_quiet_bus_with_a_healthy_controller_stays_open);
  RUN_TEST(test_failed_frames_count_as_nobody_acking);
  RUN_TEST(test_a_driver_reinstall_resetting_the_counter_is_not_a_failure);
  RUN_TEST(test_the_first_sample_is_the_baseline_not_a_verdict);
  RUN_TEST(test_the_first_frame_heard_opens_it_at_once);
  RUN_TEST(test_it_reprobes_with_a_backoff_that_doubles_to_thirty_seconds);
  RUN_TEST(test_a_probe_that_is_heard_keeps_it_open_and_resets_the_backoff);
  RUN_TEST(test_a_probe_whose_frames_were_acked_stays_open);
  RUN_TEST(test_a_probe_whose_tec_climbs_from_zero_shuts_again);
  RUN_TEST(test_a_probe_with_a_low_flat_tec_stays_open);
  RUN_TEST(test_a_probe_stuck_at_error_passive_shuts_early);
  RUN_TEST(test_failed_frames_in_a_probe_shut_it_at_once);
  RUN_TEST(test_listen_only_opens_it_and_forgets);
  return UNITY_END();
}
