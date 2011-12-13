#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>	/* alloc_chrdev_region */
#include <linux/sched.h>

#include <asm/uaccess.h>	/* copy_from_user, copy_to_user */
#include <linux/cdev.h>	/* cdev_init */
#include <linux/ioctl.h>	/* _IO* */
#include <linux/cpumask.h>	/* cpumask_weight() */

#include <linux/clist.h>	/* 循環リストライブラリ */

#define MODNAME "clist_benchmark"
static char *log_prefix = "module[clist_benchmark]";
#define MINOR_COUNT 1 // num of minor number

static dev_t dev_id;  /* デバイス番号 */
static struct cdev c_dev; /* キャラクタデバイス用構造体 */
static int end_flag = 0;

#define IO_MAGIC				'k'
#define IOC_USEREND_NOTIFY			_IO(IO_MAGIC, 0)	/* ユーザアプリ終了時 */
#define IOC_SIGRESET_REQUEST		_IO(IO_MAGIC, 1)	/* send_sig_argをリセット要求 */
#define IOC_SET_SIGNO			_IO(IO_MAGIC, 2)	/* シグナル番号を設定 */
#define IOC_SET_NR_NODE			_IO(IO_MAGIC, 3)	/* データの転送粒度を設定 */
#define IOC_SET_NODE_NR_COMPOSED		_IO(IO_MAGIC, 4)	/* データの転送粒度を設定 */
#define IOC_SET_PID				_IO(IO_MAGIC, 5)	/* PIDを設定 */

enum signal_status{
	PID_READY,
	SIGNO_READY,
	NR_NODE_READY,
	NODE_NR_COMPOSED_READY,
	SIG_READY,
	SIGRESET_REQUEST,
	SIGRESET_ACCEPTED,
	MAX_STATUS
};

struct lb_object{	/* やりとりするオブジェクト */
	pid_t pid;
	unsigned long seconds;
	int src_cpu, dst_cpu;
};

struct signal_spec{	/* ユーザ空間とシグナルで通信するための管理用構造体 */
	enum signal_status sr_status;
	int signo;
	struct siginfo info;
	struct task_struct *t;
};

static int nr_node, node_nr_composed;

static struct clist_controler *clist_ctl;
static struct signal_spec sigspec;
static struct timer_list flush_timer;

#define FLUSH_PERIOD	2000	/* この周期でタイマーが設定される */

extern int send_sig_info(int sig, struct siginfo *info, struct task_struct *p);

/* open(2) */
static int clbench_open(struct inode *inode, struct file *filp) 
{
	printk(KERN_INFO "%s : clist_benchmark open\n", log_prefix);
	return 0;
}

/* close(2) */
static int clbench_release(struct inode* inode, struct file* filp)
{
	printk(KERN_INFO "%s : clbench release\n", log_prefix);
	return 0;
}

/* read(2) */
static ssize_t clbench_read(struct file* filp, char* buf, size_t count, loff_t* offset)
{
	int actually_pulled, objects;
	void *temp_mem;

	objects = count / sizeof(struct lb_object);

	/* 中間メモリを確保 */
	temp_mem = (void *)kzalloc(count, GFP_KERNEL);

	if(sigspec.sr_status == SIG_READY){

		actually_pulled = clist_pull_order(temp_mem, objects, clist_ctl);

		/* このコードはタイマルーチン経由 */
		printk(KERN_INFO "%s : count = %d, actually_pulled:%d, wlen:%d\n", log_prefix, (int)count, actually_pulled, clist_wlen(clist_ctl));

		/* ユーザ空間にコピー */
		if(copy_to_user(buf, temp_mem, objs_to_byte(clist_ctl, actually_pulled))){
			printk(KERN_WARNING "%s : copy_to_user failed\n", log_prefix);
			return -EFAULT;
		}

	}
	else if(sigspec.sr_status == SIGRESET_REQUEST){

		if(end_flag){
			actually_pulled = clist_pull_end(temp_mem, objects, clist_ctl);
		}
		else{
			actually_pulled = clist_pull_order(temp_mem, objects, clist_ctl);
		}

		if(actually_pulled < 0){	/* エラー処理 */
			printk(KERN_INFO "%s : Error clist_pull failed\n", log_prefix);
			return actually_pulled;
		}

		/* ユーザ空間にコピー */
		if(copy_to_user(buf, temp_mem, objs_to_byte(clist_ctl, actually_pulled))){
			printk(KERN_WARNING "%s : copy_to_user failed\n", log_prefix);

			return -EFAULT;
		}

		if(end_flag){
			sigspec.sr_status = SIGRESET_ACCEPTED;
		}

		/* 条件が揃えば次の呼び出しはclist_pull_end() */
		if(end_flag == 0 && (count != actually_pulled || actually_pulled == 0)){
			printk(KERN_INFO "%s : now, call clist_pusll_end(), next\n", log_prefix);
			end_flag = 1;
		}

	}
	else{
		printk(KERN_WARNING "%s : invalid sigspec.sr_status\n", log_prefix);
		return 0;	/* error */
	}

	/* 中間メモリを解放 */
	kfree(temp_mem);

	*offset += objs_to_byte(clist_ctl, actually_pulled);

	return objs_to_byte(clist_ctl, actually_pulled);
}

/* ioctl(2) ※file_operations->unlocked_ioctl対応 */
static long clbench_ioctl(struct file *flip, unsigned int cmd, unsigned long arg)
{
	int retval = -1;
	struct pid *p;
	struct task_struct *t;

	switch(cmd){
		case IOC_USEREND_NOTIFY:	/* USEREND_NOTIFYがioctl(2)される前にユーザ側でsleep(PERIOD)してくれている */
			/* signal送信を止める処理 */
			if(sigspec.sr_status == SIG_READY){
				sigspec.sr_status = SIGRESET_REQUEST;
				printk(KERN_INFO "%s : IOC_USEREND_NOTIFY recieved\n", log_prefix);

				/* ユーザに通知してユーザにread(2)してもらう */
				put_user(clist_set_cold(clist_ctl, NULL, NULL), (unsigned int __user *)arg);
				retval = 1;
			}
			else{
				printk(KERN_INFO "%s : IOC_USEREND_NOTIFY was regarded\n", log_prefix);
				retval = -EPERM;
			}
			break;

		case IOC_SIGRESET_REQUEST:
			/* シグナルを止める処理 */
			if(sigspec.sr_status == SIG_READY){
				sigspec.sr_status = SIGRESET_REQUEST;
				printk(KERN_INFO "%s : IOC_SIGRESET_REQUES recieved\n", log_prefix);
			}
			else{
				printk(KERN_INFO "%s : IOC_SIGRESET_REQUEST was regarded\n", log_prefix);
				retval = -EPERM;
			}
			break;

		case IOC_SET_SIGNO:
			/* シグナル番号の設定 */
			printk(KERN_INFO "%s : IOC_SET_SIGNO accepted\n", log_prefix);
			sigspec.signo = arg;
			sigspec.info.si_signo = arg;

			sigspec.sr_status = SIGNO_READY;

			retval = 1;
			break;

		case IOC_SET_NR_NODE:
			/* 循環リストのノード数の設定 */
			printk(KERN_INFO "%s : IOC_SET_NR_NODE accepted arg = %lu\n", log_prefix, arg);

			sigspec.sr_status = NR_NODE_READY;
			nr_node = (int)arg;

			retval = 1;
			break;

		case IOC_SET_NODE_NR_COMPOSED:
			/* 循環リストのノードのオブジェクト数の設定 */
			printk(KERN_INFO "%s : IOC_SET_NODE_NR_COMPOSED accepted arg = %lu\n", log_prefix, arg);

			sigspec.sr_status = NODE_NR_COMPOSED_READY;
			node_nr_composed = (int)arg;

			retval = 1;
			break;

		case IOC_SET_PID:
			printk(KERN_INFO "%s : IOC_SET_PID accepted\n", log_prefix);
			p = find_vpid(arg);	/* get struct pid* from arg */
			t = pid_task(p, PIDTYPE_PID);	/* get struct task_struct* from p */
			sigspec.t = t;
			sigspec.info.si_errno = 0;
			sigspec.info.si_code = SI_KERNEL;
			sigspec.info.si_pid = 0;
			sigspec.info.si_uid = 0;

			sigspec.sr_status = SIG_READY;

			retval = 1;
			break;
	}

	if(sigspec.sr_status == SIG_READY){	/* clbench_add_object()を実行できる状態にする */
		clist_ctl = clist_alloc(nr_node, node_nr_composed, sizeof(struct lb_object));
		mod_timer(&flush_timer, jiffies + msecs_to_jiffies(FLUSH_PERIOD));
	}

	return retval;
}

/*
	システムコールを担当する関数を登録する
*/
static struct file_operations clbench_fops = {
	.owner   = THIS_MODULE,
	.open    = clbench_open,
	.release = clbench_release,
	.read    = clbench_read,
	.write   = NULL,
	.unlocked_ioctl   = clbench_ioctl,	/* kernel 2.6.36以降はunlocked_ioctl */
};

/*
	シグナルを送ってユーザプログラムにread(2)させる関数
	@__data タイマのコールバック関数で必要
*/
static void clbench_flush(unsigned long __data)
{
	if(sigspec.sr_status == SIG_READY){	/* SIG_READYである間はタイマは生きている */

		if(clist_wlen(clist_ctl) > 0){	/* read待ちのノードが1つ以上あれば */

			/* カーネルからユーザスレッドにシグナルを送る */
			send_sig_info(sigspec.signo, &sigspec.info, sigspec.t);
		}

		/* 次のタイマをセット */
		mod_timer(&flush_timer, jiffies + msecs_to_jiffies(FLUSH_PERIOD));
	}
}

/*
	balance_tasks()@sched.cで呼び出される関数 ロードバランスが行われている箇所で呼び出される
	@p ロードバランスされたtask_structのアドレス
	@src_cpu 最も忙しいCPU番号
	@this_cpu ロードバランス先のCPU番号
*/
void clbench_add_object(struct task_struct *p, int src_cpu, int this_cpu)
{
	struct lb_object lb;

	if(sigspec.sr_status != SIG_READY){	/* シグナルを送信できる状態かどうか */
		return;
	}

	lb.pid = p->pid;
	lb.seconds = get_seconds();
	lb.src_cpu = src_cpu;
	lb.dst_cpu = this_cpu;

	clist_push_one((void *)&lb, clist_ctl);
}
EXPORT_SYMBOL(clbench_add_object);


/*
	insmod時に呼び出される関数
*/
static int __init clbench_init(void)
{
	int ret;

	// キャラクタデバイス番号の動的取得
	ret = alloc_chrdev_region(&dev_id, 0, MINOR_COUNT, MODNAME);

	if(ret < 0){
		printk(KERN_WARNING "%s : alloc_chrdev_region failed\n", log_prefix);
		return ret;
	}

	cdev_init(&c_dev, &clbench_fops);
	c_dev.owner = THIS_MODULE;

	ret = cdev_add(&c_dev, dev_id, MINOR_COUNT);

	if(ret < 0){
		printk(KERN_WARNING "%s : cdev_add failed\n", log_prefix);
		return ret;
	}

	setup_timer(&flush_timer, clbench_flush, 0);

	sigspec.sr_status = MAX_STATUS;

	printk(KERN_INFO "%s : clbench is loaded\n", log_prefix);
	printk(KERN_INFO "%s : clbench %d %d\n", log_prefix, IOC_SET_SIGNO, IOC_SET_PID);

	return 0;
}

/*
	rmmod時に呼び出される関数 組み込みモジュールなら呼び出されない
*/
static void __exit clbench_exit(void)
{
	cdev_del(&c_dev);	/* デバイスの削除 */

	del_timer_sync(&flush_timer);	/* タイマの終了 */

	/* 循環リストを解放 */
	clist_free(clist_ctl);

	unregister_chrdev_region(dev_id, MINOR_COUNT);	/* メジャー番号の解放 */
	printk(KERN_INFO "%s : clbench is removed\n", log_prefix);
}

module_init(clbench_init);
module_exit(clbench_exit);

MODULE_DESCRIPTION("Load-Balance profiler");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("K.Shimada");
