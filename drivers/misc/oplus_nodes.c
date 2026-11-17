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
 *   /proc/oplus_scheduler/sched_assist/{sched_assist_scene,
 *                       sched_impt_task,im_flag,im_flag_app}
 *   /proc/oplus_afs_config/{afs_config,afs_enable}
 *   /proc/waker_identify/rt_info
 *   /proc/shell-temp
 *   /proc/task_cpustats
 *
 * The scheduler/performance nodes use the exact read formats of the OPLUS
 * 4.19 sources (kernel/sched_assist/sched_assist_common.c and
 * kernel/oplus_cpu/sched/sched_assist/sa_sysfs.c), but no scheduler state
 * is changed: the values are only recorded and returned.  Real sched_assist
 * hooks would need task_struct changes and scheduler patches, which this
 * compat driver deliberately does not do.
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
#include <linux/rcupdate.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/threads.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/utsname.h>

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
	seq_printf(m, "kernel: %s\n", utsname()->release);
	seq_printf(m, "uptime: %llu\n", ktime_get_seconds());
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
	seq_printf(m, "uptime: %llu\n", ktime_get_seconds());
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
};

MK_SINGLE_FOPS(phoenix)
MK_SINGLE_FOPS(jank_info)
MK_SINGLE_FOPS(theia_pwk_report)

/* -------------------------------------------------------------------- */
/* /proc/oplus_scheduler/sched_assist/*                                 */
/* -------------------------------------------------------------------- */

#define OPLUS_SCHEDULER_DIR	"oplus_scheduler"
#define OPLUS_SCHED_ASSIST_DIR	"sched_assist"

/*
 * Scene bits: writing 0 clears everything, bit 7 is the "set" modifier
 * (the bits below it are ORed in), any other value clears those bits.
 * This mirrors proc_sched_assist_scene_write() in the OPLUS 4.19 sources.
 */
#define SA_SCENE_OPT_CLEAR	0
#define SA_SCENE_OPT_SET	(1 << 7)

static int sa_scene;
static DEFINE_MUTEX(sa_scene_lock);

static ssize_t proc_sa_scene_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, val;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	mutex_lock(&sa_scene_lock);
	if (val == SA_SCENE_OPT_CLEAR) {
		sa_scene = val;
		goto out;
	}

	if (val & SA_SCENE_OPT_SET)
		sa_scene |= val & ~SA_SCENE_OPT_SET;
	else if (val & sa_scene)
		sa_scene &= ~val;
out:
	mutex_unlock(&sa_scene_lock);

	return count;
}

static ssize_t proc_sa_scene_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	char buffer[32];
	size_t len;

	mutex_lock(&sa_scene_lock);
	len = snprintf(buffer, sizeof(buffer), "scene=%d\n", sa_scene);
	mutex_unlock(&sa_scene_lock);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct file_operations proc_sa_scene_fops = {
	.owner = THIS_MODULE,
	.write = proc_sa_scene_write,
	.read = proc_sa_scene_read,
	.llseek = default_llseek,
};

/*
 * im_flag has no task_struct field in this tree, so the values written by
 * userspace are recorded per pid.  The read formats and the "p/r" command
 * grammar are identical to the OPLUS implementations.
 */
#define IM_MAP_SIZE	128

struct im_entry {
	pid_t pid;
	int flag;
};

static struct im_entry im_map[IM_MAP_SIZE];
static int im_map_count;
static int im_map_next;
static DEFINE_MUTEX(im_map_lock);
static pid_t im_flag_pid = -1;

static void im_set_flag(pid_t pid, int flag)
{
	int i;

	mutex_lock(&im_map_lock);
	for (i = 0; i < im_map_count; i++) {
		if (im_map[i].pid == pid) {
			im_map[i].flag = flag;
			goto out;
		}
	}

	if (im_map_count < IM_MAP_SIZE) {
		im_map[im_map_count].pid = pid;
		im_map[im_map_count].flag = flag;
		im_map_count++;
	} else {
		im_map[im_map_next].pid = pid;
		im_map[im_map_next].flag = flag;
		im_map_next = (im_map_next + 1) % IM_MAP_SIZE;
	}
out:
	mutex_unlock(&im_map_lock);
}

static int im_get_flag(pid_t pid)
{
	int i, flag = 0;

	mutex_lock(&im_map_lock);
	for (i = 0; i < im_map_count; i++) {
		if (im_map[i].pid == pid) {
			flag = im_map[i].flag;
			break;
		}
	}
	mutex_unlock(&im_map_lock);

	return flag;
}

static bool im_task_allowed(struct task_struct *task)
{
	return task->tgid == current->tgid;
}

static ssize_t im_flag_common_write(const char __user *buf, size_t count,
				    bool app_only)
{
	char buffer[64];
	char *str, *token;
	char opt_str[3][8];
	int cnt = 0;
	pid_t pid = 0;
	int im_flag = 0;
	int err;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	memset(buffer, 0, sizeof(buffer));
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';

	memset(opt_str, 0, sizeof(opt_str));
	str = strstrip(buffer);
	while ((token = strsep(&str, " ")) && *token && cnt < 3) {
		strlcpy(opt_str[cnt], token, sizeof(opt_str[cnt]));
		cnt++;
	}

	if (cnt != 3) {
		if (cnt == 2 && !strncmp(opt_str[0], "r", 1)) {
			err = kstrtoint(strstrip(opt_str[1]), 10, &pid);
			if (err)
				return err;
			if (pid > 0 && pid <= PID_MAX_DEFAULT)
				WRITE_ONCE(im_flag_pid, pid);
			return count;
		}
		return -EFAULT;
	}

	err = kstrtoint(strstrip(opt_str[1]), 10, &pid);
	if (err)
		return err;

	err = kstrtoint(strstrip(opt_str[2]), 10, &im_flag);
	if (err)
		return err;

	if (strncmp(opt_str[0], "p", 1))
		return count;

	if (pid > 0 && pid <= PID_MAX_DEFAULT) {
		struct task_struct *task;

		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();

		if (task) {
			if (!app_only || im_task_allowed(task))
				im_set_flag(task->pid, im_flag);
			put_task_struct(task);
		}
	}

	return count;
}

static ssize_t im_flag_common_read(char __user *buf, size_t count,
				   loff_t *ppos, bool app_only)
{
	char buffer[256];
	struct task_struct *task = NULL;
	pid_t pid = READ_ONCE(im_flag_pid);
	size_t len;

	if (pid > 0) {
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
	}

	if (task) {
		if (!app_only || im_task_allowed(task))
			len = snprintf(buffer, sizeof(buffer),
				       "comm=%s pid=%d tgid=%d im_flag=%d\n",
				       task->comm, task->pid, task->tgid,
				       im_get_flag(task->pid));
		else
			len = snprintf(buffer, sizeof(buffer),
				       "Can not find task\n");
		put_task_struct(task);
	} else {
		len = snprintf(buffer, sizeof(buffer), "Can not find task\n");
	}

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_im_flag_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return im_flag_common_write(buf, count, false);
}

static ssize_t proc_im_flag_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	return im_flag_common_read(buf, count, ppos, false);
}

static ssize_t proc_im_flag_app_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	return im_flag_common_write(buf, count, true);
}

static ssize_t proc_im_flag_app_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	return im_flag_common_read(buf, count, ppos, true);
}

static const struct file_operations proc_im_flag_fops = {
	.owner = THIS_MODULE,
	.write = proc_im_flag_write,
	.read = proc_im_flag_read,
	.llseek = default_llseek,
};

static const struct file_operations proc_im_flag_app_fops = {
	.owner = THIS_MODULE,
	.write = proc_im_flag_app_write,
	.read = proc_im_flag_app_read,
	.llseek = default_llseek,
};

/* -------------------------------------------------------------------- */
/* /proc/oplus_scheduler/sched_assist/sched_impt_task                   */
/* -------------------------------------------------------------------- */

#define SA_IMPT_PID_MAX	2

static pid_t sa_top_tgid;
static unsigned int sa_top_type;
static pid_t sa_audio_tgid;
static pid_t sa_impt_tgid[SA_IMPT_PID_MAX];
static unsigned int sa_impt_idx;
static DEFINE_MUTEX(sa_impt_lock);

static ssize_t proc_sa_impt_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char temp_buf[32];
	char *temp_str, *token;
	char in_str[2][16];
	int cnt, err, pid;

	if (count > sizeof(temp_buf) - 1)
		return -EFAULT;

	memset(temp_buf, 0, sizeof(temp_buf));
	if (copy_from_user(temp_buf, buf, count))
		return -EFAULT;
	temp_buf[count] = '\0';

	cnt = 0;
	temp_str = strstrip(temp_buf);
	while ((token = strsep(&temp_str, " ")) && *token && cnt < 2) {
		strlcpy(in_str[cnt], token, sizeof(in_str[cnt]));
		cnt++;
	}

	if (cnt != 2)
		return -EFAULT;

	err = kstrtoint(strstrip(in_str[1]), 0, &pid);
	if (err)
		return err;

	if (pid < 0 || pid > PID_MAX_DEFAULT)
		return -EINVAL;

	mutex_lock(&sa_impt_lock);
	if (!strncmp(in_str[0], "fg", 2)) {
		sa_top_tgid = pid;
		sa_top_type = !strncmp(in_str[0], "fgLauncher", 10);
	} else if (!strncmp(in_str[0], "au", 2)) {
		sa_audio_tgid = pid;
	} else if (!strncmp(in_str[0], "addAu", 5)) {
		int i;

		for (i = 0; i < SA_IMPT_PID_MAX; i++)
			if (sa_impt_tgid[i] == pid)
				break;
		if (i >= SA_IMPT_PID_MAX) {
			sa_impt_tgid[sa_impt_idx] = pid;
			sa_impt_idx = (sa_impt_idx + 1) % SA_IMPT_PID_MAX;
		}
	} else if (!strncmp(in_str[0], "remAu", 5)) {
		int i;

		for (i = 0; i < SA_IMPT_PID_MAX; i++) {
			if (sa_impt_tgid[i] == pid) {
				sa_impt_tgid[i] = 0;
				sa_impt_idx = i;
				break;
			}
		}
	}
	mutex_unlock(&sa_impt_lock);

	return count;
}

static ssize_t proc_sa_impt_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	char buffer[32];
	size_t len;

	mutex_lock(&sa_impt_lock);
	len = snprintf(buffer, sizeof(buffer), "top(%d %u) au(%d)\n",
		       sa_top_tgid, sa_top_type, sa_audio_tgid);
	mutex_unlock(&sa_impt_lock);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct file_operations proc_sa_impt_fops = {
	.owner = THIS_MODULE,
	.write = proc_sa_impt_write,
	.read = proc_sa_impt_read,
	.llseek = default_llseek,
};

/* -------------------------------------------------------------------- */
/* /proc/oplus_afs_config/*                                             */
/* -------------------------------------------------------------------- */

#define AFS_PROC_DIR		"oplus_afs_config"
#define AFS_PROC_CONFIG		"afs_config"
#define AFS_PROC_ENABLE		"afs_enable"
#define AFS_PB_PATH		"/system_ext/etc/afsConfig.pb"
#define AFS_BUF_SIZE		4096

/*
 * oplus.fscene.fcore.afsConfig:
 *   field 1 (string)  = "20250515"   version / config date
 *   field 2 (varint)  = 2            schema revision
 *   field 3 (string)  = "true"       feature gate
 *   field 4 (message, repeated) = { 1,1,1,0,1 } and { 2,1,1,0,1 }
 */
static const u8 afs_default_payload[] = {
	0x0a, 0x08, 0x32, 0x30, 0x32, 0x35, 0x30, 0x35,
	0x31, 0x35, 0x10, 0x02, 0x1a, 0x04, 0x74, 0x72,
	0x75, 0x65, 0x22, 0x0a, 0x08, 0x01, 0x10, 0x01,
	0x18, 0x01, 0x20, 0x00, 0x28, 0x01, 0x22, 0x0a,
	0x08, 0x02, 0x10, 0x01, 0x18, 0x01, 0x20, 0x00,
	0x28, 0x01,
};

static u8 afs_buf[AFS_BUF_SIZE];
static size_t afs_len;
static bool afs_loaded;
static bool afs_enabled = true;
static DEFINE_MUTEX(afs_lock);

static void afs_load_locked(void)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t got;

	memset(afs_buf, 0, sizeof(afs_buf));
	afs_len = 0;

	f = filp_open(AFS_PB_PATH, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		got = kernel_read(f, afs_buf, sizeof(afs_buf) - 1, &pos);
		if (got > 0)
			afs_len = (size_t)got;
		filp_close(f, NULL);
	}

	if (!afs_len) {
		memcpy(afs_buf, afs_default_payload,
		       sizeof(afs_default_payload));
		afs_len = sizeof(afs_default_payload);
		pr_info("afs_config: using builtin payload (%zu bytes)\n",
			afs_len);
	} else {
		pr_info("afs_config: loaded %zu bytes from %s\n",
			afs_len, AFS_PB_PATH);
	}
}

static int afs_config_show(struct seq_file *m, void *v)
{
	mutex_lock(&afs_lock);
	if (afs_len)
		seq_write(m, afs_buf, afs_len);
	mutex_unlock(&afs_lock);
	return 0;
}

static int afs_config_open(struct inode *inode, struct file *file)
{
	mutex_lock(&afs_lock);
	if (!afs_loaded) {
		afs_load_locked();
		afs_loaded = true;
	}
	mutex_unlock(&afs_lock);

	return single_open(file, afs_config_show, NULL);
}

static ssize_t afs_config_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	if (len >= AFS_BUF_SIZE)
		return -EINVAL;

	mutex_lock(&afs_lock);
	if (copy_from_user(afs_buf, buf, len)) {
		mutex_unlock(&afs_lock);
		return -EFAULT;
	}
	afs_len = len;
	mutex_unlock(&afs_lock);

	return len;
}

static const struct file_operations afs_config_fops = {
	.owner = THIS_MODULE,
	.open = afs_config_open,
	.read = seq_read,
	.write = afs_config_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int afs_enable_show(struct seq_file *m, void *v)
{
	mutex_lock(&afs_lock);
	seq_printf(m, "%d\n", afs_enabled ? 1 : 0);
	mutex_unlock(&afs_lock);
	return 0;
}

static int afs_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, afs_enable_show, NULL);
}

static ssize_t afs_enable_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	char c;
	bool en;

	if (len == 0)
		return 0;

	if (copy_from_user(&c, buf, 1))
		return -EFAULT;

	switch (c) {
	case '0':
		en = false;
		break;
	case '1':
		en = true;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&afs_lock);
	afs_enabled = en;
	mutex_unlock(&afs_lock);

	return len;
}

static const struct file_operations afs_enable_fops = {
	.owner = THIS_MODULE,
	.open = afs_enable_open,
	.read = seq_read,
	.write = afs_enable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* -------------------------------------------------------------------- */
/* /proc/waker_identify/rt_info                                         */
/* -------------------------------------------------------------------- */

#define WAKER_IDENTIFY_DIR	"waker_identify"
#define WAKER_HANDLE_NUM	10000

static int waker_handle_current = 1;
static DEFINE_MUTEX(waker_lock);

static int rt_info_show(struct seq_file *m, void *v)
{
	mutex_lock(&waker_lock);
	seq_printf(m, "%d", waker_handle_current - 1);
	mutex_unlock(&waker_lock);
	return 0;
}

static int rt_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, rt_info_show, NULL);
}

static ssize_t rt_info_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char page[128];
	struct task_struct *task;
	int pid, uid, for_wakee, ret;

	if (count >= sizeof(page))
		return -EINVAL;

	memset(page, 0, sizeof(page));
	if (copy_from_user(page, buf, count))
		return -EFAULT;

	ret = sscanf(page, "%d;%d;%d", &pid, &uid, &for_wakee);
	if (ret != 3)
		return -EINVAL;

	rcu_read_lock();
	task = (pid > 0) ? find_task_by_vpid(pid) : NULL;
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (task) {
		put_task_struct(task);
		mutex_lock(&waker_lock);
		waker_handle_current = (waker_handle_current + 1) %
				       WAKER_HANDLE_NUM;
		mutex_unlock(&waker_lock);
	}

	return count;
}

static const struct file_operations rt_info_fops = {
	.owner = THIS_MODULE,
	.open = rt_info_open,
	.read = seq_read,
	.write = rt_info_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* -------------------------------------------------------------------- */
/* /proc/shell-temp (write-only, mirrors horae_shell_temp.c)            */
/* -------------------------------------------------------------------- */

#define SHELL_TEMP_MAX	3

static int shell_temp_values[SHELL_TEMP_MAX];
static DEFINE_SPINLOCK(shell_temp_lock);

static ssize_t proc_shell_temp_write(struct file *file,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char tmp[32];
	int index, temp, ret;

	if (count == 0)
		return 0;

	if (count > sizeof(tmp) - 1)
		count = sizeof(tmp) - 1;

	memset(tmp, 0, sizeof(tmp));
	if (copy_from_user(tmp, buf, count))
		return -EFAULT;

	ret = sscanf(tmp, "%d %d", &index, &temp);
	if (ret < 2)
		return count;

	if (index < 0 || index >= SHELL_TEMP_MAX)
		return count;

	spin_lock(&shell_temp_lock);
	shell_temp_values[index] = temp;
	spin_unlock(&shell_temp_lock);

	return count;
}

static const struct file_operations proc_shell_temp_fops = {
	.owner = THIS_MODULE,
	.write = proc_shell_temp_write,
	.llseek = default_llseek,
};

/* -------------------------------------------------------------------- */
/* /proc/task_cpustats                                                  */
/* -------------------------------------------------------------------- */

static int task_cpustats_show(struct seq_file *m, void *v)
{
	/*
	 * The real node is fed by account_task_time() from the scheduler
	 * tick.  No such hook exists in this compat driver, so the table is
	 * empty; readers handle that as "no samples" instead of ENOENT.
	 */
	return 0;
}

MK_SINGLE_FOPS(task_cpustats)

/* -------------------------------------------------------------------- */
/* performance node registration                                        */
/* -------------------------------------------------------------------- */

static struct proc_dir_entry *oplus_sched_dir;
static struct proc_dir_entry *oplus_sa_dir;
static struct proc_dir_entry *oplus_afs_dir;
static struct proc_dir_entry *oplus_waker_dir;

static void oplus_perf_nodes_exit(void);

static int oplus_perf_nodes_init(void)
{
	struct proc_dir_entry *pe;

	oplus_sched_dir = proc_mkdir(OPLUS_SCHEDULER_DIR, NULL);
	if (!oplus_sched_dir)
		goto err;

	oplus_sa_dir = proc_mkdir(OPLUS_SCHED_ASSIST_DIR, oplus_sched_dir);
	if (!oplus_sa_dir)
		goto err;

	pe = proc_create("sched_assist_scene", 0666, oplus_sa_dir,
			 &proc_sa_scene_fops);
	if (!pe)
		goto err;
	pe = proc_create("sched_impt_task", 0666, oplus_sa_dir,
			 &proc_sa_impt_fops);
	if (!pe)
		goto err;
	pe = proc_create("im_flag", 0666, oplus_sa_dir,
			 &proc_im_flag_fops);
	if (!pe)
		goto err;
	pe = proc_create("im_flag_app", 0666, oplus_sa_dir,
			 &proc_im_flag_app_fops);
	if (!pe)
		goto err;

	oplus_afs_dir = proc_mkdir(AFS_PROC_DIR, NULL);
	if (!oplus_afs_dir)
		goto err;
	pe = proc_create(AFS_PROC_CONFIG, 0666, oplus_afs_dir,
			 &afs_config_fops);
	if (!pe)
		goto err;
	pe = proc_create(AFS_PROC_ENABLE, 0666, oplus_afs_dir,
			 &afs_enable_fops);
	if (!pe)
		goto err;

	oplus_waker_dir = proc_mkdir(WAKER_IDENTIFY_DIR, NULL);
	if (!oplus_waker_dir)
		goto err;
	pe = proc_create("rt_info", 0666, oplus_waker_dir, &rt_info_fops);
	if (!pe)
		goto err;

	pe = proc_create("shell-temp", 0666, NULL, &proc_shell_temp_fops);
	if (!pe)
		goto err;
	pe = proc_create("task_cpustats", 0444, NULL, &task_cpustats_fops);
	if (!pe)
		goto err;

	return 0;

err:
	pr_err("failed to create oplus performance proc nodes\n");
	oplus_perf_nodes_exit();
	return -ENOMEM;
}

static void oplus_perf_nodes_exit(void)
{
	remove_proc_entry("task_cpustats", NULL);
	remove_proc_entry("shell-temp", NULL);

	if (oplus_waker_dir) {
		remove_proc_subtree(WAKER_IDENTIFY_DIR, NULL);
		oplus_waker_dir = NULL;
	}
	if (oplus_afs_dir) {
		remove_proc_subtree(AFS_PROC_DIR, NULL);
		oplus_afs_dir = NULL;
	}
	if (oplus_sched_dir) {
		remove_proc_subtree(OPLUS_SCHEDULER_DIR, NULL);
		oplus_sched_dir = NULL;
		oplus_sa_dir = NULL;
	}
}

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

	if (oplus_perf_nodes_init())
		pr_warn("performance/compat nodes not created\n");

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
	oplus_perf_nodes_exit();
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
MODULE_DESCRIPTION("Oplus/ColorOS proc nodes compat (binder, bootprof, sched_assist, afs_config, shell-temp, waker_identify)");
