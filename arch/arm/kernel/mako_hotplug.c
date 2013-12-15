/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Simple no bullshit hot[un]plug driver for SMP
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
#include <linux/input.h>
#include <linux/lcd_notify.h>

#include <mach/cpufreq.h>

#define DEFAULT_FIRST_LEVEL 70
#define DEFAULT_SUSPEND_FREQ 883200
#define DEFAULT_CORES_ON_TOUCH 2
#define HIGH_LOAD_COUNTER 20
#define TIMER HZ
#define GPU_BUSY_THRESHOLD 60
#define DEFAULT_FIXED_CORES 0
#define NUM_CORES 4

/*
 * 1000ms = 1 second
 */
#define MIN_TIME_CPU_ONLINE_MS 2000

static struct cpu_stats
{
    unsigned int default_first_level;
    unsigned int suspend_frequency;
    unsigned int cores_on_touch;
    unsigned int counter[2];
	u64 timestamp[2];
	struct notifier_block notif;
	bool gpu_busy_quad_mode;
	unsigned int fixed_cores;
	bool first_boot;
} stats = {
	.default_first_level = DEFAULT_FIRST_LEVEL,
    .suspend_frequency = DEFAULT_SUSPEND_FREQ,
    .cores_on_touch = DEFAULT_CORES_ON_TOUCH,
    .counter = {0},
	.timestamp = {0},
	.gpu_busy_quad_mode = false,
	.fixed_cores = DEFAULT_FIXED_CORES,
	.first_boot = true,
};

struct cpu_load_data {
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
};

static unsigned int old_fixed_cores = DEFAULT_FIXED_CORES;

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static struct workqueue_struct *wq;
static struct workqueue_struct *screen_on_off_wq;
static struct delayed_work decide_hotplug;
static struct work_struct suspend;
static struct work_struct resume;

static inline int get_cpu_load(unsigned int cpu)
{
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	struct cpufreq_policy policy;
	u64 cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;
	unsigned int cur_load;

	cpufreq_get_policy(&policy, cpu);

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, true);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	return (cur_load * policy.cur) / policy.max;
}

/* 
 * this is just here as a reminder, currently not sure I want to keep this
 * feature 
 */
#if 0
	if (stats.gpu_busy_quad_mode && 
			unlikely(gpu_pref_counter >= GPU_BUSY_THRESHOLD))
	{
		if (num_online_cpus() < num_possible_cpus())
		{
			for_each_possible_cpu(cpu)
			{
				if (cpu && cpu_is_offline(cpu))
					cpu_up(cpu);
			}
		}

		return;
	}
#endif

static void cpu_revive(unsigned int cpu)
{
	cpu_up(cpu);
	stats.timestamp[cpu - 2] = jiffies;
}

static void cpu_smash(unsigned int cpu)
{
	/*
	 * Let's not unplug this cpu unless its been online for longer than
	 * 1sec to avoid consecutive ups and downs if the load is varying
	 * closer to the threshold point.
	 */
	if (jiffies + msecs_to_jiffies(MIN_TIME_CPU_ONLINE_MS)
		> stats.timestamp[cpu - 2])
	{
		cpu_down(cpu);
		stats.counter[cpu - 2] = 0;
	}
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
    int cpu;
    unsigned int cur_load;
	int i;

	//commented for now
	/*	
	if (_ts->ts_data.curr_data[0].state == ABS_PRESS)
	{
		for (i = num_online_cpus(); i < stats.cores_on_touch; i++)
		{
			if (cpu_is_offline(i))
			{
				cpu_up(i);
				stats.timestamp[i-2] = ktime_to_ms(ktime_get());
			}
		}
		goto re_queue;
	}*/

	/*
	 * Fixed Core Logic - Simone Renzo (simone201)
	 * Allows the user to set a fixed amount of online cores
	 * to gain more power or save a lot of battery by going in single core mode
	 */
	if(stats.fixed_cores == 0 && old_fixed_cores != 0) {
		stats.counter[0] = 0;
		stats.counter[1] = 0;
		
		for_each_possible_cpu(cpu) {
			if (cpu_is_offline(cpu) && cpu)
				cpu_up(cpu);
		}
		
		flush_workqueue(wq);
		cancel_delayed_work(&decide_hotplug);
		
		old_fixed_cores = stats.fixed_cores;
		
		goto reschedule;
	} else if(stats.fixed_cores > 0) {
		for_each_possible_cpu(cpu) {
			if (cpu_online(cpu) && cpu)
				cpu_down(cpu);
		}
		
		for(i = 1; i < stats.fixed_cores; i++) {
			if(cpu_is_offline(i))
				cpu_up(i);
		}
		
		goto reschedule;
	}
	/* END FIXED CORE LOGIC */

    for_each_online_cpu(cpu) 
    {
		cur_load = get_cpu_load(cpu);

		if (cur_load >= stats.default_first_level)
		{
			if (likely(stats.counter[cpu] < HIGH_LOAD_COUNTER))    
				stats.counter[cpu] += 2;

			if (cpu_is_offline(cpu + 2) && stats.counter[cpu] > 10)
				cpu_revive(cpu + 2);
		}

		else
		{
			if (stats.counter[cpu])
				--stats.counter[cpu];

			if (cpu_online(cpu + 2) && stats.counter[cpu] < 10)
				cpu_smash(cpu + 2);
		}

		if (cpu)
			break;
	}

reschedule:	
    queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(TIMER));
}

static void hotplug_suspend(struct work_struct *work)
{	 
    int cpu;

    /* First flush the WQ then cancel the hotplug work when the screen is off */
	flush_workqueue(wq);
    cancel_delayed_work(&decide_hotplug);

    pr_info("Suspend stopping Hotplug work...\n");

	/* reset the counters so that we start clean next time the display is on */
	stats.counter[0] = 0;
	stats.counter[1] = 0;

	for_each_possible_cpu(cpu)
	{
		if (cpu_online(cpu) && cpu)
			cpu_down(cpu);
	}
}

static void __ref hotplug_resume(struct work_struct *work)
{  
    int cpu;

	for_each_possible_cpu(cpu)
	{
		if (cpu_is_offline(cpu) && cpu)
			cpu_up(cpu);
	}
    
    pr_info("Resume starting Hotplug work...\n");
    queue_delayed_work(wq, &decide_hotplug, HZ * 2);
}

static int __ref lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_START:
		if (stats.first_boot)
			break;

		pr_info("LCD is on.\n");
		queue_work(screen_on_off_wq, &resume);
		break;
	case LCD_EVENT_ON_END:
		break;
	case LCD_EVENT_OFF_START:
		pr_info("LCD is off.\n");
		queue_work(screen_on_off_wq, &suspend);
		stats.first_boot = false;
		break;
	case LCD_EVENT_OFF_END:
		break;
	default:
		break;
	}

	return 0;
}

/* sysfs functions for external driver */
void update_first_level(unsigned int level)
{
    stats.default_first_level = level;
}

void update_suspend_frequency(unsigned int freq)
{
    stats.suspend_frequency = freq;
}

void update_cores_on_touch(unsigned int num)
{
    stats.cores_on_touch = num;
}

void update_gpu_busy_quad_mode(unsigned int num)
{
	stats.gpu_busy_quad_mode = num;
}

void update_fixed_cores(unsigned int num)
{	
	old_fixed_cores = stats.fixed_cores;
	stats.fixed_cores = num;
}

unsigned int get_first_level()
{
    return stats.default_first_level;
}

unsigned int get_suspend_frequency()
{
    return stats.suspend_frequency;
}

unsigned int get_cores_on_touch()
{
    return stats.cores_on_touch;
}

unsigned int get_gpu_busy_quad_mode()
{
	return stats.gpu_busy_quad_mode;
}

unsigned int get_fixed_cores()
{
	return stats.fixed_cores;
}

/* end sysfs functions from external driver */

int __init mako_hotplug_init(void)
{
	pr_info("Mako Hotplug driver started.\n");

    wq = alloc_ordered_workqueue("mako_hotplug_workqueue", 0);
    
    if (!wq)
        return -ENOMEM;

	screen_on_off_wq = alloc_workqueue("screen_on_off_workqueue", 0, 0);
    
    if (!screen_on_off_wq)
        return -ENOMEM;

	stats.notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&stats.notif))
		return -EINVAL;
    
	INIT_WORK(&suspend, hotplug_suspend);
	INIT_WORK(&resume, hotplug_resume);
    INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);

	queue_delayed_work(wq, &decide_hotplug, HZ * 20);
    
    return 0;
}
late_initcall(mako_hotplug_init);

