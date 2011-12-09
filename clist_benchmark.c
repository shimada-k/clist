#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>	/* rand(), srand() */
#include "clist.h"

#define SEND_FREQUENCY		1	/* send_workerが送る時間（秒） */
#define SEND_GRAIN_SIZE		6	/* send_workerが送るデータ単位量（オブジェクトの数） */

#define RECV_FREQUENCY_STATIC	8	/* recieve_workerが受信する静的時間（秒） */
#define RECV_FREQUENCY_DYNAMIC	5	/* recieve_workerが受信する動的時間（秒） */

#define RECV_FREQUENCY()		(rand() % RECV_FREQUENCY_DYNAMIC) + RECV_FREQUENCY_STATIC;

#define RECV_GRAIN_SIZE		6	/* recieve_workerが受信するデータ単位量（オブジェクトの数） */

unsigned int count;

struct sample_object{
	unsigned long long id_no;
	char padding[24];	/* 合計で32バイトになるように */
};


unsigned int count, spilled;

/*
	循環リストにデータをpushするスレッド
*/
void *send_worker(void *p)
{
	int i;
	size_t ret;
	struct clist_controler *clist_ctl;
	struct sample_object sobj[SEND_GRAIN_SIZE];

	clist_ctl = (struct clist_controler *)p;

	while(1){
		sleep(SEND_FREQUENCY);

		for(i = 0; i < SEND_GRAIN_SIZE; i++){
			sobj[i].id_no = count;
			strcpy(sobj[i].padding, "");

			count++;
		}

		ret = clist_push((void *)sobj, sizeof(struct sample_object) * SEND_GRAIN_SIZE, clist_ctl);

		if(ret != sizeof(struct sample_object) * SEND_GRAIN_SIZE){	/* 失敗したのでリトライ */
			int nr_written;
			struct sample_object *s;

			nr_written = ret / sizeof(struct sample_object);

			/* clist_push()で一つも書き込むことができなかった場合は？？ */

			printf("send_data_worker() ret:%ld, nr_written:%d\n", ret, nr_written);

			/* clist_push()に失敗したデータを表示 */
			for(i = 0; i < SEND_GRAIN_SIZE - nr_written; i++){

				s = &sobj[nr_written + i];

#ifdef DEBUG
				printf("\t*** sobj.id_no:%llu\n", s->id_no);
#endif
			}

			sleep(15);
			clist_push((void *)&sobj[nr_written], sizeof(struct sample_object) * (SEND_GRAIN_SIZE - nr_written), clist_ctl);

			spilled++;
		}
	}
}

/*
	循環リストからデータを回収するスレッド
*/
void *recieve_worker(void *p)
{
	int sleep_time, rlen, i;
	struct sample_object *sobj;
	struct clist_controler *clist_ctl;

	sobj = calloc(RECV_GRAIN_SIZE, sizeof(struct sample_object));

	srand(time(NULL));

	clist_ctl = (struct clist_controler *)p;

	while(1){

		sleep_time = RECV_FREQUENCY();
#ifdef DEBUG
		printf("sleep_time:%d\n", sleep_time);
#endif
		sleep(sleep_time);

		while(((rlen = clist_readable_len(clist_ctl, NULL, NULL)) / clist_ctl->node_len) > 0){

			int pick_len;

			pick_len = clist_pull((void *)sobj, sizeof(struct sample_object) * RECV_GRAIN_SIZE, clist_ctl);

#ifdef DEBUG
			printf("recieve_data_worker pick_len:%d\n",pick_len);
#endif

			if(pick_len % sizeof(struct sample_object) == 0){
#ifdef DEBUG
				for(i = 0; i < (pick_len / sizeof(struct sample_object)); i++){

					printf("\tsobj.id_no:%llu\n", sobj[i].id_no);
				}
				puts("\t*****");
#endif
			}
			else{
				break;
			}
		}
	}

	free(sobj);

}

#define RBUF_NR_STEP			8
#define RBUF_NR_STEP_COMPOSED	6

int main(int argc, char *argv[])
{
	int signo;
	struct clist_controler *clist_ctl;
	pthread_t send, recv;
	sigset_t ss;

	clist_ctl = clist_alloc(RBUF_NR_STEP, RBUF_NR_STEP_COMPOSED, sizeof(struct sample_object));

	pthread_create(&send , NULL , send_worker , (void *)clist_ctl);
	pthread_create(&recv , NULL , recieve_worker , (void *)clist_ctl);

	/* シグナルハンドリングの準備 */
	sigemptyset(&ss);
	/* block SIGTERM */
	sigaddset(&ss, SIGTERM);

	sigprocmask(SIG_BLOCK, &ss, NULL);

	/* シグナルが届くまでmainスレッドは無限ループ */
	while(1){
		if(sigwait(&ss, &signo) == 0){	/* シグナルが受信できたら */
			if(signo == SIGTERM){
				puts("main:sigterm recept");
				pthread_cancel(send);
				pthread_cancel(recv);
				break;
			}
		}
	}

	/* kill -s SIGTERM 'this program PID'でここにくる */

	pthread_join(send, NULL);
	pthread_join(recv, NULL);

	clist_free(clist_ctl);


	/* ベンチマーク結果を出力 */
	puts("------------ベンチマーク結果---------------");
	printf("入出力オブジェクト総数：%d\n", count);
	printf("循環リストの総回転数：%d\n", count / (RBUF_NR_STEP * RBUF_NR_STEP_COMPOSED));
	printf("データのpushをリトライした回数：%d\n", spilled);

	return 0;
}

