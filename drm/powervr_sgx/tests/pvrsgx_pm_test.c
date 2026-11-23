// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Viktor Semykin */

#include <kunit/test.h>

struct pm_test_wait {
	struct pvrsgx_inval_wait wait;
	bool done;
};

static void pm_test_wait_done(struct pvrsgx_inval_wait *wait)
{
	container_of(wait, struct pm_test_wait, wait)->done = true;
}

/* A stopped GPU cannot consume another command. Even waiters past the ring's
 * current sequence must retire when runtime suspend establishes quiescence.
 */
static void pm_test_stop_retires_waiters(struct kunit *test)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	struct pm_test_wait first = { .wait = { .seq = 1, .done = pm_test_wait_done } };
	struct pm_test_wait last = { .wait = { .seq = U64_MAX, .done = pm_test_wait_done } };

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	mutex_init(&sdev->reset_lock);
	spin_lock_init(&sdev->inval_lock);
	INIT_LIST_HEAD(&sdev->inval_waits);
	dev_set_drvdata(dev, sdev);
	list_add_tail(&first.wait.node, &sdev->inval_waits);
	list_add_tail(&last.wait.node, &sdev->inval_waits);
	KUNIT_EXPECT_EQ(test, pvrsgx_runtime_suspend(dev), 0);
	KUNIT_EXPECT_TRUE(test, first.done);
	KUNIT_EXPECT_TRUE(test, last.done);
	KUNIT_EXPECT_TRUE(test, list_empty(&sdev->inval_waits));
	mutex_destroy(&sdev->reset_lock);
}

static struct kunit_case pvrsgx_pm_test_cases[] = {
	KUNIT_CASE(pm_test_stop_retires_waiters),
	{}
};

static struct kunit_suite pvrsgx_pm_test_suite = {
	.name = "pvrsgx_pm",
	.test_cases = pvrsgx_pm_test_cases,
};

kunit_test_suite(pvrsgx_pm_test_suite);
