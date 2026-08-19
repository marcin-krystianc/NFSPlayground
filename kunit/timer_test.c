// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the SunRPC round-trip time estimator in
 * net/sunrpc/timer.c, plus the timeout-counter helpers in
 * include/linux/sunrpc/timer.h.
 *
 * This is the Van Jacobson RTT/variance estimator described in appendix A
 * of "Congestion Avoidance and Control" (Jacobson and Karels, 1988), used
 * for RPC over datagram transports. It is pure integer arithmetic with no
 * I/O, no allocation and no locking.
 *
 * Note the timer index convention: rpc_update_rtt() and rpc_calc_rto()
 * both do "timer--", so caller-visible timer 1..5 selects array slot
 * 0..4, and timer 0 means "not a frequently issued RPC" and is handled
 * specially.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/sunrpc/timer.h>

/*
 * Mirrors the constants in net/sunrpc/timer.c, which are file-private.
 * Expressed in terms of HZ so the tests do not assume a tick rate.
 */
#define TEST_RTO_MAX	(60 * HZ)
#define TEST_RTO_INIT	(HZ / 5)
#define TEST_RTO_MIN	(HZ / 10)

#define RTT_SLOTS	5

/*
 * rpc_init_rtt()
 */

static void init_below_threshold_zeroes_srtt(struct kunit *test)
{
	struct rpc_rtt rt;
	unsigned int i;

	rpc_init_rtt(&rt, TEST_RTO_INIT);

	KUNIT_EXPECT_EQ(test, rt.timeo, (unsigned long)TEST_RTO_INIT);
	for (i = 0; i < RTT_SLOTS; i++) {
		KUNIT_EXPECT_EQ_MSG(test, rt.srtt[i], 0UL, "slot %u", i);
		KUNIT_EXPECT_EQ_MSG(test, rt.sdrtt[i],
				    (unsigned long)TEST_RTO_INIT, "slot %u", i);
		KUNIT_EXPECT_EQ_MSG(test, rt.ntimeouts[i], 0, "slot %u", i);
	}
}

/* Above the threshold the excess is pre-scaled by 8, since srtt is
 * stored shifted left by 3 (timer.c:43).
 */
static void init_above_threshold_scales_srtt(struct kunit *test)
{
	struct rpc_rtt rt;
	unsigned int i;

	rpc_init_rtt(&rt, TEST_RTO_INIT + 8);

	for (i = 0; i < RTT_SLOTS; i++)
		KUNIT_EXPECT_EQ_MSG(test, rt.srtt[i], 64UL, "slot %u", i);
}

/*
 * rpc_update_rtt()
 */

/* timer 0 is the "other RPC" case and must not touch any slot. */
static void update_timer_zero_is_noop(struct kunit *test)
{
	struct rpc_rtt rt;
	unsigned int i;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_update_rtt(&rt, 0, 4 * TEST_RTO_INIT);

	for (i = 0; i < RTT_SLOTS; i++) {
		KUNIT_EXPECT_EQ_MSG(test, rt.srtt[i], 0UL, "slot %u", i);
		KUNIT_EXPECT_EQ_MSG(test, rt.sdrtt[i],
				    (unsigned long)TEST_RTO_INIT, "slot %u", i);
	}
}

/* A negative sample means jiffies wrapped and is discarded (timer.c:69). */
static void update_discards_negative_sample(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_update_rtt(&rt, 1, -1);

	KUNIT_EXPECT_EQ(test, rt.srtt[0], 0UL);
	KUNIT_EXPECT_EQ(test, rt.sdrtt[0], (unsigned long)TEST_RTO_INIT);
}

/*
 * A zero sample is treated as 1 rather than discarded (timer.c:72), so
 * starting from srtt 0 it must leave srtt at exactly 1. This is what
 * distinguishes "clamped to 1" from "ignored".
 */
static void update_treats_zero_sample_as_one(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_update_rtt(&rt, 1, 0);

	KUNIT_EXPECT_EQ(test, rt.srtt[0], 1UL);
}

/*
 * From a zeroed estimator the first sample is adopted whole: srtt starts
 * at 0, so m - (srtt >> 3) == m, and srtt becomes exactly m.
 */
static void update_first_sample_adopted_whole(struct kunit *test)
{
	const long sample = 4 * TEST_RTO_INIT;
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_update_rtt(&rt, 1, sample);

	KUNIT_EXPECT_EQ(test, rt.srtt[0], (unsigned long)sample);
	KUNIT_EXPECT_EQ(test, rt.sdrtt[0],
			(unsigned long)(TEST_RTO_INIT + sample -
					(TEST_RTO_INIT >> 2)));
}

/*
 * srtt is the RTT shifted left by 3, so feeding a constant sample must
 * drive it to 8 * sample. This exercises the estimator as an algorithm
 * rather than re-deriving one update step.
 */
static void update_converges_to_eight_times_rtt(struct kunit *test)
{
	const long sample = 4 * TEST_RTO_INIT;
	struct rpc_rtt rt;
	unsigned int i;
	long drift;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	for (i = 0; i < 200; i++)
		rpc_update_rtt(&rt, 1, sample);

	drift = (long)rt.srtt[0] - 8 * sample;
	if (drift < 0)
		drift = -drift;
	KUNIT_EXPECT_LE_MSG(test, drift, 8L,
			    "srtt %lu did not converge to %ld",
			    rt.srtt[0], 8 * sample);
}

/*
 * Once the samples stop varying the deviation decays, but timer.c:87
 * floors it at RPC_RTO_MIN so the RTO never collapses to the mean.
 */
static void update_variance_has_lower_bound(struct kunit *test)
{
	const long sample = 4 * TEST_RTO_INIT;
	struct rpc_rtt rt;
	unsigned int i;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	for (i = 0; i < 200; i++)
		rpc_update_rtt(&rt, 1, sample);

	KUNIT_EXPECT_EQ(test, rt.sdrtt[0], (unsigned long)TEST_RTO_MIN);
}

/* Each request type keeps its own estimate. */
static void update_slots_are_independent(struct kunit *test)
{
	struct rpc_rtt rt;
	unsigned int i;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_update_rtt(&rt, 1, 4 * TEST_RTO_INIT);

	KUNIT_EXPECT_NE(test, rt.srtt[0], 0UL);
	for (i = 1; i < RTT_SLOTS; i++) {
		KUNIT_EXPECT_EQ_MSG(test, rt.srtt[i], 0UL, "slot %u", i);
		KUNIT_EXPECT_EQ_MSG(test, rt.sdrtt[i],
				    (unsigned long)TEST_RTO_INIT, "slot %u", i);
	}
}

/*
 * rpc_calc_rto()
 */

/* timer 0 falls back to the configured default (timer.c:114). */
static void rto_timer_zero_returns_default(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	KUNIT_EXPECT_EQ(test, rpc_calc_rto(&rt, 0),
			(unsigned long)TEST_RTO_INIT);
}

/* RTO is mean + 4 * deviation, i.e. ((srtt + 7) >> 3) + sdrtt. */
static void rto_is_mean_plus_deviation(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rt.srtt[0] = 800;
	rt.sdrtt[0] = 25;

	KUNIT_EXPECT_EQ(test, rpc_calc_rto(&rt, 1), 125UL);
}

/* Rounding is upward: (srtt + 7) >> 3, not srtt >> 3. */
static void rto_rounds_mean_upward(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rt.srtt[0] = 1;
	rt.sdrtt[0] = 0;

	KUNIT_EXPECT_EQ(test, rpc_calc_rto(&rt, 1), 1UL);
}

static void rto_clamped_to_maximum(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rt.srtt[0] = 8UL * 60 * HZ * 100;
	rt.sdrtt[0] = 60UL * HZ;

	KUNIT_EXPECT_EQ(test, rpc_calc_rto(&rt, 1),
			(unsigned long)TEST_RTO_MAX);
}

/*
 * rpc_set_timeo() / rpc_ntimeo(), static inlines in
 * include/linux/sunrpc/timer.h
 */

static void set_timeo_timer_zero_is_noop(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_set_timeo(&rt, 0, 5);

	KUNIT_EXPECT_EQ(test, rt.ntimeouts[0], 0);
}

static void set_timeo_records_value(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_set_timeo(&rt, 1, 5);

	KUNIT_EXPECT_EQ(test, rt.ntimeouts[0], 5);
	KUNIT_EXPECT_EQ(test, rpc_ntimeo(&rt, 1), 5);
}

/* Values above 8 are clamped (timer.h). */
static void set_timeo_clamps_at_eight(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_set_timeo(&rt, 1, 100);

	KUNIT_EXPECT_EQ(test, rt.ntimeouts[0], 8);
}

/*
 * An improvement decays the counter by one rather than adopting the new
 * value outright, so a single fast reply does not erase the history of a
 * flaky transport.
 */
static void set_timeo_decays_by_one_on_improvement(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_set_timeo(&rt, 1, 5);
	rpc_set_timeo(&rt, 1, 0);

	KUNIT_EXPECT_EQ(test, rt.ntimeouts[0], 4);
}

static void set_timeo_does_not_decay_below_zero(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rpc_set_timeo(&rt, 1, 0);
	rpc_set_timeo(&rt, 1, 0);

	KUNIT_EXPECT_EQ(test, rt.ntimeouts[0], 0);
}

static void ntimeo_timer_zero_returns_zero(struct kunit *test)
{
	struct rpc_rtt rt;

	rpc_init_rtt(&rt, TEST_RTO_INIT);
	rt.ntimeouts[0] = 7;

	KUNIT_EXPECT_EQ(test, rpc_ntimeo(&rt, 0), 0);
}

/*
 * Suites
 */

static struct kunit_case sunrpc_rtt_init_cases[] = {
	KUNIT_CASE(init_below_threshold_zeroes_srtt),
	KUNIT_CASE(init_above_threshold_scales_srtt),
	{}
};

static struct kunit_suite sunrpc_rtt_init_suite = {
	.name		= "sunrpc-rtt-init",
	.test_cases	= sunrpc_rtt_init_cases,
};

static struct kunit_case sunrpc_rtt_update_cases[] = {
	KUNIT_CASE(update_timer_zero_is_noop),
	KUNIT_CASE(update_discards_negative_sample),
	KUNIT_CASE(update_treats_zero_sample_as_one),
	KUNIT_CASE(update_first_sample_adopted_whole),
	KUNIT_CASE(update_converges_to_eight_times_rtt),
	KUNIT_CASE(update_variance_has_lower_bound),
	KUNIT_CASE(update_slots_are_independent),
	{}
};

static struct kunit_suite sunrpc_rtt_update_suite = {
	.name		= "sunrpc-rtt-update",
	.test_cases	= sunrpc_rtt_update_cases,
};

static struct kunit_case sunrpc_rtt_rto_cases[] = {
	KUNIT_CASE(rto_timer_zero_returns_default),
	KUNIT_CASE(rto_is_mean_plus_deviation),
	KUNIT_CASE(rto_rounds_mean_upward),
	KUNIT_CASE(rto_clamped_to_maximum),
	{}
};

static struct kunit_suite sunrpc_rtt_rto_suite = {
	.name		= "sunrpc-rtt-rto",
	.test_cases	= sunrpc_rtt_rto_cases,
};

static struct kunit_case sunrpc_rtt_ntimeo_cases[] = {
	KUNIT_CASE(set_timeo_timer_zero_is_noop),
	KUNIT_CASE(set_timeo_records_value),
	KUNIT_CASE(set_timeo_clamps_at_eight),
	KUNIT_CASE(set_timeo_decays_by_one_on_improvement),
	KUNIT_CASE(set_timeo_does_not_decay_below_zero),
	KUNIT_CASE(ntimeo_timer_zero_returns_zero),
	{}
};

static struct kunit_suite sunrpc_rtt_ntimeo_suite = {
	.name		= "sunrpc-rtt-ntimeo",
	.test_cases	= sunrpc_rtt_ntimeo_cases,
};

kunit_test_suites(&sunrpc_rtt_init_suite,
		  &sunrpc_rtt_update_suite,
		  &sunrpc_rtt_rto_suite,
		  &sunrpc_rtt_ntimeo_suite);

MODULE_DESCRIPTION("Test SunRPC RTT estimator");
MODULE_LICENSE("GPL");
