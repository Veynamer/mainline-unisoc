// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 */

#include "linux/sched_clock.h"
#include "vdso/time64.h"
#define CLOCK_TICK_RATE 32768

#include "linux/clockchips.h"
#include "linux/clocksource.h"
#include "linux/jiffies.h"
#include "linux/of_address.h"
#include "linux/of_irq.h"
#include <linux/init.h>
#include <linux/interrupt.h>

#include "timer-of.h"

#define TIMER_NAME		"sprd_timer"

#define TIMER_LOAD_LO		0x0
#define TIMER_LOAD_HI		0x4
#define TIMER_VALUE_LO		0x8
#define TIMER_VALUE_HI		0xc

#define TIMER_CTL		0x10
#define TIMER_CTL_PERIOD_MODE	BIT(0)
#define TIMER_CTL_ENABLE	BIT(1)
#define TIMER_CTL_64BIT_WIDTH	BIT(16)

#define TIMER_INT		0x14
#define TIMER_INT_EN		BIT(0)
#define TIMER_INT_RAW_STS	BIT(1)
#define TIMER_INT_MASK_STS	BIT(2)
#define TIMER_INT_CLR		BIT(3)

#define TIMER_VALUE_SHDW_LO	0x18
#define TIMER_VALUE_SHDW_HI	0x1c

#define TIMER_VALUE_LO_MASK	GENMASK(31, 0)

static void sprd_timer_enable(void __iomem *base, u32 flag)
{
	u32 val = readl_relaxed(base + TIMER_CTL);

	val |= TIMER_CTL_ENABLE;
	if (flag & TIMER_CTL_64BIT_WIDTH)
		val |= TIMER_CTL_64BIT_WIDTH;
	else
		val &= ~TIMER_CTL_64BIT_WIDTH;

	if (flag & TIMER_CTL_PERIOD_MODE)
		val |= TIMER_CTL_PERIOD_MODE;
	else
		val &= ~TIMER_CTL_PERIOD_MODE;

	writel_relaxed(val, base + TIMER_CTL);
}

static void sprd_timer_disable(void __iomem *base)
{
	u32 val = readl_relaxed(base + TIMER_CTL);

	val &= ~TIMER_CTL_ENABLE;
	writel_relaxed(val, base + TIMER_CTL);
}

static void sprd_timer_update_counter(void __iomem *base, unsigned long cycles)
{
	writel_relaxed(cycles & TIMER_VALUE_LO_MASK, base + TIMER_LOAD_LO);
	writel_relaxed(0, base + TIMER_LOAD_HI);
}

static void sprd_timer_enable_interrupt(void __iomem *base)
{
	writel_relaxed(TIMER_INT_EN, base + TIMER_INT);
}

static void sprd_timer_clear_interrupt(void __iomem *base)
{
	u32 val = readl_relaxed(base + TIMER_INT);

	val |= TIMER_INT_CLR;
	writel_relaxed(val, base + TIMER_INT);
}

static int sprd_timer_set_next_event(unsigned long cycles,
				     struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	sprd_timer_update_counter(timer_of_base(to), cycles);
	sprd_timer_enable(timer_of_base(to), 0);

	return 0;
}

static int sprd_timer_set_periodic(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	sprd_timer_update_counter(timer_of_base(to), timer_of_period(to));
	sprd_timer_enable(timer_of_base(to), TIMER_CTL_PERIOD_MODE);

	return 0;
}

static int sprd_timer_shutdown(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	return 0;
}

static irqreturn_t sprd_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_clear_interrupt(timer_of_base(to));

	if (clockevent_state_oneshot(ce))
		sprd_timer_disable(timer_of_base(to));

	ce->event_handler(ce);
	return IRQ_HANDLED;
}

static struct timer_of to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE | TIMER_OF_CLOCK,

	.clkevt = {
		.name = TIMER_NAME,
		.rating = 300,
		.features = CLOCK_EVT_FEAT_DYNIRQ | CLOCK_EVT_FEAT_PERIODIC |
			CLOCK_EVT_FEAT_ONESHOT,
		.set_state_shutdown = sprd_timer_shutdown,
		.set_state_periodic = sprd_timer_set_periodic,
		.set_next_event = sprd_timer_set_next_event,
		.cpumask = cpu_possible_mask,
	},

	.of_irq = {
		.handler = sprd_timer_interrupt,
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
	},
};

static int __init sprd_timer_init(struct device_node *np)
{
	int ret;

	ret = timer_of_init(np, &to);
	if (ret)
		return ret;

	sprd_timer_enable_interrupt(timer_of_base(&to));
	clockevents_config_and_register(&to.clkevt, timer_of_rate(&to),
					1, UINT_MAX);

	return 0;
}

static struct timer_of suspend_to = {
	.flags = TIMER_OF_BASE | TIMER_OF_CLOCK,
};

static u64 sprd_suspend_timer_read(struct clocksource *cs)
{
	return ~(u64)readl_relaxed(timer_of_base(&suspend_to) +
				   TIMER_VALUE_SHDW_LO) & cs->mask;
}

static int sprd_suspend_timer_enable(struct clocksource *cs)
{
	sprd_timer_update_counter(timer_of_base(&suspend_to),
				  TIMER_VALUE_LO_MASK);
	sprd_timer_enable(timer_of_base(&suspend_to), TIMER_CTL_PERIOD_MODE);

	return 0;
}

static void sprd_suspend_timer_disable(struct clocksource *cs)
{
	sprd_timer_disable(timer_of_base(&suspend_to));
}

static struct clocksource suspend_clocksource = {
	.name	= "sprd_suspend_timer",
	.rating	= 200,
	.read	= sprd_suspend_timer_read,
	.enable = sprd_suspend_timer_enable,
	.disable = sprd_suspend_timer_disable,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_SUSPEND_NONSTOP,
};

static int __init sprd_suspend_timer_init(struct device_node *np)
{
	int ret;

	ret = timer_of_init(np, &suspend_to);
	if (ret)
		return ret;

	clocksource_register_hz(&suspend_clocksource,
				timer_of_rate(&suspend_to));

	return 0;
}

#define SCX35_TIMER_DISABLE     0
#define SCX35_TIMER_ENABLE      BIT(7)
#define SCX35_TIMER_NEW         BIT(8)

#define SCX35_TIMER_INT_EN      BIT(1)
#define SCX35_TIMER_INT_CLR     BIT(3)
#define SCX35_TIMER_INT_BUSY    BIT(4)

#define SCX35_ONESHOT_MODE      0
#define SCX35_PERIOD_MODE       BIT(6)

#define SCX35_EVENT_TIMER       0
#define SCX35_SOURCE_TIMER      1

#define SCX35_TIMER_OFFSET(id)  (0x20 * id)
#define SCX35_TIMER_LOAD        (0x00)
#define SCX35_TIMER_VALUE       (0x04)
#define SCX35_TIMER_CTL         (0x08)
#define SCX35_TIMER_INT         (0x0c)
#define SCX35_TIMER_CNT_RD      (0x10)

static int scx35_timer_shutdown(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);
	u32 saved;

	writel_relaxed(SCX35_TIMER_INT_CLR, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_INT);

	saved = readl_relaxed(timer_of_base(to) +
	                      SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) +
	                      SCX35_TIMER_CTL) & SCX35_PERIOD_MODE;
	writel_relaxed(SCX35_TIMER_DISABLE | saved,
	               timer_of_base(to) + SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) +
	               SCX35_TIMER_CTL);
	return 0;
}

static int scx35_timer_set_periodic(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	writel_relaxed(SCX35_TIMER_DISABLE | SCX35_PERIOD_MODE, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_CTL);
	writel_relaxed(LATCH, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_LOAD);
	writel_relaxed(SCX35_TIMER_ENABLE | SCX35_PERIOD_MODE, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_CTL);
	writel_relaxed(SCX35_TIMER_INT_EN, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_INT);

	return 0;
}

static int scx35_timer_set_next_event(unsigned long cycles,
				     struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);
	while (SCX35_TIMER_INT_BUSY & readl_relaxed(timer_of_base(to) +
	                              SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) +
	                              SCX35_TIMER_INT));
	writel_relaxed(SCX35_TIMER_DISABLE | SCX35_ONESHOT_MODE, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_CTL);
	writel_relaxed(cycles, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_LOAD);
	writel_relaxed(SCX35_TIMER_ENABLE | SCX35_ONESHOT_MODE, timer_of_base(to) +
	               SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_CTL);

	return 0;
}

static irqreturn_t scx35_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(ce);
	u32 value;

	value = readl_relaxed(timer_of_base(to) +
	                      SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_INT);
	value |= TIMER_INT_CLR;
	writel_relaxed(value, timer_of_base(to) +
	                      SCX35_TIMER_OFFSET(SCX35_EVENT_TIMER) + SCX35_TIMER_INT);

	if (ce && ce->event_handler)
		ce->event_handler(ce);
	return IRQ_HANDLED;
}

/* CPU0 GP clockevent */
static struct timer_of scx35_to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE,

	.clkevt = {
		.name = TIMER_NAME,
		.rating = 200,
		.shift = 32,
		.features = CLOCK_EVT_FEAT_ONESHOT,
		.set_state_shutdown = scx35_timer_shutdown,
		.set_state_periodic = scx35_timer_set_periodic,
		.set_next_event = scx35_timer_set_next_event,
	},

	.of_irq = {
		.handler = scx35_timer_interrupt,
		.flags = IRQF_TIMER | IRQF_NOBALANCING,
	},
};

static u64 scx35_sched_clock_read(void) {
	return ~readl_relaxed(timer_of_base(&scx35_to) +
	                      SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER) + SCX35_TIMER_CNT_RD);
}

static u64 scx35_timer_read(struct clocksource *cs)
{
	return scx35_sched_clock_read();
}

static int scx35_timer_enable(struct clocksource *cs)
{
	writel_relaxed(SCX35_TIMER_ENABLE | SCX35_TIMER_NEW | SCX35_PERIOD_MODE,
	               timer_of_base(&scx35_to) +
	               SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER) + SCX35_TIMER_CTL);

	return 0;
}

static void scx35_timer_disable(struct clocksource *cs)
{
	writel_relaxed(SCX35_TIMER_DISABLE | SCX35_PERIOD_MODE,
	               timer_of_base(&scx35_to) +
	               SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER) + SCX35_TIMER_CTL);
}

/* CPU0 GP local timer */
static struct clocksource scx35_clocksource = {
	.name	= "sprd_scx35_clocksource",
	.rating	= 300,
	.read	= scx35_timer_read,
	.enable = scx35_timer_enable,
	.disable = scx35_timer_disable,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int __init scx35_init(struct device_node *np) {
	void __iomem *base;
	struct clock_event_device* evt;
	int ret;

	ret = timer_of_init(np, &scx35_to);
	if (ret)
		return ret;

	base = timer_of_base(&scx35_to);
	// disable irq
	writel_relaxed(0, base + SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER)
	                  + SCX35_TIMER_INT);
	writel_relaxed(SCX35_TIMER_DISABLE | SCX35_TIMER_NEW | SCX35_PERIOD_MODE,
	               base + SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER)
	               + SCX35_TIMER_CTL);
	writel_relaxed(ULONG_MAX, base + SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER)
	               + SCX35_TIMER_LOAD);
	writel_relaxed(SCX35_TIMER_NEW | SCX35_PERIOD_MODE,
	               base + SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER)
	               + SCX35_TIMER_CTL);
	writel_relaxed(SCX35_TIMER_ENABLE | SCX35_TIMER_NEW | SCX35_PERIOD_MODE,
	               base + SCX35_TIMER_OFFSET(SCX35_SOURCE_TIMER)
	               + SCX35_TIMER_CTL);

	evt = &scx35_to.clkevt;
	evt->mult = div_sc(32768, NSEC_PER_SEC, evt->shift);
	evt->max_delta_ns = clockevent_delta2ns(0xf0000000, evt);
	evt->min_delta_ns = clockevent_delta2ns(4, evt);
	clockevents_register_device(evt);

	sched_clock_register(scx35_sched_clock_read, 32, 32768);

	// TODO: timer_of_rate(&to)
	return clocksource_register_hz(&scx35_clocksource, 32768);
}

TIMER_OF_DECLARE(sc9860_timer, "sprd,sc9860-timer", sprd_timer_init);
TIMER_OF_DECLARE(sc9860_persistent_timer, "sprd,sc9860-suspend-timer",
		 sprd_suspend_timer_init);
TIMER_OF_DECLARE(scx35_timer, "sprd,scx35-timer", scx35_init);
