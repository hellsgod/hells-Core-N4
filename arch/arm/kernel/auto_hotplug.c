/* Copyright (c) 2012, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 * Copyright (c) 2013 enhanced by motley <motley.slate@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * Generic auto hotplug driver for ARM SoCs. Targeted at current generation
 * SoCs with dual and quad core applications processors.
 * Automatically hotplugs online and offline CPUs based on system load.
 * It is also capable of immediately onlining a core based on an external
 * event by calling void hotplug_boostpulse(void)
 *
 * Not recommended for use with OMAP4460 due to the potential for lockups
 * whilst hotplugging.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/slab.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define CPUS_AVAILABLE		num_possible_cpus()

/*
 * SAMPLING_PERIODS * MIN_SAMPLING_RATE is the minimum
 * load history which will be averaged
 */
#define DEFAULT_SAMPLING_PERIODS	10

/*
 * DEFAULT_MIN_SAMPLING_RATE is the base minimum sampling rate
 * that is based on num_online_cpus()
 */
#define DEFAULT_MIN_SAMPLING_RATE	20

/*
 * Load defines:
 * DEFAULT_ENABLE_ALL_LOAD_THRESHOLD is a default high watermark to rapidly online all CPUs
 *
 * DEFAULT_ENABLE_LOAD_THRESHOLD is the default load which is required to enable 1 extra CPU
 * DEFAULT_DISABLE_LOAD_THRESHOLD is the default load at which a CPU is disabled
 * These two are scaled based on num_online_cpus()
 */
#define DEFAULT_ENABLE_ALL_LOAD_THRESHOLD	(100 * CPUS_AVAILABLE)
#define DEFAULT_ENABLE_LOAD_THRESHOLD		200
#define DEFAULT_DISABLE_LOAD_THRESHOLD		80

/* Control flags */
unsigned char flags;
#define HOTPLUG_DISABLED	(1 << 0)
#define HOTPLUG_PAUSED		(1 << 1)
#define BOOSTPULSE_ACTIVE	(1 << 2)
#define EARLYSUSPEND_ACTIVE	(1 << 3)

/*
 * Enable debug output to dump the average
 * calculations and ring buffer array values
 * WARNING: Enabling this causes a ton of overhead
 */
static unsigned int debug = 0;

static unsigned int enable_all_load_threshold;
static unsigned int enable_load_threshold = DEFAULT_ENABLE_LOAD_THRESHOLD;
static unsigned int disable_load_threshold = DEFAULT_DISABLE_LOAD_THRESHOLD;
static unsigned int min_sampling_rate = DEFAULT_MIN_SAMPLING_RATE;
static unsigned int sampling_periods = DEFAULT_SAMPLING_PERIODS;
static unsigned int live_sampling_periods = DEFAULT_SAMPLING_PERIODS;
static unsigned int index_max_value  = (DEFAULT_SAMPLING_PERIODS - 1);
static unsigned int min_online_cpus = 1;
static unsigned int max_online_cpus;

struct delayed_work hotplug_decision_work;
struct delayed_work hotplug_unpause_work;
struct work_struct hotplug_online_all_work;
struct work_struct hotplug_online_single_work;
struct delayed_work hotplug_offline_work;
struct work_struct hotplug_offline_all_work;
struct work_struct hotplug_boost_online_work;

static unsigned int *history;
static unsigned int index;

static int set_enable_all_load_threshold(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	long num;

	if (!val)
		return -EINVAL;

	ret = strict_strtol(val, 0, &num);
	if (ret == -EINVAL || num > 550 || num < 270)
		return -EINVAL;

	ret = param_set_int(val, kp);
	pr_info("auto_hotplug: enable_all_load_threshold = %d\n", enable_all_load_threshold);

	return ret;
}

static int set_enable_load_threshold(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	long num;

	if (!val)
		return -EINVAL;

	ret = strict_strtol(val, 0, &num);
	if (ret == -EINVAL || num > 250 || num < 130)
		return -EINVAL;

	ret = param_set_int(val, kp);

	pr_info("auto_hotplug: enable_load_threshold = %d\n", enable_load_threshold);

	return ret;
}

static int set_disable_load_threshold(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	long num;

	if (!val)
		return -EINVAL;

	ret = strict_strtol(val, 0, &num);
	if (ret == -EINVAL || num > 125 || num < 40)
		return -EINVAL;

	ret = param_set_int(val, kp);

	pr_info("auto_hotplug: disable_load_threshold = %d\n", disable_load_threshold);

	return ret;
}

static int set_min_sampling_rate(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	long num;

	if (!val)
		return -EINVAL;

	ret = strict_strtol(val, 0, &num);
	if (ret == -EINVAL || num > 50 || num < 10)
		return -EINVAL;

	ret = param_set_int(val, kp);

	pr_info("auto_hotplug: min_sampling_rate = %d\n", min_sampling_rate);

	return ret;
}

static int set_debug(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	pr_info("auto_hotplug: debug = %d\n", debug);

	return ret;
}

static int set_sampling_periods(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	long num;

	if (!val)
		return -EINVAL;

	ret = strict_strtol(val, 0, &num);
	if (ret == -EINVAL || num > 50 || num < 5)
		return -EINVAL;

	ret = param_set_int(val, kp);

	pr_info("auto_hotplug: sampling_periods = %d\n", sampling_periods);

	return ret;
}

static int min_online_cpus_set(const char *arg, const struct kernel_param *kp)
{
    int ret; 
    
    ret = param_set_int(arg, kp);
    
    ///at least 1 core must run even if set value is out of range
    if ((min_online_cpus < 1) || (min_online_cpus > CPUS_AVAILABLE))
        min_online_cpus = 1;
    
    return ret;
}

static int max_online_cpus_set(const char *arg, const struct kernel_param *kp)
{
    int ret;

    ret = param_set_int(arg, kp);

    ///default to cpus available if set value is out of range
    if ((max_online_cpus < 1) || (max_online_cpus > CPUS_AVAILABLE))
        max_online_cpus = CPUS_AVAILABLE;

    return ret;
}

static struct kernel_param_ops min_online_cpus_ops = {
    .set = min_online_cpus_set,
    .get = param_get_uint,
};

static struct kernel_param_ops max_online_cpus_ops = {
    .set = max_online_cpus_set,
    .get = param_get_uint,
};

static struct kernel_param_ops module_ops_enable_all_load_threshold = {
	.set = set_enable_all_load_threshold,
	.get = param_get_uint,
};

static struct kernel_param_ops module_ops_enable_load_threshold = {
	.set = set_enable_load_threshold,
	.get = param_get_uint,
};

static struct kernel_param_ops module_ops_disable_load_threshold = {
	.set = set_disable_load_threshold,
	.get = param_get_uint,
};

static struct kernel_param_ops module_ops_min_sampling_rate = {
	.set = set_min_sampling_rate,
	.get = param_get_uint,
};

static struct kernel_param_ops module_ops_debug = {
	.set = set_debug,
	.get = param_get_bool,
};

static struct kernel_param_ops module_ops_sampling_periods = {
	.set = set_sampling_periods,
	.get = param_get_uint,
};

module_param_cb(enable_all_load_threshold, &module_ops_enable_all_load_threshold, &enable_all_load_threshold, 0775);
MODULE_PARM_DESC(enable_all_load_threshold, "auto_hotplug load threshold to rapidly online all CPUs (270-550)");

module_param_cb(enable_load_threshold, &module_ops_enable_load_threshold, &enable_load_threshold, 0775);
MODULE_PARM_DESC(enable_load_threshold, "auto_hotplug load threshold to enable one CPU (130-250)");

module_param_cb(disable_load_threshold, &module_ops_disable_load_threshold, &disable_load_threshold, 0775);
MODULE_PARM_DESC(disable_load_threshold, "auto_hotplug load threshold to disable one CPU (40-125)");

module_param_cb(min_sampling_rate, &module_ops_min_sampling_rate, &min_sampling_rate, 0775);
MODULE_PARM_DESC(min_sampling_rate, "auto_hotplug minimum sampling rate (10-50ms)");

module_param_cb(debug, &module_ops_debug, &debug, 0775);
MODULE_PARM_DESC(enabled, "auto_hotplug debug to kernel log (Y/N)");

module_param_cb(sampling_periods, &module_ops_sampling_periods, &sampling_periods, 0775);
MODULE_PARM_DESC(sampling_periods, "auto_hotplug history sampling periods (5-50)");

module_param_cb(min_online_cpus, &min_online_cpus_ops, &min_online_cpus, 0775);
MODULE_PARM_DESC(min_online_cpus, "auto_hotplug min_online_cpus (1-#CPUs)");

module_param_cb(max_online_cpus, &max_online_cpus_ops, &max_online_cpus, 0775);
MODULE_PARM_DESC(max_online_cpus, "auto_hotplug max_online_cpus (1-#CPUs)");

static void hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int running, disable_load, enable_load, avg_running = 0;
	unsigned int online_cpus, available_cpus, i, j;
	unsigned int k;
	unsigned long sampling_rate = 0;
	unsigned long min_sampling_rate_in_jiffies = 0;
	void __iomem **new_sample_size;

	min_sampling_rate_in_jiffies = msecs_to_jiffies(min_sampling_rate);
	online_cpus = num_online_cpus();
	available_cpus = CPUS_AVAILABLE;
	disable_load = disable_load_threshold * online_cpus;
	enable_load = enable_load_threshold * online_cpus;

	if (live_sampling_periods != sampling_periods) {
		new_sample_size = krealloc(history,live_sampling_periods*sizeof(int),GFP_KERNEL);
		if (!new_sample_size) {
			pr_info("live sampling periods reallocation failed.");
		} else {
			live_sampling_periods = sampling_periods;
			index_max_value = live_sampling_periods - 1;
			if (debug)
				pr_info("live sampling periods changed: %d\n",live_sampling_periods);
		}
	}

	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use fp division to get the average.
	 */
	running = nr_running() * 100;

	history[index] = running;

	if (debug) {
		pr_info("online_cpus: %d\n", online_cpus);
		pr_info("enable_load: %d, disable_load:%d\n", enable_load, disable_load);
		pr_info("curr index: %d, curr load:%d\n", index, running);
	}

	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short duration where
	 * we don't want additional cores to be onlined because
	 * the cpufreq driver should take care of those load spikes.
	 */
	for (i = 0, j = index; i < live_sampling_periods; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = index_max_value;
	}

	/*
	 * If we are at the end of the buffer, return to the beginning.
	 */
	if (unlikely(index++ == index_max_value))
		index = 0;

	if (debug) {
		pr_info("load samples: %d\n",live_sampling_periods);
		for (k = 0; k < live_sampling_periods; k++) {
			 pr_info("%d: %d\t",k, history[k]);
		}
	}

	avg_running = avg_running / live_sampling_periods;

	if (debug)
		pr_info("average load: %d\n", avg_running);

	if (likely(!(flags & HOTPLUG_DISABLED))) {
		if (unlikely((avg_running >= enable_all_load_threshold) && (online_cpus < available_cpus) && (max_online_cpus > online_cpus))) {
			if (debug)
				pr_info("auto_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);

			/*
			 * Flush any delayed offlining work from the workqueue.
			 * No point in having expensive unnecessary hotplug transitions.
			 * We still online after flushing, because load is high enough to
			 * warrant it.
			 * We set the paused flag so the sampling can continue but no more
			 * hotplug events will occur.
			 */
			flags |= HOTPLUG_PAUSED;
			if (delayed_work_pending(&hotplug_offline_work))
				cancel_delayed_work(&hotplug_offline_work);
			schedule_work(&hotplug_online_all_work);
			return;
		} else if (flags & HOTPLUG_PAUSED) {
			schedule_delayed_work_on(0, &hotplug_decision_work, min_sampling_rate_in_jiffies);
			return;
		} else if ((avg_running >= enable_load) && (online_cpus < available_cpus) && (max_online_cpus > online_cpus)) {
			if (debug)
				pr_info("auto_hotplug: Onlining single CPU, avg running: %d\n", avg_running);
			if (delayed_work_pending(&hotplug_offline_work))
				cancel_delayed_work(&hotplug_offline_work);
			schedule_work(&hotplug_online_single_work);
			return;
		} else if ((avg_running <= disable_load) && (min_online_cpus < online_cpus)) {
			/* Only queue a cpu_down() if there isn't one already pending */
			if (!(delayed_work_pending(&hotplug_offline_work))) {
				if (debug)
					pr_info("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
				schedule_delayed_work_on(0, &hotplug_offline_work, HZ);
			}
			/* If boostpulse is active, clear the flags */
			if (flags & BOOSTPULSE_ACTIVE) {
				flags &= ~BOOSTPULSE_ACTIVE;
				if (debug)
					pr_info("auto_hotplug: Clearing boostpulse flags\n");
			}
		}
	}

	/*
	 * Reduce the sampling rate dynamically based on online cpus.
	 */
	sampling_rate = min_sampling_rate_in_jiffies * (online_cpus * online_cpus);
	if (debug)
		pr_info("sampling_rate is: %d\n", jiffies_to_msecs(sampling_rate));

	schedule_delayed_work_on(0, &hotplug_decision_work, sampling_rate);

}

static void __cpuinit hotplug_online_all_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu))) {
			cpu_up(cpu);
			if (debug)
				pr_info("auto_hotplug: CPU%d up.\n", cpu);
		}
	}
	/*
	 * Pause for 2 seconds before even considering offlining a CPU
	 */
	schedule_delayed_work(&hotplug_unpause_work, HZ * 2);
	schedule_delayed_work_on(0, &hotplug_decision_work, min_sampling_rate);
}

static void hotplug_offline_all_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		if (likely(cpu_online(cpu) && (cpu))) {
			cpu_down(cpu);
			if (debug)
				pr_info("auto_hotplug: CPU%d down.\n", cpu);
		}
	}
}

static void __cpuinit hotplug_online_single_work_fn(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu) {
			if (!cpu_online(cpu)) {
				cpu_up(cpu);
				if (debug)
					pr_info("auto_hotplug: CPU%d up.\n", cpu);
				break;
			}
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, min_sampling_rate);
}

static void hotplug_offline_work_fn(struct work_struct *work)
{
	int cpu;
	for_each_online_cpu(cpu) {
		if (cpu) {
			cpu_down(cpu);
			if (debug)
				pr_info("auto_hotplug: CPU%d down.\n", cpu);
			break;
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, min_sampling_rate);
}

static void hotplug_unpause_work_fn(struct work_struct *work)
{
	if (debug)
		pr_info("auto_hotplug: Clearing pause flag\n");
	flags &= ~HOTPLUG_PAUSED;
}

void hotplug_disable(bool flag)
{
	if ((flags & HOTPLUG_DISABLED) && !flag) {
		flags &= ~HOTPLUG_DISABLED;
		flags &= ~HOTPLUG_PAUSED;
		if (debug)
			pr_info("auto_hotplug: Clearing disable flag\n");
		schedule_delayed_work_on(0, &hotplug_decision_work, 0);
	} else if (flag && (!(flags & HOTPLUG_DISABLED))) {
		flags |= HOTPLUG_DISABLED;
		if (debug)
			pr_info("auto_hotplug: Setting disable flag\n");
		cancel_delayed_work_sync(&hotplug_offline_work);
		cancel_delayed_work_sync(&hotplug_decision_work);
		cancel_delayed_work_sync(&hotplug_unpause_work);
	}
}

void hotplug_boostpulse(void)
{
	unsigned int online_cpus;
	online_cpus = num_online_cpus();
	if (unlikely(flags & (EARLYSUSPEND_ACTIVE
		| HOTPLUG_DISABLED)))
		return;

	if (!(flags & BOOSTPULSE_ACTIVE) && (max_online_cpus > online_cpus)) {
		flags |= BOOSTPULSE_ACTIVE;
		/*
		 * If there are less than 2 CPUs online, then online
		 * an additional CPU, otherwise check for any pending
		 * offlines, cancel them and pause for 2 seconds.
		 * Either way, we don't allow any cpu_down()
		 * whilst the user is interacting with the device.
		 */
		if (likely(online_cpus < 2)) {
			cancel_delayed_work_sync(&hotplug_offline_work);
			flags |= HOTPLUG_PAUSED;
			schedule_work(&hotplug_online_single_work);
			schedule_delayed_work(&hotplug_unpause_work, HZ);
		} else {
			if (debug)
				pr_info("auto_hotplug: %s: %d CPUs online\n", __func__, num_online_cpus());
			if (delayed_work_pending(&hotplug_offline_work)) {
				if (debug)
					pr_info("auto_hotplug: %s: Canceling hotplug_offline_work\n", __func__);
				cancel_delayed_work(&hotplug_offline_work);
				flags |= HOTPLUG_PAUSED;
				schedule_delayed_work(&hotplug_unpause_work, HZ * 2);
				schedule_delayed_work_on(0, &hotplug_decision_work, min_sampling_rate);
			}
		}
	}
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void auto_hotplug_early_suspend(struct early_suspend *handler)
{
	if (debug)
		pr_info("auto_hotplug: early suspend handler\n");

	flags |= EARLYSUSPEND_ACTIVE;

	/* Cancel all scheduled delayed work to avoid races */
	cancel_delayed_work_sync(&hotplug_offline_work);
	cancel_delayed_work_sync(&hotplug_decision_work);
	if (num_online_cpus() > 1) {
		pr_info("auto_hotplug: Offlining CPUs for early suspend\n");
		schedule_work_on(0, &hotplug_offline_all_work);
	}
}

static void auto_hotplug_late_resume(struct early_suspend *handler)
{
	int i = 0;

	if (debug)
		pr_info("auto_hotplug: late resume handler\n");

	flags &= ~EARLYSUSPEND_ACTIVE;

	//stack the deck, let's get moving again
	for (i=0; i<5; i++) {
		history[i] = 500;
	}

	schedule_delayed_work_on(0, &hotplug_decision_work, HZ/2);
}

static struct early_suspend auto_hotplug_suspend = {
	.suspend = auto_hotplug_early_suspend,
	.resume = auto_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

int __init auto_hotplug_init(void)
{
	pr_info("auto_hotplug: v0.220 by _thalamus\n");
	pr_info("auto_hotplug: rev 4 enhanced by motley\n");
	pr_info("auto_hotplug: %d CPUs detected\n", CPUS_AVAILABLE);

	/* Init circular buffer for history to the default default size */
	history = kmalloc(live_sampling_periods * sizeof(int),GFP_KERNEL);

	/* Placing these here to avoid a compiler warning */
	enable_all_load_threshold = DEFAULT_ENABLE_ALL_LOAD_THRESHOLD;
	max_online_cpus = num_possible_cpus();

	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_unpause_work, hotplug_unpause_work_fn);
	INIT_WORK(&hotplug_online_all_work, hotplug_online_all_work_fn);
	INIT_WORK(&hotplug_online_single_work, hotplug_online_single_work_fn);
	INIT_WORK(&hotplug_offline_all_work, hotplug_offline_all_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_offline_work, hotplug_offline_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */
	flags |= HOTPLUG_PAUSED;
	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 5);
	schedule_delayed_work(&hotplug_unpause_work, HZ * 10);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&auto_hotplug_suspend);
#endif
	return 0;
}
late_initcall(auto_hotplug_init);
