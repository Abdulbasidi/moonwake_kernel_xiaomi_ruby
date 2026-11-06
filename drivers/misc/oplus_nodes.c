// SPDX-License-Identifier: GPL-2.0-only
/*
 * Oplus/ColorOS proc interfaces compat nodes (COS17 port).
 *
 * This is a small, self-contained driver that mirrors the shape of the
 * proc nodes shipped by the Oplus kernel modules in
 * vendor/oplus/kernel (mt6877) so that ColorOS userspace services
 * (SchedAssist, PerformanceService, Theia/bscheck, analytics) find their
 * expected interfaces:
 *
 *   /proc/oplus_binder/{ux_flag,async_ux_enable,set_last_async_ux,
 *                       all_tasks_ux_sts}
 *   /proc/bootprof, /proc/opbootfrom
 *   /proc/last_boot_reason
 *   /proc/phoenix
 *   /proc/jank_info
 *   /proc/theiaPwkReport
 *
 * Honest scope note: these are interface-compatible implementations, not
 * feature-parity ports. In particular, writing to ux_flag records the
 * value (annotated per-pid), but the real binder transaction scheduling
 * hooks (vendor/oplus/kernel/ipc/binder_sched.c) are NOT ported. The nodes
 * exist to keep the services from failing on missing files.
 *
 * NOTE: this tree's proc_fs.h predates struct proc_ops, so plain
 * struct file_operations is used everywhere.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/kstrtox.h>
#include <linux/fs.h>

#define pr_fmt(fmt) "oplus_nodes: " fmt

extern unsigned long long notrace sched_clock(void);

/* -------------------------------------------------------------------- */
/* /proc/oplus_binder/*                                                 */
/* -------------------------------------------------------------------- */

#define OPLUS_BINDER_DIR "oplus_binder"

static unsigned int g_async_ux_enable;
static unsigned int g_set_last_async_ux;

#define UX_MAP_SIZE 64

struct ux_entry {
	pid_t pid;
	int enable;
};

static struct ux_entry ux_map[UX_MAP_SIZE];
static int ux_map_count;
static DEFINE_MUTEX(ux_map_lock);

static void ux_set_enable(pid_t pid, int enable)
{
	int i;

	mutex_lock(&ux_map_lock);
	for (i = 0; i < ux_map_count; i++) {
		if (ux_map[i].pid == pid) {
			ux_map[i].enable = enable;
			goto out;
		}
	}
	if (ux_map_count < UX_MAP_SIZE) {
		ux_map[ux_map_count].pid = pid;
		ux_map[ux_map_count].enable = enable;
		ux_map_count++;
	}
out:
	mutex_unlock(&ux_map_lock);
}

static int ux_get_enable(pid_t pid)
{
	int i, ret = 0;

	mutex_lock(&ux_map_lock);
	for (i = 0; i < ux_map_count; i++) {
		if (ux_map[i].pid == pid) {
			ret = ux_map[i].enable;
			break;
		}
	}
	mutex_unlock(&ux_map_lock);
	return ret;
}

static ssize_t proc_async_ux_enable_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	char buffer[8];
	int err, val;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	memset(buffer, 0, sizeof(buffer));
	buffer[count] = '\0';
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	g_async_ux_enable = val;
	return count;
}

static ssize_t proc_async_ux_enable_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	char buffer[20];

	return simple_read_from_buffer(buf, count, ppos, buffer,
		snprintf(buffer, sizeof(buffer), "%d\n", g_async_ux_enable));
}

static ssize_t proc_set_last_async_ux_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	char buffer[8];
	int err, val;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	memset(buffer, 0, sizeof(buffer));
	buffer[count] = '\0';
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	g_set_last_async_ux = val;
	return count;
}

static ssize_t proc_set_last_async_ux_read(struct file *file, char __user *buf,
					   size_t count, loff_t *ppos)
{
	char buffer[20];

	return simple_read_from_buffer(buf, count, ppos, buffer,
		snprintf(buffer, sizeof(buffer), "%d\n", g_set_last_async_ux));
}

enum {
	BINDER_OPT_STR_TYPE = 0,
	BINDER_OPT_STR_PID,
	BINDER_OPT_STR_VAL,
	BINDER_OPT_STR_MAX = 3,
};

static ssize_t proc_async_ux_flag_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	char buffer[64];
	char *str, *token;
	char opt_str[BINDER_OPT_STR_MAX][8];
	int cnt = 0;
	int pid = 0, ux_flag = 0, err = 0;

	if (!g_async_ux_enable)
		return count;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	memset(buffer, 0, sizeof(buffer));
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	str = strstrip(buffer);
	while ((token = strsep(&str, " ")) && *token &&
	       (cnt < BINDER_OPT_STR_MAX)) {
		strlcpy(opt_str[cnt], token, sizeof(opt_str[cnt]));
		cnt++;
	}

	if (cnt != BINDER_OPT_STR_MAX) {
		if (cnt == (BINDER_OPT_STR_MAX - 1) &&
		    !strncmp(opt_str[BINDER_OPT_STR_TYPE], "r", 1)) {
			err = kstrtoint(strstrip(opt_str[BINDER_OPT_STR_PID]),
					10, &pid);
			if (err)
				return err;
			return count;
		}
		return -EFAULT;
	}

	err = kstrtoint(strstrip(opt_str[BINDER_OPT_STR_PID]), 10, &pid);
	if (err)
		return err;

	err = kstrtoint(strstrip(opt_str[BINDER_OPT_STR_VAL]), 10, &ux_flag);
	if (err)
		return err;

	if (!strncmp(opt_str[BINDER_OPT_STR_TYPE], "p", 1))
		ux_set_enable(pid, ux_flag);

	return count;
}

static ssize_t proc_async_ux_flag_read(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos)
{
	char buffer[256];
	struct task_struct *task = current;

	return simple_read_from_buffer(buf, count, ppos, buffer,
		snprintf(buffer, sizeof(buffer),
			 "comm=%s pid=%d tgid=%d enable=%d\n",
			 task->comm, task->pid, task->tgid,
			 ux_get_enable(task->pid)));
}

static ssize_t proc_binder_all_tasks_async_ux_read(struct file *file,
						   char __user *buf,
						   size_t count, loff_t *ppos)
{
	const char msg[] = "pls check dmesg [async_ux_tasks]\n";

	return simple_read_from_buffer(buf, count, ppos, msg,
				       sizeof(msg) - 1);
}

static const struct file_operations proc_async_ux_enable_fops = {
	.owner = THIS_MODULE,
	.write = proc_async_ux_enable_write,
	.read = proc_async_ux_enable_read,
	.llseek = default_llseek,
};

static const struct file_operations proc_set_last_async_ux_fops = {
	.owner = THIS_MODULE,
	.write = proc_set_last_async_ux_write,
	.read = proc_set_last_async_ux_read,
	.llseek = default_llseek,
};

static const struct file_operations proc_binder_async_ux_flag_fops = {
	.owner = THIS_MODULE,
	.write = proc_async_ux_flag_write,
	.read = proc_async_ux_flag_read,
	.llseek = default_llseek,
};

static const struct file_operations proc_binder_all_tasks_ux_sts_fops = {
	.owner = THIS_MODULE,
	.read = proc_binder_all_tasks_async_ux_read,
	.llseek = default_llseek,
};

static struct proc_dir_entry *oplus_binder_dir;

static int oplus_binder_init(void)
{
	struct proc_dir_entry *node;

	oplus_binder_dir = proc_mkdir(OPLUS_BINDER_DIR, NULL);
	if (!oplus_binder_dir) {
		pr_err("failed to create proc dir %s\n", OPLUS_BINDER_DIR);
		return -ENOMEM;
	}

	node = proc_create("async_ux_enable", 0666, oplus_binder_dir,
			   &proc_async_ux_enable_fops);
	if (!node)
		goto err1;

	node = proc_create("set_last_async_ux", 0666, oplus_binder_dir,
			   &proc_set_last_async_ux_fops);
	if (!node)
		goto err2;

	node = proc_create("ux_flag", 0666, oplus_binder_dir,
			   &proc_binder_async_ux_flag_fops);
	if (!node)
		goto err3;

	node = proc_create("all_tasks_ux_sts", 0444, oplus_binder_dir,
			   &proc_binder_all_tasks_ux_sts_fops);
	if (!node)
		goto err4;

	return 0;

err4:
	remove_proc_entry("ux_flag", oplus_binder_dir);
err3:
	remove_proc_entry("set_last_async_ux", oplus_binder_dir);
err2:
	remove_proc_entry("async_ux_enable", oplus_binder_dir);
err1:
	remove_proc_entry(OPLUS_BINDER_DIR, NULL);
	oplus_binder_dir = NULL;
	return -ENOMEM;
}

/* -------------------------------------------------------------------- */
/* /proc/bootprof + /proc/opbootfrom                                    */
/* (mirrors vendor/oplus/kernel/system/oplus_phoenix/bootprof)          */
/* -------------------------------------------------------------------- */

#define BOOT_LOG_NUM 192
#define BOOT_EVENT_LEN 128
#define BOOT_FROM_SIZE 16

struct boot_log_entry {
	u64 timestamp;
	char msg[BOOT_EVENT_LEN];
};

static struct boot_log_entry boot_log[BOOT_LOG_NUM];
static int boot_log_n;
static DEFINE_MUTEX(boot_log_lock);
static char boot_from[BOOT_FROM_SIZE];

void op_log_boot_event(const char *msg)
{
	unsigned long long ts = sched_clock();
	struct boot_log_entry *e;

	mutex_lock(&boot_log_lock);
	if (boot_log_n < BOOT_LOG_NUM) {
		e = &boot_log[boot_log_n++];
		e->timestamp = ts;
		snprintf(e->msg, sizeof(e->msg), "%lld.%06lld %s",
			 (long long)(ts / 1000000),
			 (long long)(ts % 1000000), msg);
	}
	mutex_unlock(&boot_log_lock);
}
EXPORT_SYMBOL_GPL(op_log_boot_event);

static int op_bootprof_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&boot_log_lock);
	for (i = 0; i < boot_log_n; i++)
		seq_printf(m, "%s\n", boot_log[i].msg);
	mutex_unlock(&boot_log_lock);
	return 0;
}

static ssize_t op_bootprof_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char msg[BOOT_EVENT_LEN];

	if (count >= sizeof(msg))
		count = sizeof(msg) - 1;
	msg[count] = '\0';
	if (copy_from_user(msg, buf, count))
		return -EFAULT;
	op_log_boot_event(strstrip(msg));
	return count;
}

static int op_bootprof_open(struct inode *inode, struct file *file)
{
	return single_open(file, op_bootprof_show, inode->i_private);
}

static const struct file_operations op_bootprof_fops = {
	.owner = THIS_MODULE,
	.open = op_bootprof_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = op_bootprof_write,
};

static int op_bootfrom_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", boot_from);
	return 0;
}

static int op_bootfrom_open(struct inode *inode, struct file *file)
{
	return single_open(file, op_bootfrom_show, inode->i_private);
}

static const struct file_operations op_bootfrom_fops = {
	.owner = THIS_MODULE,
	.open = op_bootfrom_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* -------------------------------------------------------------------- */
/* /proc/last_boot_reason                                               */
/* -------------------------------------------------------------------- */

#define LAST_BOOT_REASON_MAX 512

static char last_boot_reason_str[LAST_BOOT_REASON_MAX] = "unknown";
static DEFINE_MUTEX(lbr_lock);

static int last_boot_reason_show(struct seq_file *m, void *v)
{
	mutex_lock(&lbr_lock);
	seq_printf(m, "%s\n", last_boot_reason_str);
	mutex_unlock(&lbr_lock);
	return 0;
}

static ssize_t last_boot_reason_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char reason[LAST_BOOT_REASON_MAX];

	if (count >= sizeof(reason))
		count = sizeof(reason) - 1;
	reason[count] = '\0';
	if (copy_from_user(reason, buf, count))
		return -EFAULT;

	mutex_lock(&lbr_lock);
	strlcpy(last_boot_reason_str, strstrip(reason),
		sizeof(last_boot_reason_str));
	mutex_unlock(&lbr_lock);
	return count;
}

static int last_boot_reason_open(struct inode *inode, struct file *file)
{
	return single_open(file, last_boot_reason_show, inode->i_private);
}

static const struct file_operations last_boot_reason_fops = {
	.owner = THIS_MODULE,
	.open = last_boot_reason_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = last_boot_reason_write,
};

static int lbr_reboot_notifier(struct notifier_block *nb, unsigned long mode,
			       void *cmd)
{
	if (mode == SYS_RESTART && cmd) {
		mutex_lock(&lbr_lock);
		strlcpy(last_boot_reason_str, (const char *)cmd,
			sizeof(last_boot_reason_str));
		mutex_unlock(&lbr_lock);
	}
	return NOTIFY_DONE;
}

static struct notifier_block lbr_reboot_nb = {
	.notifier_call = lbr_reboot_notifier,
	.priority = 10,
};

/* -------------------------------------------------------------------- */
/* /proc/phoenix + /proc/jank_info + /proc/theiaPwkReport               */
/* -------------------------------------------------------------------- */

static int phoenix_show(struct seq_file *m, void *v)
{
	seq_printf(m, "phoenix: enabled (compat node)\n");
	seq_printf(m, "kernel: %s\n", UTS_RELEASE);
	seq_printf(m, "uptime: %llu\n", (unsigned long long)ktime_get() / HZ);
	return 0;
}

static int jank_info_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "jank_info: compat node, no jank counters recorded\n");
	mutex_lock(&boot_log_lock);
	for (i = 0; i < boot_log_n; i++)
		seq_printf(m, "%s\n", boot_log[i].msg);
	mutex_unlock(&boot_log_lock);
	return 0;
}

static int theia_pwk_report_show(struct seq_file *m, void *v)
{
	seq_printf(m, "theia: disabled (compat node)\n");
	seq_printf(m, "uptime: %llu\n", (unsigned long long)ktime_get() / HZ);
	return 0;
}

#define MK_SINGLE_FOPS(name)						\
static int name##_open(struct inode *inode, struct file *file)		\
{									\
	return single_open(file, name##_show, inode->i_private);		\
}									\
static const struct file_operations name##_fops = {			\
	.owner = THIS_MODULE,					\
	.open = name##_open,					\
	.read = seq_read,					\
	.llseek = seq_lseek,				\
	.release = single_release,				\
}

MK_SINGLE_FOPS(phoenix)
MK_SINGLE_FOPS(jank_info)
MK_SINGLE_FOPS(theia_pwk_report)

/* -------------------------------------------------------------------- */
/* module init/exit                                                     */
/* -------------------------------------------------------------------- */

static int __init oplus_nodes_init(void)
{
	struct proc_dir_entry *pe;

	/* Donor bootprof records boot from the same source; keep it visible. */
	strlcpy(boot_from, last_boot_reason_str, sizeof(boot_from));

	op_log_boot_event("oplus_nodes_init");
	op_log_boot_event("COS17 compat nodes ready");

	if (register_reboot_notifier(&lbr_reboot_nb))
		pr_warn("could not register reboot notifier\n");

	if (oplus_binder_init()) {
		pr_err("oplus_binder compat nodes failed\n");
		goto out_binder;
	}

	pe = proc_create("bootprof", 0666, NULL, &op_bootprof_fops);
	if (!pe)
		goto out_bootprof;
	pe = proc_create("opbootfrom", 0666, NULL, &op_bootfrom_fops);
	if (!pe)
		goto out_bootfrom;
	pe = proc_create("last_boot_reason", 0666, NULL,
			 &last_boot_reason_fops);
	if (!pe)
		goto out_lbr;
	pe = proc_create("phoenix", 0444, NULL, &phoenix_fops);
	if (!pe)
		goto out_phoenix;
	pe = proc_create("jank_info", 0444, NULL, &jank_info_fops);
	if (!pe)
		goto out_jank;
	pe = proc_create("theiaPwkReport", 0444, NULL, &theia_pwk_report_fops);
	if (!pe)
		goto out_theia;

	pr_info("ColorOS compat proc nodes ready\n");
	return 0;

out_theia:
	remove_proc_entry("jank_info", NULL);
out_jank:
	remove_proc_entry("phoenix", NULL);
out_phoenix:
	remove_proc_entry("last_boot_reason", NULL);
out_lbr:
	remove_proc_entry("opbootfrom", NULL);
out_bootfrom:
	remove_proc_entry("bootprof", NULL);
out_bootprof:
out_binder:
	return -ENOMEM;
}

static void __exit oplus_nodes_exit(void)
{
	remove_proc_entry("theiaPwkReport", NULL);
	remove_proc_entry("jank_info", NULL);
	remove_proc_entry("phoenix", NULL);
	remove_proc_entry("last_boot_reason", NULL);
	remove_proc_entry("opbootfrom", NULL);
	remove_proc_entry("bootprof", NULL);
	remove_proc_entry("all_tasks_ux_sts", oplus_binder_dir);
	remove_proc_entry("ux_flag", oplus_binder_dir);
	remove_proc_entry("set_last_async_ux", oplus_binder_dir);
	remove_proc_entry("async_ux_enable", oplus_binder_dir);
	remove_proc_entry(OPLUS_BINDER_DIR, NULL);
	unregister_reboot_notifier(&lbr_reboot_nb);
}

module_init(oplus_nodes_init);
module_exit(oplus_nodes_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("COS17 port");
MODULE_DESCRIPTION("Oplus/ColorOS proc nodes compat (oplus_binder, bootprof, last_boot_reason, phoenix, jank_info)");
