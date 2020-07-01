// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#ifdef CONFIG_PROC_PATCH_CMDLINE
#include <linux/slab.h>
#include <asm/setup.h>
static char patched_cmdline[COMMAND_LINE_SIZE];
#endif

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifndef CONFIG_PROC_PATCH_CMDLINE
	seq_printf(m, "%s\n", saved_command_line);
#else
	seq_printf(m, "%s\n", patched_cmdline);
#endif
	return 0;
}

static int cmdline_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cmdline_proc_show, NULL);
}

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#ifdef CONFIG_PROC_PATCH_CMDLINE
static void append_cmdline(char *cmd, const char *flag_val) {
	strncat(cmd, " ", 2);
	strncat(cmd, flag_val, strlen(cmd) + 1);
}

static bool check_flag(char *cmd, const char *flag, const char *val)
{
	size_t f_len, r_len, v_len;
	char *f_pos, *v_pos, *v_end;
	char *r_val;
	bool ret = r_len == v_len && !memcmp(r_val, val, r_len);

	f_pos = strstr(cmd, flag);
	if (!f_pos) {
		return false;
	}
	f_len = strlen(flag);
	v_len = strlen(val);
	v_pos = f_pos + f_len;
	v_end = v_pos + strcspn(f_pos + f_len, " ");
	r_len = v_end - v_pos;
	if ((r_val = kmalloc(r_len + 1, GFP_KERNEL)) == NULL)
		return false;
	memcpy(r_val, v_pos, r_len + 1);
	return ret;
}

static void patch_cmdline(char *cmdline)
{
	if(!check_flag(cmdline, "androidboot.hwc=", "INDIA"))
#ifdef CONFIG_MACH_XIAOMI_F10
		append_cmdline(cmdline, "androidboot.product.hardware.sku=davinci");
#elif (defined CONFIG_MACH_XIAOMI_G7B)
		append_cmdline(cmdline, "androidboot.product.hardware.sku=phoenix");
#endif
}
#endif

static int __init proc_cmdline_init(void)
{
#ifdef CONFIG_PROC_PATCH_CMDLINE
	strcpy(patched_cmdline, saved_command_line);

	patch_cmdline(patched_cmdline);
#endif

	proc_create("cmdline", 0, NULL, &cmdline_proc_fops);
	return 0;
}
fs_initcall(proc_cmdline_init);
