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

#define MAKO_HOTPLUG_VERSION 2

/* threshold for comparing time diffs */
#define SEC_THRESHOLD         2000
#define HISTORY_SIZE          10
#define DEFAULT_FIRST_LEVEL   80
#define DEFAULT_SECOND_LEVEL  40
#define DEFAULT_THIRD_LEVEL   25
#define DEFAULT_FOURTH_LEVEL  50
#define DEFAULT_SUSPEND_FREQ  702000

static unsigned int suspend_freq;
static unsigned int load_history[HISTORY_SIZE];
static unsigned int counter;

/* from msm_rq_stats */
extern unsigned int report_load_at_max_freq(void);

struct cpu_stats {
	/* variable to be accessed to filter spurious load spikes */
	unsigned int default_first_level;
	unsigned int default_second_level;
	unsigned int default_third_level;
	unsigned int default_fourth_level;
	unsigned long time_stamp;
	unsigned int online_cpus;
	unsigned int total_cpus;
};

static struct cpu_stats stats;
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static void high_load_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	/* lets bail if all cores are online */
	if (stats.online_cpus == stats.total_cpus)
		return;

	if ((now - stats.time_stamp) >= temp_diff) {
		cpufreq_governor_load_tuning(GOV_TUNE_HIGH);

		for_each_possible_cpu(cpu) {
			if (cpu && !cpu_online(cpu)) {
				cpu_up(cpu);
				pr_debug
				    ("mako_hotplug: cpu%d is up - high load\n",
				     cpu);
			}
		}

		stats.time_stamp = now;
	}
}

static void medium_load_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	/* lets bail if all cores are online */
	if (stats.online_cpus == stats.total_cpus)
		return;

	if (stats.online_cpus == 1 || (now - stats.time_stamp) >= temp_diff) {
		cpufreq_governor_load_tuning(GOV_TUNE_MEDIUM);

		for_each_possible_cpu(cpu) {
			if (cpu && !cpu_online(cpu)) {
				cpu_up(cpu);
				pr_debug
				    ("mako_hotplug: cpu%d is up - medium/high load\n",
				     cpu);
				break;
			}
		}

		stats.time_stamp = now;
	}
}

static void low_load_work_check(unsigned long temp_diff, unsigned long now)
{
	unsigned int cpu = nr_cpu_ids;

	/* lets bail if all cores are offline */
	if (stats.online_cpus == 1)
		return;

	if ((now - stats.time_stamp) >= temp_diff) {
		cpufreq_governor_load_tuning(GOV_TUNE_LOW);

		for_each_online_cpu(cpu) {
			if (cpu) {
				cpu_down(cpu);
				pr_debug
				    ("mako_hotplug: cpu%d is down - low load\n",
				     cpu);
			}
		}

		stats.time_stamp = now;
	}
}

static void __cpuinit decide_hotplug_func(struct work_struct *work)
{
	unsigned long now;
	unsigned int i, j, load = 0;
	unsigned int first_level, second_level, third_level, fourth_level;

	load_history[counter] = report_load_at_max_freq();

	for (i = 0, j = counter; i < HISTORY_SIZE; i++, j--) {
		load += load_history[j];

		if (j == 0)
			j = HISTORY_SIZE;
	}

	if (++counter == HISTORY_SIZE)
		counter = 0;

	load = load / HISTORY_SIZE;
	/* finish load routines */

	/* time of this sampling time */
	now = ktime_to_ms(ktime_get());

	stats.online_cpus = num_online_cpus();

	/* the load thresholds scale with the number of online cpus */
	first_level = stats.default_first_level * stats.online_cpus;
	second_level = stats.default_second_level * stats.online_cpus;
	third_level = stats.default_third_level * stats.online_cpus;
	fourth_level = stats.default_fourth_level * stats.online_cpus;

	if (load >= first_level) {
		high_load_work_check(SEC_THRESHOLD, now);
		queue_delayed_work_on(0, wq, &decide_hotplug,
				      msecs_to_jiffies(HZ));
		return;
	}

	else if (load >= second_level
		 || (load >= third_level && stats.online_cpus == 1)) {
		/*
		 * In the medium/high zone, double the seconds threshold
		 * because a check onlines cpu1 bypassing the time_diff.
		 * Afterwards it takes at least 4 seconds as threshold
		 * before onlining another cpu. This eliminates unneeded
		 * onlining when we are for example swiping between home or
		 * app drawer and we only need cpu0 and cpu1 online for
		 * that. cpufreq takes care of the rest.
		 */
		medium_load_work_check(SEC_THRESHOLD * 2, now);
		queue_delayed_work_on(0, wq, &decide_hotplug,
				      msecs_to_jiffies(HZ));
		return;
	}

	else if (load >= third_level && stats.online_cpus == 2) {
		/*
		 * If two cpus are online while load is in the medium/low
		 * zone, then its more than likely that the user is
		 * interacting with the UI, so instead of onlinig/offlining
		 * cpu1 every now and then, lets keep it online until the
		 * user is not interacting anymore. This should save some
		 * resources that are inherent to the hotplugging routines.
		 */
		pr_debug("mako_hotplug: cpu0 and cpu1 up - medium/low load\n");
		queue_delayed_work_on(0, wq, &decide_hotplug,
				      msecs_to_jiffies(HZ));
		return;
	}

	else if (load <= fourth_level && stats.online_cpus > 1) {
		/* low load obliterate the cpus to death */
		low_load_work_check(SEC_THRESHOLD, now);
	}

	queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(HZ));
}

static void mako_hotplug_early_suspend(struct early_suspend *handler)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	/* cancel the hotplug work when the screen is off and flush the WQ */
	flush_workqueue(wq);
	cancel_delayed_work_sync(&decide_hotplug);
	pr_info("mako_hotplug: Early suspend - stopping Hotplug work...\n");

	low_load_work_check(0, ktime_to_ms(ktime_get()));

	cpufreq_governor_load_tuning(GOV_TUNE_SUSPEND);

	/* cap max frequency to 702MHz by default */
	msm_cpufreq_set_freq_limits(0, policy->min, suspend_freq);
	pr_info
	    ("mako_hotplug: Early suspend - cpu%d max freq: %dMHz\n",
	     0, suspend_freq / 1000);

	stats.online_cpus = num_online_cpus();
}

static void __ref mako_hotplug_late_resume(struct early_suspend *handler)
{
	unsigned int cpu = nr_cpu_ids;
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	cpufreq_governor_load_tuning(GOV_TUNE_HIGH);

	/* online all cores when the screen goes online */
	for_each_possible_cpu(cpu) {
		if (cpu && !cpu_online(cpu)) {
			cpu_up(cpu);
			pr_debug("mako_hotplug: Late resume - cpu%d is up\n",
				 cpu);
		}
		/* restore default max frequency */
		msm_cpufreq_set_freq_limits(cpu, policy->min, policy->max);
	}

	pr_info("mako_hotplug: Late resume - restore cpu%d max frequency\n", 0);

	/* new time_stamp and online_cpu because all cpus were just onlined */
	stats.time_stamp = ktime_to_ms(ktime_get());
	stats.online_cpus = num_online_cpus();

	pr_info("mako_hotplug: Late resume - starting Hotplug work...\n");
	queue_delayed_work_on(0, wq, &decide_hotplug, HZ);
}

static struct early_suspend mako_hotplug_suspend = {
	.suspend = mako_hotplug_early_suspend,
	.resume = mako_hotplug_late_resume,
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 1,
};

static ssize_t load_levels_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u %u %u %u\n", stats.default_first_level,
		       stats.default_second_level, stats.default_third_level,
		       stats.default_fourth_level);
}

static ssize_t load_levels_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	int i, ret = 0;
	unsigned int val[4];

	ret = sscanf(buf, "%u %u %u %u", &val[0], &val[1], &val[2], &val[3]);
	if (ret != ARRAY_SIZE(val))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(val); i++) {
		if (val[i] <= 0 || val[i] >= 100)
			return -EINVAL;
	}

	stats.default_first_level = val[0];
	stats.default_second_level = val[1];
	stats.default_third_level = val[2];
	stats.default_fourth_level = val[3];

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
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	if (!policy)
		return -EINVAL;

	min_freq = policy->min;
	max_freq = policy->max;

	ret = sscanf(buf, "%u", &val);
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
	stats.default_first_level = DEFAULT_FIRST_LEVEL;
	stats.default_second_level = DEFAULT_SECOND_LEVEL;
	stats.default_third_level = DEFAULT_THIRD_LEVEL;
	stats.default_fourth_level = DEFAULT_FOURTH_LEVEL;
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
