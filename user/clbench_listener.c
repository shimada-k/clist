#include <stdio.h>
#include <stdlib.h>		/* calloc(3) */
#include <unistd.h>		/* open(2), sleep(3) */
#include <sys/types.h>
#include <signal.h>		/* getpid(2) */

#include <fcntl.h>
#include <sys/ioctl.h>


#define IO_MAGIC				'k'
#define IOC_USEREND_NOTIFY			_IO(IO_MAGIC, 0)	/* ユーザアプリ終了時 */
#define IOC_SIGRESET_REQUEST		_IO(IO_MAGIC, 1)	/* send_sig_argをリセット要求 */
#define IOC_SUBMIT_SPEC			_IOW(IO_MAGIC, 2, void)	/* ユーザからのパラメータ設定 */


#define READ_NR_OBJECT	250

struct ioc_submit_spec{
	int pid;
	int signo, flush_period;
	int nr_node, node_nr_composed;
	int dummy;	/* padding防止のための変数 */
};

/*
	/dev/clbenchからデータを読んでファイルに書き出すプログラム
*/

struct lb_object{	/* やりとりするオブジェクト */
	pid_t pid, padding;
	int src_cpu, dst_cpu;
	long sec, usec;
};

int dev, out;	/* ファイルディスクリプタ */
int count;
void *buffer;

/*
	カーネルからのシグナルのハンドラ関数
	@sig シグナルハンドラの仕様
*/
void clbench_handler(int sig)
{
	ssize_t size;

	/* カーネルのメモリを読む */
	size = read(dev, buffer, sizeof(struct lb_object) * READ_NR_OBJECT);
	/* ファイルに書き出す */
	write(out, buffer, (size_t)size);

	printf("read(オブジェクト数): %d\n", (int)(size / sizeof(struct lb_object)));

	count += (int)(size / sizeof(struct lb_object));

	lseek(dev, 0, SEEK_SET);
}

int main(int argc, char *argv[])
{
	int nr_wcurr, signo, nr_picked, grain;
	struct sigaction act;
	struct ioc_submit_spec submit_spec;
	ssize_t size;

	dev = open("/dev/clbench", O_RDONLY);
	out = open("./output.clbench", O_CREAT|O_WRONLY|O_TRUNC);

	buffer = (struct lb_object *)calloc(READ_NR_OBJECT, sizeof(struct lb_object));

	/* デバイスの準備 */
	submit_spec.pid = (int)getpid();
	submit_spec.signo = SIGUSR1;
	submit_spec.flush_period = 1500;
	submit_spec.nr_node = 10;
	submit_spec.node_nr_composed = 100;

	ioctl(dev, IOC_SUBMIT_SPEC, &submit_spec);

	/* シグナルハンドリングの準備 */
	act.sa_handler = clbench_handler;
	act.sa_flags = 0;

	sigemptyset(&act.sa_mask);
	sigaction(SIGUSR1, &act, NULL);

	/* SIGTERMをブロックするための設定 */
	sigaddset(&act.sa_mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &act.sa_mask, NULL);

	/* シグナルが届くまでmainスレッドは無限ループ */
	while(1){
		if(sigwait(&act.sa_mask, &signo) == 0){	/* シグナルが受信できたら */
			if(signo == SIGTERM){
				puts("main:SIGTERM recept");
				break;
			}
		}
	}

	/***						***
	***	シグナルを受信するとここに到達する	***
	***						***/

	/* カーネル側に終了通知を送る */
	ioctl(dev, IOC_USEREND_NOTIFY, &nr_wcurr);

	printf("wcurr_len:%d\n", nr_wcurr);

	/* clist_pull_end()でpull残しがないように大きい方でメモリを確保 */
	if(nr_wcurr >= READ_NR_OBJECT){
		/* 一端freeして、再度calloc */
		free(buffer);
		buffer = calloc(nr_wcurr, sizeof(struct lb_object));

		grain = nr_wcurr;
	}
	else{
		/* bufferをそのまま使うのでcalloc無し */
		grain = READ_NR_OBJECT;
	}

	/* grainだけひたすら読んでread(2)が0を返したらbreakする */
	while(1){
		/* カーネルのメモリを読む */
		size = read(dev, buffer, sizeof(struct lb_object) * grain);

		if(size == 0){	/* ここを通るということはclist_benchmark側がSIGRESET_ACCEPTEDになったということ */
			break;
		}

		/* ファイルに書き出す */
		write(out, buffer, (size_t)size);

		printf("read(オブジェクト数): %d\n", (int)(size / sizeof(struct lb_object)));

		count += (int)(size / sizeof(struct lb_object));

		lseek(dev, 0, SEEK_SET);
	}

	putchar('\n');

	/* ベンチマーク結果を出力 */
	puts("------------ベンチマーク結果---------------");
	printf("入出力オブジェクト総数：%d\n", count);
	printf("読み込み粒度（オブジェクト数）：%d\n", READ_NR_OBJECT);
	printf("clistのノード数：%d\n", 10);
	printf("clistのノードに含まれるオブジェクト数：%d\n", 100);

	/* リソース解放 */
	free(buffer);

	close(out);
	close(dev);
}
