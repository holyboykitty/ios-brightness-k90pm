// SPDX-License-Identifier: GPL-2.0
/*
 * ios_brightness.c — iOS-Style Brightness Control
 * Device:  Redmi K90 Pro Max (myron/canoe, Snapdragon 8 Elite)
 * Panel:   CSOT NT37801 2K LTPO AMOLED, 14-bit PWM (max=16383)
 * Kernel:  6.12 (GKI, SukiSU Ultra)
 * ALS:     Goodix GLSX via SSC (userspace bridge required)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>

/* ─── Module Parameters ─── */

static char *bl_sysfs_path = "/sys/class/backlight/panel0-backlight/brightness";
module_param(bl_sysfs_path, charp, 0644);
MODULE_PARM_DESC(bl_sysfs_path, "Backlight brightness sysfs path");

static int def_max_raw = 16383;
module_param(def_max_raw, int, 0644);
MODULE_PARM_DESC(def_max_raw, "Max raw brightness (K90PM=16383)");

static int def_min_raw = 1;
module_param(def_min_raw, int, 0644);
MODULE_PARM_DESC(def_min_raw, "Min raw brightness");

static int def_gamma_x100 = 250;
module_param(def_gamma_x100, int, 0644);
MODULE_PARM_DESC(def_gamma_x100, "Gamma * 100 (250=2.5)");

static int def_transition_ms = 300;
module_param(def_transition_ms, int, 0644);
MODULE_PARM_DESC(def_transition_ms, "Transition ms");

/* ─── Constants ─── */

#define PROC_DIR       "ios_brightness"
#define SLIDER_MAX     1000
#define SMOOTH_SAMPLES 5
#define HYSTERESIS     8
#define BL_BUF_SZ      16

/* ─── Mode ─── */

enum ios_mode { MODE_MANUAL = 0, MODE_AUTO = 1, MODE_OVERLAY = 2 };
static const char *mode_str[] = { "manual", "auto", "overlay" };

/* ─── Lux -> Slider Table (iOS-style) ─── */

struct lux_map { int lux; int slider; };

static const struct lux_map lux_table[] = {
	{     0,   15 }, {     5,   40 }, {    20,   90 },
	{    50,  160 }, {   100,  240 }, {   200,  320 },
	{   500,  460 }, {  1000,  580 }, {  3000,  730 },
	{  5000,  810 }, { 10000,  890 }, { 30000,  950 },
	{ 50000, 1000 },
};
#define LUX_TABLE_SZ ARRAY_SIZE(lux_table)

/* ─── Context ─── */

struct ios_ctx {
	bool        enabled;
	enum ios_mode mode;
	bool        display_on;

	unsigned long cur_raw;
	unsigned long manual_slider, auto_slider;
	unsigned long max_raw, min_raw;
	int           gamma_x100;

	/* transition animation */
	struct task_struct *thr;
	bool          thr_run;
	unsigned long t_from, t_to;
	ktime_t       t_start;
	unsigned int  t_ms;

	/* auto brightness */
	struct delayed_work als_work;
	int  poll_ms;
	int  lux;
	int  lux_buf[SMOOTH_SAMPLES];
	int  lux_idx;
	int  prev_auto;

	/* userspace lux feed */
	int  lux_feed;
	bool lux_feed_valid;

	/* fb notifier */
	struct notifier_block fb_nb;

	/* procfs */
	struct proc_dir_entry *proc;

	/* locks */
	struct mutex mtx;
	spinlock_t   spn;
};

static struct ios_ctx C;

/* ═══════════════════════════════════════════════
 *  Sysfs backlight write (no backlight API dependency)
 * ═══════════════════════════════════════════════ */

static void hw_set(unsigned long raw)
{
	struct file *f;
	char buf[BL_BUF_SZ];
	int len;
	loff_t pos = 0;
	ssize_t ret;

	raw = clamp_val(raw, C.min_raw, C.max_raw);
	C.cur_raw = raw;

	len = snprintf(buf, sizeof(buf), "%lu\n", raw);

	f = filp_open(bl_sysfs_path, O_WRONLY, 0);
	if (IS_ERR(f)) {
		pr_warn("ios_brightness: cannot open %s (%ld)\n",
			bl_sysfs_path, PTR_ERR(f));
		return;
	}
	ret = kernel_write(f, buf, len, &pos);
	if (ret < 0)
		pr_warn("ios_brightness: write failed (%zd)\n", ret);
	filp_close(f, NULL);
}

/* ═══════════════════════════════════════════════
 *  Integer Gamma Math (no float in kernel)
 * ═══════════════════════════════════════════════ */

static unsigned long ipow(unsigned long x, int gx)
{
	u64 x64, r, next, gi, gf;
	int i;

	if (x == 0)    return 0;
	if (x >= 1000) return 1000;
	if (gx == 100) return x;

	x64 = (u64)x;

	if (gx == 250) {
		r = x64 * x64 * int_sqrt(x64 * 1000) / 1000000ULL;
		return (unsigned long)min_t(u64, r, 1000);
	}
	if (gx == 200) {
		r = x64 * x64 / 1000ULL;
		return (unsigned long)min_t(u64, r, 1000);
	}
	if (gx == 300) {
		r = x64 * x64 * x64 / 1000000ULL;
		return (unsigned long)min_t(u64, r, 1000);
	}

	gi = gx / 100;
	gf = gx % 100;
	r = 1000;
	for (i = 0; i < (int)gi; i++)
		r = r * x64 / 1000ULL;
	if (gf > 0) {
		next = r * x64 / 1000ULL;
		r = r + (next - r) * gf / 100ULL;
	}
	return (unsigned long)min_t(u64, r, 1000);
}

/* ═══════════════════════════════════════════════
 *  Brightness Curve Engine
 * ═══════════════════════════════════════════════ */

static unsigned long slider_to_raw(unsigned long s)
{
	unsigned long range, pct;

	if (s >= SLIDER_MAX) return C.max_raw;
	if (s == 0)          return C.min_raw;

	range = C.max_raw - C.min_raw;
	pct   = ipow(s, C.gamma_x100);
	return clamp_val(C.min_raw + range * pct / SLIDER_MAX,
			 C.min_raw, C.max_raw);
}

static unsigned long raw_to_slider(unsigned long r)
{
	unsigned long lo = 0, hi = SLIDER_MAX, mid, v, vlo, vhi;

	if (r <= C.min_raw) return 0;
	if (r >= C.max_raw) return SLIDER_MAX;

	while (hi - lo > 1) {
		mid = (lo + hi) / 2;
		v = slider_to_raw(mid);
		if (v < r) lo = mid; else hi = mid;
	}
	vlo = slider_to_raw(lo);
	vhi = slider_to_raw(hi);
	if (vhi == vlo) return lo;
	return lo + (r - vlo) * (hi - lo) / (vhi - vlo);
}

/* ═══════════════════════════════════════════════
 *  Auto Brightness Engine
 * ═══════════════════════════════════════════════ */

static int lux_to_slider(int lux)
{
	int i;
	if (lux <= 0) return lux_table[0].slider;
	if (lux >= lux_table[LUX_TABLE_SZ - 1].lux)
		return lux_table[LUX_TABLE_SZ - 1].slider;

	for (i = 1; i < (int)LUX_TABLE_SZ; i++) {
		if (lux <= lux_table[i].lux) {
			int l0 = lux_table[i-1].lux, l1 = lux_table[i].lux;
			int s0 = lux_table[i-1].slider, s1 = lux_table[i].slider;
			if (l1 == l0) return s0;
			return s0 + (s1 - s0) * (lux - l0) / (l1 - l0);
		}
	}
	return SLIDER_MAX;
}

static int smooth_lux(int raw)
{
	int i, sum;
	C.lux_buf[C.lux_idx] = raw;
	C.lux_idx = (C.lux_idx + 1) % SMOOTH_SAMPLES;
	for (i = 0, sum = 0; i < SMOOTH_SAMPLES; i++)
		sum += C.lux_buf[i];
	return sum / SMOOTH_SAMPLES;
}

static void als_work_fn(struct work_struct *w)
{
	int lux, sl, new_raw;

	if (!C.enabled || C.mode != MODE_AUTO || !C.display_on)
		goto resched;

	if (!C.lux_feed_valid)
		goto resched;

	lux = C.lux_feed;
	C.lux = lux;
	lux = smooth_lux(lux);
	sl = lux_to_slider(lux);

	if (abs(sl - C.prev_auto) < HYSTERESIS)
		goto resched;

	C.prev_auto   = sl;
	C.auto_slider = sl;

	mutex_lock(&C.mtx);
	new_raw = slider_to_raw(sl);
	if (new_raw != C.cur_raw) {
		C.t_from  = C.cur_raw;
		C.t_to    = clamp_val(new_raw, C.min_raw, C.max_raw);
		C.t_start = ktime_get();
		spin_lock(&C.spn);
		C.thr_run = true;
		spin_unlock(&C.spn);
	}
	mutex_unlock(&C.mtx);

resched:
	if (C.enabled && C.mode == MODE_AUTO)
		schedule_delayed_work(&C.als_work,
				      msecs_to_jiffies(C.poll_ms));
}

/* ═══════════════════════════════════════════════
 *  60fps Smooth Transition (cubic ease-in-out)
 * ═══════════════════════════════════════════════ */

static unsigned long ease_cubic(unsigned long t)
{
	u64 tt, u, r;
	if (t == 0)       return 0;
	if (t >= 1000000) return 1000000;
	tt = (u64)t;
	if (tt < 500000) {
		r = 4ULL * tt * tt * tt / 1000000000000ULL;
	} else {
		u = 2000000ULL - 2ULL * tt;
		u = u * u * u / 1000000000000ULL;
		r = 1000000ULL - u / 2ULL;
	}
	return (unsigned long)clamp_val(r, 0, 1000000);
}

static int trans_fn(void *data)
{
	unsigned long el, pr, ez, rng, raw;

	while (!kthread_should_stop()) {
		if (!C.thr_run || !C.display_on) {
			msleep(20);
			continue;
		}
		el = (unsigned long)ktime_ms_delta(ktime_get(), C.t_start);
		if (el >= C.t_ms) {
			spin_lock(&C.spn);
			C.thr_run = false;
			spin_unlock(&C.spn);
			hw_set(C.t_to);
			msleep(5);
			continue;
		}
		pr = el * 1000000UL / C.t_ms;
		ez = ease_cubic(pr);
		if (C.t_to >= C.t_from) {
			rng = C.t_to - C.t_from;
			raw = C.t_from + rng * ez / 1000000UL;
		} else {
			rng = C.t_from - C.t_to;
			raw = C.t_from - rng * ez / 1000000UL;
		}
		hw_set(raw);
		msleep(16);
	}
	return 0;
}

static void start_trans(unsigned long target)
{
	target = clamp_val(target, C.min_raw, C.max_raw);
	if (target == C.cur_raw) { C.thr_run = false; return; }
	C.t_from  = C.cur_raw;
	C.t_to    = target;
	C.t_start = ktime_get();
	C.t_ms    = C.t_ms > 0 ? C.t_ms : def_transition_ms;
	spin_lock(&C.spn);
	C.thr_run = true;
	spin_unlock(&C.spn);
}

/* ═══════════════════════════════════════════════
 *  FB Notifier: Screen on/off
 * ═══════════════════════════════════════════════ */

static int fb_cb(struct notifier_block *nb, unsigned long act, void *d)
{
	struct fb_event *ev = d;
	int *blank;
	if (act != FB_EVENT_BLANK || !ev || !ev->data) return NOTIFY_OK;
	blank = (int *)ev->data;
	mutex_lock(&C.mtx);
	if (*blank == FB_BLANK_UNBLANK) {
		C.display_on = true;
		if (C.enabled) {
			unsigned long s = (C.mode == MODE_AUTO) ?
				C.auto_slider : C.manual_slider;
			start_trans(slider_to_raw(s));
		}
	} else {
		C.display_on = false;
		C.thr_run = false;
	}
	mutex_unlock(&C.mtx);
	return NOTIFY_OK;
}

/* ═══════════════════════════════════════════════
 *  Procfs Interface
 * ═══════════════════════════════════════════════ */

static int p_en_show(struct seq_file *m, void *v)
{ seq_printf(m, "%d\n", C.enabled ? 1 : 0); return 0; }

static ssize_t p_en_write(struct file *f, const char __user *b,
			   size_t c, loff_t *p)
{
	char k[8]; int v;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	if (kstrtoint(k, 0, &v)) return -EINVAL;
	mutex_lock(&C.mtx);
	C.enabled = !!v;
	if (C.enabled) {
		unsigned long s = (C.mode == MODE_AUTO) ?
			C.auto_slider : C.manual_slider;
		start_trans(slider_to_raw(s));
		if (C.mode == MODE_AUTO)
			schedule_delayed_work(&C.als_work,
					      msecs_to_jiffies(50));
	} else {
		cancel_delayed_work_sync(&C.als_work);
		C.thr_run = false;
	}
	mutex_unlock(&C.mtx);
	pr_info("ios_brightness: %s\n", v ? "enabled" : "disabled");
	return c;
}

static int p_mode_show(struct seq_file *m, void *v)
{ seq_printf(m, "%s\n", mode_str[C.mode]); return 0; }

static ssize_t p_mode_write(struct file *f, const char __user *b,
			     size_t c, loff_t *p)
{
	char k[16]; size_t l;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	l = strcspn(k, "\n\r "); k[l] = '\0';
	mutex_lock(&C.mtx);
	if (!strcmp(k, "auto")) {
		C.mode = MODE_AUTO;
		if (C.enabled)
			schedule_delayed_work(&C.als_work,
					      msecs_to_jiffies(50));
	} else if (!strcmp(k, "manual")) {
		C.mode = MODE_MANUAL;
		cancel_delayed_work_sync(&C.als_work);
	} else if (!strcmp(k, "overlay")) {
		C.mode = MODE_OVERLAY;
		cancel_delayed_work_sync(&C.als_work);
	} else {
		mutex_unlock(&C.mtx);
		return -EINVAL;
	}
	if (C.enabled) {
		unsigned long s = (C.mode == MODE_AUTO) ?
			C.auto_slider : C.manual_slider;
		start_trans(slider_to_raw(s));
	}
	mutex_unlock(&C.mtx);
	pr_info("ios_brightness: mode -> %s\n", k);
	return c;
}

static ssize_t p_set_write(struct file *f, const char __user *b,
			    size_t c, loff_t *p)
{
	char k[16]; unsigned long s;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	if (kstrtoul(k, 0, &s)) return -EINVAL;
	if (s > SLIDER_MAX) s = SLIDER_MAX;
	mutex_lock(&C.mtx);
	C.manual_slider = s;
	if (C.enabled && C.mode == MODE_MANUAL)
		start_trans(slider_to_raw(s));
	mutex_unlock(&C.mtx);
	return c;
}

static ssize_t p_lux_write(struct file *f, const char __user *b,
			    size_t c, loff_t *p)
{
	char k[16]; int lux;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	if (kstrtoint(k, 0, &lux)) return -EINVAL;
	mutex_lock(&C.mtx);
	C.lux_feed = lux;
	C.lux_feed_valid = true;
	mutex_unlock(&C.mtx);
	return c;
}

static int p_bri_show(struct seq_file *m, void *v)
{
	unsigned long s = raw_to_slider(C.cur_raw);
	seq_printf(m, "%lu (%lu.%lu%%) raw=%lu\n",
		   s, s / 10, s % 10, C.cur_raw);
	return 0;
}

static int p_luxro_show(struct seq_file *m, void *v)
{ seq_printf(m, "%d\n", C.lux); return 0; }

static int p_gam_show(struct seq_file *m, void *v)
{ seq_printf(m, "%d.%02d\n", C.gamma_x100 / 100, C.gamma_x100 % 100); return 0; }

static ssize_t p_gam_write(struct file *f, const char __user *b,
			    size_t c, loff_t *p)
{
	char k[12]; int v;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	if (kstrtoint(k, 0, &v)) return -EINVAL;
	if (v < 100 || v > 500) return -EINVAL;
	mutex_lock(&C.mtx);
	C.gamma_x100 = v;
	if (C.enabled) {
		unsigned long s = (C.mode == MODE_AUTO) ?
			C.auto_slider : C.manual_slider;
		start_trans(slider_to_raw(s));
	}
	mutex_unlock(&C.mtx);
	pr_info("ios_brightness: gamma -> %d.%02d\n", v / 100, v % 100);
	return c;
}

static int p_tr_show(struct seq_file *m, void *v)
{ seq_printf(m, "%u\n", C.t_ms); return 0; }

static ssize_t p_tr_write(struct file *f, const char __user *b,
			   size_t c, loff_t *p)
{
	char k[12]; unsigned int v;
	if (c >= sizeof(k)) return -EINVAL;
	if (copy_from_user(k, b, c)) return -EFAULT;
	k[c] = '\0';
	if (kstrtouint(k, 0, &v)) return -EINVAL;
	if (v > 2000) v = 2000;
	C.t_ms = v;
	return c;
}

static int p_sta_show(struct seq_file *m, void *v)
{
	unsigned long s = raw_to_slider(C.cur_raw);
	seq_printf(m,
		"=== iOS Brightness (K90 Pro Max) ===\n"
		"Enabled:     %s\n"
		"Mode:        %s\n"
		"Display:     %s\n"
		"Current:     %lu (%lu.%lu%%) raw=%lu\n"
		"Manual:      %lu (%lu.%lu%%)\n"
		"Auto:        %lu (%lu.%lu%%)\n"
		"Gamma:       %d.%02d\n"
		"Min/Max:     %lu / %lu\n"
		"Transition:  %u ms %s\n"
		"Lux:         %d (feed=%s)\n"
		"BL path:     %s\n",
		C.enabled ? "yes" : "no",
		mode_str[C.mode],
		C.display_on ? "on" : "off",
		s, s / 10, s % 10, C.cur_raw,
		C.manual_slider, C.manual_slider / 10, C.manual_slider % 10,
		C.auto_slider, C.auto_slider / 10, C.auto_slider % 10,
		C.gamma_x100 / 100, C.gamma_x100 % 100,
		C.min_raw, C.max_raw,
		C.t_ms, C.thr_run ? "ACTIVE" : "idle",
		C.lux, C.lux_feed_valid ? "on" : "off",
		bl_sysfs_path
	);
	return 0;
}

/* --- Procfs boilerplate --- */

#define PFS(N) \
	static int p_##N##_open(struct inode *i, struct file *f) \
	{ return single_open(f, p_##N##_show, NULL); } \
	static const struct proc_ops p_##N##_ops = { \
		.proc_open = p_##N##_open, .proc_read = seq_read, \
		.proc_lseek = seq_lseek, .proc_release = single_release }

#define PFRW(N) \
	static int p_##N##_open(struct inode *i, struct file *f) \
	{ return single_open(f, p_##N##_show, NULL); } \
	static const struct proc_ops p_##N##_ops = { \
		.proc_open = p_##N##_open, .proc_read = seq_read, \
		.proc_write = p_##N##_write, \
		.proc_lseek = seq_lseek, .proc_release = single_release }

#define PFW(N) \
	static const struct proc_ops p_##N##_ops = { \
		.proc_write = p_##N##_write }

PFRW(en);
PFRW(mode);
PFW(set);
PFW(lux);
PFS(bri);
PFS(luxro);
PFRW(gam);
PFRW(tr);
PFS(sta);

static int create_proc(void)
{
	struct proc_dir_entry *d = proc_mkdir(PROC_DIR, NULL);
	if (!d) return -ENOMEM;
	C.proc = d;
#define CP(N,M) proc_create(#N, M, d, &p_##N##_ops)
	CP(en,    0644);
	CP(mode,  0644);
	CP(set,   0200);
	CP(lux,   0200);
	CP(bri,   0444);
	CP(luxro, 0444);
	CP(gam,   0644);
	CP(tr,    0644);
	CP(sta,   0444);
#undef CP
	return 0;
}

static void destroy_proc(void)
{
	if (C.proc) { remove_proc_subtree(PROC_DIR, NULL); C.proc = NULL; }
}

/* ═══════════════════════════════════════════════
 *  Init & Exit
 * ═══════════════════════════════════════════════ */

static int __init ios_brightness_init(void)
{
	int ret;
	struct file *f;

	memset(&C, 0, sizeof(C));
	mutex_init(&C.mtx);
	spin_lock_init(&C.spn);

	C.mode          = MODE_MANUAL;
	C.display_on    = true;
	C.gamma_x100    = def_gamma_x100;
	C.min_raw       = def_min_raw;
	C.max_raw       = def_max_raw;
	C.manual_slider = 500;
	C.auto_slider   = 250;
	C.t_ms          = def_transition_ms;
	C.poll_ms       = 200;
	C.prev_auto     = -1;

	/* 验证背光 sysfs 路径存在 */
	f = filp_open(bl_sysfs_path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("ios_brightness: '%s' not found (%ld)\n",
		       bl_sysfs_path, PTR_ERR(f));
		return -ENODEV;
	}
	filp_close(f, NULL);
	pr_info("ios_brightness: backlight path ok: %s\n", bl_sysfs_path);

	INIT_DELAYED_WORK(&C.als_work, als_work_fn);

	C.thr = kthread_run(trans_fn, NULL, "ios_br");
	if (IS_ERR(C.thr)) {
		ret = PTR_ERR(C.thr);
		goto err;
	}

	C.fb_nb.notifier_call = fb_cb;
	fb_register_client(&C.fb_nb);

	ret = create_proc();
	if (ret < 0) goto err;

	pr_info("ios_brightness: ready -> echo 1 > /proc/%s/enabled\n",
		PROC_DIR);
	return 0;

err:
	if (!IS_ERR_OR_NULL(C.thr)) kthread_stop(C.thr);
	cancel_delayed_work_sync(&C.als_work);
	return ret;
}

static void __exit ios_brightness_exit(void)
{
	C.enabled = false;
	cancel_delayed_work_sync(&C.als_work);
	C.thr_run = false;
	if (!IS_ERR_OR_NULL(C.thr)) kthread_stop(C.thr);
	fb_unregister_client(&C.fb_nb);
	destroy_proc();
	pr_info("ios_brightness: unloaded\n");
}

module_init(ios_brightness_init);
module_exit(ios_brightness_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MiMo");
MODULE_DESCRIPTION("iOS-style brightness for K90 Pro Max on SukiSU Ultra");
MODULE_VERSION("2.0.0");
