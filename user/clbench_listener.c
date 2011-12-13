#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <stdlib.h>	/* calloc(3), exit(3), EXIT_SUCCESS */
#include <pthread.h>	/* pthread_exit(3), pthread_join(3), pthread_create(3) */
#include <unistd.h>	/* open(2), sleep(3) */
#include <sys/types.h>
#include <sys/stat.h>	/* mkdir(2) */
#include <signal.h>	/* getpid(2) */

#include <fcntl.h>
#include <sys/ioctl.h>


#define IO_MAGIC				'k'
#define IOC_USEREND_NOTIFY			_IO(IO_MAGIC, 0)	/* ユーザアプリ終了時 */
#define IOC_SIGRESET_REQUEST		_IO(IO_MAGIC, 1)	/* send_sig_argをリセット要求 */
#define IOC_SET_SIGNO			_IO(IO_MAGIC, 2)	/* シグナル番号を設定 */
#define IOC_SET_NR_NODE			_IO(IO_MAGIC, 3)	/* データの転送粒度を設定 */
#define IOC_SET_NODE_NR_COMPOSED		_IO(IO_MAGIC, 4)	/* データの転送粒度を設定 */
#define IOC_SET_PID				_IO(IO_MAGIC, 5)	/* PIDを設定 */

#define READ_NR_OBJECT	12

/*
	/dev/clbenchからデータを読んでファイルに書き出すプログラム
*/

struct lb_object{	/* やりとりするオブジェクト */
	pid_t pid;
	unsigned long seconds;
	int src_cpu, dst_cpu;
};

int dev, out;
int count;
void *handler_buffer;

/*
	カーネルからのシグナルのハンドラ関数
	@sig シグナルハンドラの仕様
*/
void clbench_handler(int sig)
{
	ssize_t size;

	/* カーネルのメモリを読む */
	size = read(dev, handler_buffer, sizeof(struct lb_object) * READ_NR_OBJECT);
	/* ファイルに書き出す */
	write(out, handler_buffer, (size_t)size);

	count += (int)(size / sizeof(struct lb_object));

	lseek(dev, 0, SEEK_SET);
}

int main(int argc, char *argv[])
{
	int piece, signo;
	void *temp_mem;
	struct sigaction act;

	dev = open("/dev/clbench", O_RDONLY);
	out = open("./output.clbench", O_RDWR|O_CREAT);

	handler_buffer = (struct lb_object *)calloc(READ_NR_OBJECT, sizeof(struct lb_object));

	/* デバイスの準備 */
	ioctl(dev, IOC_SET_SIGNO, SIGUSR1);
	ioctl(dev, IOC_SET_NR_NODE, 8);
	ioctl(dev, IOC_SET_NODE_NR_COMPOSED, 10);
	ioctl(dev, IOC_SET_PID, (int)getpid());

	/* シグナルハンドリングの準備 */
	act.sa_handler = clbench_handler;
	act.sa_flags = 0;

	sigemptyset(&act.sa_mask);
	sigaction(SIGUSR1, &act, NULL);

	/* block SIGTERM */
	sigaddset(&act.sa_mask, SIGTERM);

	sigprocmask(SIG_BLOCK, &act.sa_mask, NULL);

	/* シグナルが届くまでmainスレッドは無限ループ */
	while(1){
		if(sigwait(&act.sa_mask, &signo) == 0){	/* シグナルが受信できたら */
			if(signo == SIGTERM){
				puts("main:sigterm recept");
				break;
			}
		}
	}

	/* カーネル側に終了通知を送る */
	ioctl(dev, IOC_USEREND_NOTIFY, &piece);

#ifdef DEBUG
	printf("piece:%d\n", piece);
#endif

	temp_mem = (void *)malloc(piece * sizeof(struct lb_object));

	while(1){
		ssize_t size;

		/* カーネルメモリを読む */
		size = read(dev, temp_mem, sizeof(struct lb_object) * piece);
		/* ファイルに書き出す */
		write(out, temp_mem, (size_t)size);

		count += (int)(size / sizeof(struct lb_object));

		if(size != sizeof(struct lb_object) * piece || size == 0){
			break;
		}

	}

	/* ベンチマーク結果を出力 */
	puts("------------ベンチマーク結果---------------");
	printf("入出力オブジェクト総数：%d\n", count);

	free(temp_mem);
	free(handler_buffer);
	close(out);
	close(dev);
}
