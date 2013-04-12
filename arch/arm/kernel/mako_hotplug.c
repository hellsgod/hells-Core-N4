/*
 *  Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *  All rights reserved.
 *
 *  Simple no bullshit hot[in]plug driver for SMP
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/cpufreq.h>
#include <mach/cpufreq.h>

#define MAKO_HOTPLUG_VERSION 1

/* threshold for comparing time diffs */
#define SEC_THRESHOLD         2000
#define HISTORY_SIZE          10
#define DEFAULT_FIRST_LEVEL   90
#define DEFAULT_SECOND_LEVEL  25
#define DEFAULT_THIRD_LEVEL   50
#define DEFAULT_SUSPEND_FREQ  702000

static unsigned int default_first_level;
static unsigned int default_second_level;
static unsigned int default_third_level;
static unsigned int suspend_freq;
static unsigned int load_history[HISTORY_SIZE];
static unsigned int counter;

/* from msm_rq_stats */
unsigned int report_load_at_max_freq(void);

/*
 * TODO probably populate the struct with more relevant data
 */
struct cpu_stats {
	/* variable to be accessed to filter spurious load spikes */
	unsigned long time_stamp;
	unsigned int online_cpus;
	unsigned int total_cpus;
};

static struct cpu_stats stats;
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static void first_level_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	if ((now - stats.time_stamp) >= temp_diff) {
		for_each_possible_cpu(cpu) {
			if (cpu) {
				if (!cpu_online(cpu)) {
					cpu_up(cpu);
					pr_debug
					    ("mako_hotplug: cpu%d is up - high load\n",
					     cpu);
				}
			}
		}

		/*
		 * new current time for comparison in the next load check
		 * we don't want too many hot[in]plugs in small time span
		 */
		stats.time_stamp = now;
	}
}

static void second_level_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	if (stats.online_cpus < 2 || (now - stats.time_stamp) >= temp_diff) {
		for_each_possible_cpu(cpu) {
			if (cpu) {
				if (!cpu_online(cpu)) {
					cpu_up(cpu);
					pr_debug
					    ("mako_hotplug: cpu%d is up - medium load\n",
					     cpu);
					break;
				}
			}
		}

		stats.time_stamp = now;
	}
}

static void third_level_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	if ((now - stats.time_stamp) >= temp_diff) {
		for_each_possible_cpu(cpu) {
			if (cpu) {
				if (cpu_online(cpu)) {
					cpu_down(cpu);
					pr_debug
					    ("mako_hotplug: cpu%d is down - low load\n",
					     cpu);
				}
			}
		}

		stats.time_stamp = now;
	}
}

static void __cpuinit decide_hotplug_func(struct work_struct *work)
{
	unsigned long now;
	unsigned int k, first_level, second_level, third_level;
	static unsigned int load = 0;

	/* 
	 * start feeding the current load to the history array so that we can
	 * make a little average. Works good for filtering low and/or high load
	 * spikes
	 */
	if (counter++ == HISTORY_SIZE)
		counter = 0;

	load_history[counter] = report_load_at_max_freq();

	for (k = 0; k < HISTORY_SIZE; k++)
		load += load_history[k];

	load = load / HISTORY_SIZE;
	/* finish load routines */

	/* time of this sampling time */
	now = ktime_to_ms(ktime_get());

	stats.online_cpus = num_online_cpus();

	/* the load thresholds scale with the number of online cpus */
	first_level = default_first_level * stats.online_cpus;
	second_level = default_second_level * stats.online_cpus;
	third_level = default_third_level * stats.online_cpus;

	if (load >= first_level) {
		first_level_work_check(SEC_THRESHOLD, now);
		queue_delayed_work_on(0, wq, &decide_hotplug,
				      msecs_to_jiffies(HZ));
		return;
	}

	/* load is medium-high so online only one core at a time */
	else if (load >= second_level) {
		/*
		 * Feed it 2 times the seconds threshold because when this
		 * is called there is a check inside that onlines cpu1
		 * bypassing the time_diff but afterwards it takes at least 4
		 * seconds as threshold before onlining another cpu. This
		 * eliminates unneeded onlining when we are for example
		 * swipping between home or app drawer and we only need
		 * cpu0 and cpu1 online for that, cpufreq takes care of the
		 * rest
		 */
		second_level_work_check(SEC_THRESHOLD * 2, now);
		queue_delayed_work_on(0, wq, &decide_hotplug,
				      msecs_to_jiffies(HZ));
		return;
	}

	/* low load obliterate the cpus to death */
	else if (load <= third_level && stats.online_cpus > 1) {
		third_level_work_check(SEC_THRESHOLD, now);
	}

	queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(HZ));
}

static void mako_hotplug_early_suspend(struct early_suspend *handler)
{
	unsigned int cpu = nr_cpu_ids;

	/* cancel the hotplug work when the screen is off and flush the WQ */
	flush_workqueue(wq);
	cancel_delayed_work_sync(&decide_hotplug);
	pr_info("mako_hotplug: Early suspend - stopping Hotplug work...");

	if (num_online_cpus() > 1) {
		for_each_possible_cpu(cpu) {
			if (cpu) {
				if (cpu_online(cpu)) {
					cpu_down(cpu);
					pr_debug
					    ("mako_hotplug: Early suspend - cpu%d is down\n",
					     cpu);
				}
			}
		}
	}

	/* cap max frequency to 702MHz by default */
	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT, suspend_freq);
	pr_info
	    ("mako_hotplug: Early suspend - cpu%d max freq: %dMHz\n",
	     0, suspend_freq / 1000);

	stats.online_cpus = num_online_cpus();
}

static void __ref mako_hotplug_late_resume(struct early_suspend *handler)
{
	unsigned int cpu = nr_cpu_ids;

	/* online all cores when the screen goes online */
	for_each_possible_cpu(cpu) {
		if (cpu) {
			if (!cpu_online(cpu)) {
				cpu_up(cpu);
				pr_debug
				    ("mako_hotplug: Late resume - cpu%d is up\n",
				     cpu);
			}
		}
	}

	/* restore default max frequency */
	msm_cpufreq_set_freq_limits(0, MSM_CPUFREQ_NO_LIMIT,
				    MSM_CPUFREQ_NO_LIMIT);
	pr_info("mako_hotplug: Late resume - restore cpu%d max frequency.\n",
		0);

	/* new time_stamp and online_cpu because all cpus were just onlined */
	stats.time_stamp = ktime_to_ms(ktime_get());
	stats.online_cpus = num_online_cpus();

	pr_info("mako_hotplug: Late resume - starting Hotplug work...\n");
	queue_delayed_work_on(0, wq, &decide_hotplug, HZ);
}

static struct early_suspend mako_hotplug_suspend = {
	.suspend = mako_hotplug_early_suspend,
	.resume = mako_hotplug_late_resume,
};

static ssize_t load_levels_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u %u %u\n", default_first_level,
		       default_second_level, default_third_level);
}

static ssize_t load_levels_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	int i, ret = 0;
	unsigned int val[3];

	ret = sscanf(buf, "%u %u %u\n", &val[0], &val[1], &val[2]);
	if (ret != ARRAY_SIZE(val))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(val); i++) {
		if (val[i] <= 0 || val[i] >= 100)
			return -EINVAL;
	}

	default_first_level = val[0];
	default_second_level = val[1];
	default_third_level = val[2];

	return size;
}

static ssize_t suspend_frequency_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", suspend_freq);
}

static ssize_t suspend_frequency_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
	unsigned int ret, val = 0;
	unsigned int min_freq, max_freq;
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get(0);
	min_freq = policy->min;
	max_freq = policy->max;

	ret = sscanf(buf, "%u\n", &val);
	if (ret != 1 || val < min_freq || val > max_freq)
		return -EINVAL;

	suspend_freq = val;

	return size;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%d\n", MAKO_HOTPLUG_VERSION);
}

static DEVICE_ATTR(load_levels, 0644, load_levels_show, load_levels_store);
static DEVICE_ATTR(suspend_frequency, 0644, suspend_frequency_show,
		   suspend_frequency_store);
static DEVICE_ATTR(version, 0400, version_show, NULL);

static struct attribute *mako_hotplug_attributes[] = {
	&dev_attr_load_levels.attr,
	&dev_attr_suspend_frequency.attr,
	&dev_attr_version.attr,
	NULL
};

static struct attribute_group mako_hotplug_group = {
	.attrs = mako_hotplug_attributes,
};

static struct miscdevice mako_hotplug_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mako_hotplug",
};

static int __init mako_hotplug_init(void)
{
	int ret;

	/* init everything here */
	stats.time_stamp = 0;
	stats.online_cpus = num_online_cpus();
	stats.total_cpus = num_present_cpus();
	default_first_level = DEFAULT_FIRST_LEVEL;
	default_second_level = DEFAULT_SECOND_LEVEL;
	default_third_level = DEFAULT_THIRD_LEVEL;
	suspend_freq = DEFAULT_SUSPEND_FREQ;

	wq = alloc_workqueue("mako_hotplug_workqueue",
			     WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);

	if (!wq)
		return -ENOMEM;

	ret = misc_register(&mako_hotplug_device);
	if (ret) {
		pr_err("Failed to register %s device!\n",
		       mako_hotplug_device.name);
		return ret;
	}

	ret =
	    sysfs_create_group(&mako_hotplug_device.this_device->kobj,
			       &mako_hotplug_group);
	if (ret) {
		pr_err("Failed to create sysfs group for %s device!\n",
		       mako_hotplug_device.name);
		return ret;
	}

	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	queue_delayed_work_on(0, wq, &decide_hotplug, HZ * 25);

	register_early_suspend(&mako_hotplug_suspend);

	return ret;
}

late_initcall(mako_hotplug_init);
