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


/*
+	注意！	ユーザ空間とやりとりするオブジェクトはパッディングが発生しない構造にすること
		この構造体のメンバのsizeof()の合計がsizeof(struct object)と一致するようにすること

		取得したいイベントにあわせてこの構造体とclbench_add_object()を作成する
*/
struct object{
	pid_t pid, padding;
	int src_cpu, dst_cpu;
	long sec, usec;
};


#define MODNAME "clist_benchmark"
static char *log_prefix = "module[clist_benchmark]";
#define MINOR_COUNT 1 // num of minor number

static dev_t dev_id;  /* デバイス番号 */
static struct cdev c_dev; /* キャラクタデバイス用構造体 */

#define IO_MAGIC				'k'
#define IOC_USEREND_NOTIFY			_IO(IO_MAGIC, 0)		/* ユーザアプリ終了時 */
#define IOC_SIGRESET_REQUEST		_IO(IO_MAGIC, 1)		/* signal_spec構造体のリセット要求 */
#define IOC_SUBMIT_SPEC			_IOW(IO_MAGIC, 2, void)	/* ユーザからのパラメータ設定 */

enum signal_status{
	SIG_READY,
	SIGRESET_REQUEST,
	SIGRESET_ACCEPTED,
	MAX_STATUS
};

/* ユーザからデバイス初期化時に送られるデータ構造 */
struct ioc_submit_spec{
	int pid;
	int signo, flush_period;
	int nr_node, node_nr_composed;
	int dummy;
};

struct signal_spec{	/* ユーザ空間とシグナルで通信するための管理用構造体 */
	enum signal_status sr_status;
	int signo, flush_period;
	struct siginfo info;
	struct task_struct *t;
	struct timer_list flush_timer;
};

static struct clist_controller *clist_ctl;
static struct signal_spec sigspec;

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
	if(sigspec.sr_status != SIGRESET_ACCEPTED){
		printk(KERN_INFO "%s : Warning sr_status isn't SIGRESET_ACCEPTED\n", log_prefix);
	}

	/* 循環リストを解放 */
	clist_free(clist_ctl);

	printk(KERN_INFO "%s : clbench release\n", log_prefix);
	return 0;
}

/* read(2) */
static ssize_t clbench_read(struct file* filp, char* buf, size_t count, loff_t* offset)
{
	int actually_pulled, objects, ret;
	void *temp_mem;

	if(sigspec.sr_status == SIG_READY || sigspec.sr_status == SIGRESET_REQUEST){

		objects = count / sizeof(struct object);

		/* 中間メモリを確保 */
		temp_mem = (void *)kzalloc(count, GFP_KERNEL);

		actually_pulled = clist_pull_order(temp_mem, objects, clist_ctl);

		if(actually_pulled == 0 && CLIST_IS_END(clist_ctl)){	/* ここは1回しか通らないはず */
			printk(KERN_INFO "%s : now, clist_pull_end() is calling\n", log_prefix);

			/* もし1つも読めなくて、かつ循環リストがCOLDなら書き込み中のノードから読む */
			actually_pulled = clist_pull_end(temp_mem, clist_ctl);
			sigspec.sr_status = SIGRESET_ACCEPTED;
		}

		/* pullしたバイト数を計算 */
		ret = objs_to_byte(clist_ctl, actually_pulled);

		/* ユーザ空間にコピー */
		if(copy_to_user(buf, temp_mem, ret)){
			printk(KERN_WARNING "%s : copy_to_user failed\n", log_prefix);
			return -EFAULT;
		}

		printk(KERN_INFO "%s : count = %d, actually_pulled:%d, wlen:%d\n", log_prefix, (int)count, actually_pulled, clist_wlen(clist_ctl));

		/* 中間メモリを解放 */
		kfree(temp_mem);

		*offset += ret;
	}
	else if(sigspec.sr_status == SIGRESET_ACCEPTED){
		ret = 0;
	}
	else{	/* エラー */
		ret = -ECANCELED;
	}

	return ret;
}

/* ioctl(2) ※file_operations->unlocked_ioctl対応 */
static long clbench_ioctl(struct file *flip, unsigned int cmd, unsigned long arg)
{
	int retval = -1;
	struct pid *p;
	struct task_struct *t;
	struct ioc_submit_spec submit_spec;

	switch(cmd){
		case IOC_USEREND_NOTIFY:	/* USEREND_NOTIFYがioctl(2)される前にユーザ側でsleep(PERIOD)してくれている */
			/* signal送信を止める処理 */
			if(sigspec.sr_status == SIG_READY){
				int nr_objs, nr_first, nr_burst;

				sigspec.sr_status = SIGRESET_REQUEST;
				printk(KERN_INFO "%s : IOC_USEREND_NOTIFY recieved\n", log_prefix);

				/* ユーザに通知してユーザにread(2)してもらう */

				nr_objs = clist_set_end(clist_ctl, &nr_first, &nr_burst);

				nr_objs += nr_first + (nr_burst * clist_ctl->nr_composed);

				put_user(nr_objs, (unsigned int __user *)arg);
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
				retval = 1;
			}
			else{
				printk(KERN_INFO "%s : IOC_SIGRESET_REQUEST was regarded\n", log_prefix);
				retval = -EPERM;
			}
			break;

		case IOC_SUBMIT_SPEC:

			copy_from_user(&submit_spec, (struct ioc_submit_spec __user *)arg, sizeof(struct ioc_submit_spec));

			printk(KERN_INFO "%s : IOC_SET_SPEC pid:%d, flush_period:%d signo:%d nr_node:%d node_nr_cmposed:%d\n",
				log_prefix, submit_spec.pid, submit_spec.flush_period, submit_spec.signo, submit_spec.nr_node, submit_spec.node_nr_composed);

			/* pidの準備 */
			p = find_vpid(submit_spec.pid);
			t = pid_task(p, PIDTYPE_PID);
			sigspec.t = t;
			sigspec.info.si_errno = 0;
			sigspec.info.si_code = SI_KERNEL;
			sigspec.info.si_pid = 0;
			sigspec.info.si_uid = 0;

			/* signoの準備 */
			sigspec.signo = submit_spec.signo;
			sigspec.info.si_signo = submit_spec.signo;

			/* flush_periodの準備 */
			sigspec.flush_period = submit_spec.flush_period;


			/* 準備完了 */
			sigspec.sr_status = SIG_READY;

			printk(KERN_INFO "%s : signal ready, object-size is %ld byte\n", log_prefix, sizeof(struct object));

			clist_ctl = clist_alloc(submit_spec.nr_node, submit_spec.node_nr_composed, sizeof(struct object));

			if(clist_ctl == NULL){
				printk(KERN_INFO "%s : clist_alloc() failed returned NULL\n");
				retval = -ENOMEM;
			}
			else{
				mod_timer(&sigspec.flush_timer, jiffies + msecs_to_jiffies(sigspec.flush_period));
				printk(KERN_INFO "%s : device setup complete\n", log_prefix);
				retval = 1;
			}

			break;
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
		mod_timer(&sigspec.flush_timer, jiffies + msecs_to_jiffies(sigspec.flush_period));
	}
}


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

	setup_timer(&sigspec.flush_timer, clbench_flush, 0);

	sigspec.sr_status = MAX_STATUS;

	return 0;
}

/*
	rmmod時に呼び出される関数 組み込みモジュールなら呼び出されない
*/
static void __exit clbench_exit(void)
{
	cdev_del(&c_dev);	/* デバイスの削除 */

	del_timer_sync(&sigspec.flush_timer);	/* タイマの終了 */

	/* 循環リストを解放 */
	clist_free(clist_ctl);

	unregister_chrdev_region(dev_id, MINOR_COUNT);	/* メジャー番号の解放 */
	printk(KERN_INFO "%s : clbench is removed\n", log_prefix);
}

module_init(clbench_init);
module_exit(clbench_exit);


/*
	balance_tasks()@sched.cで呼び出される関数 ロードバランスが行われている箇所で呼び出される
	@p ロードバランスされたtask_structのアドレス
	@src_cpu 最も忙しいCPU番号
	@this_cpu ロードバランス先のCPU番号

	※カーネルイベントが補足される箇所にこの関数を挿入する
*/
void clbench_add_object(struct task_struct *p, int src_cpu, int this_cpu)
{
	struct object lb;
	struct timeval t;

	if(sigspec.sr_status != SIG_READY){	/* シグナルを送信できる状態かどうか */
		return;
	}

	do_gettimeofday(&t);

	lb.pid = p->pid;
	lb.sec = (long)t.tv_sec;
	lb.usec = (long)t.tv_usec;
	lb.src_cpu = src_cpu;
	lb.dst_cpu = this_cpu;

	/* この関数はフック先でしか実行されていないので、エラー処理は行っていない */

	clist_push_one((void *)&lb, clist_ctl);
}
EXPORT_SYMBOL(clbench_add_object);

MODULE_DESCRIPTION("clist-benchmark");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("K.Shimada");
