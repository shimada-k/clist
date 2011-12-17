#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>	/* strerror() */
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

static int death_flag = 0;

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
	int ret = 0;
	struct clist_controler *clist_ctl;
	struct sample_object sobj[SEND_GRAIN_SIZE];

	clist_ctl = (struct clist_controler *)p;

	while(1){
		if(death_flag == 1){
			break;
		}

		sleep(SEND_FREQUENCY);

		for(i = 0; i < SEND_GRAIN_SIZE; i++){
			sobj[i].id_no = count;
			strcpy(sobj[i].padding, "");

			count++;
		}

		//for(i = 0; i < SEND_GRAIN_SIZE; i++){
		//	ret += clist_push_one((void *)&sobj[i], clist_ctl);
		//}
		ret = clist_push_order((void *)sobj, SEND_GRAIN_SIZE, clist_ctl);

		if(ret < 0){
			/* clist側からエラーが帰ってきている */
			printf("%s\n", strerror(-ret));
			spilled++;

			sleep(15);
		}
		else if(ret != SEND_GRAIN_SIZE){
			struct sample_object *s;

			/* clist_push()に失敗したデータを表示 */
			for(i = 0; i < SEND_GRAIN_SIZE - ret; i++){

				s = &sobj[ret + i];

#ifdef DEBUG
				printf("\t*** sobj[%d].id_no:%llu\n", i, s->id_no);
#endif
			}

			sleep(15);
			clist_push_order((void *)&sobj[ret], (SEND_GRAIN_SIZE - ret), clist_ctl);
		}
		ret = 0;
	}

	death_flag = 2;

	return NULL;
}

/*
	循環リストからデータを回収するスレッド
*/
void *recieve_worker(void *p)
{
	int sleep_time, i;
	struct sample_object *sobj;
	struct clist_controler *clist_ctl;

	sobj = calloc(RECV_GRAIN_SIZE, sizeof(struct sample_object));

	srand(time(NULL));

	clist_ctl = (struct clist_controler *)p;

	while(1){
		if(death_flag == 2){
			break;
		}

		sleep_time = RECV_FREQUENCY();
#ifdef DEBUG
		printf("sleep_time:%d\n", sleep_time);
#endif
		sleep(sleep_time);

		while(clist_wlen(clist_ctl) > 0){

			int pick_len = 0;

			pick_len = clist_pull_order((void *)sobj, RECV_GRAIN_SIZE, clist_ctl);

			//for(i = 0; i < RECV_GRAIN_SIZE; i++){
			//	pick_len += clist_pull_one((void *)&sobj[i], clist_ctl);
			//}

#ifdef DEBUG
			printf("recieve_data_worker pick_len:%d\n",pick_len);
#endif

#ifdef DEBUG
			for(i = 0; i < pick_len; i++){
				printf("\tsobj[%d].id_no:%llu\n", i, sobj[i].id_no);
			}
			puts("\t*****");
#endif
		}
	}

	free(sobj);

	return NULL;

}

void recieve_end(struct clist_controler *clist_ctl)
{
	int remain_len, pick_len, i;
	struct sample_object *sobj;

	remain_len = clist_set_cold(clist_ctl, NULL, NULL);

	sobj = calloc(remain_len, sizeof(struct sample_object));

	while(1){
		pick_len = clist_pull_order((void *)sobj, remain_len, clist_ctl);

#ifdef DEBUG
		puts("\tclist_pull done");
		for(i = 0; i < pick_len; i++){
			printf("\tsobj[%d].id_no:%llu\n", i, sobj[i].id_no);
		}
		puts("\t*****");
#endif

		if(pick_len != remain_len || pick_len == 0){
			pick_len = clist_pull_end(sobj, clist_ctl);
#ifdef DEBUG
			puts("\tclist_pull_end() done");
			for(i = 0; i < pick_len; i++){
				printf("\tsobj[%d].id_no:%llu\n", i, sobj[i].id_no);
			}
			puts("\t*****");
#endif
			break;
		}
	}
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
				death_flag = 1;
				break;
			}
		}
	}

	/* kill -s SIGTERM 'this program PID'でここにくる */

	pthread_join(send, NULL);
	pthread_join(recv, NULL);

	recieve_end(clist_ctl);

	clist_free(clist_ctl);

	/* ベンチマーク結果を出力 */
	puts("------------ベンチマーク結果---------------");
	printf("入出力オブジェクト総数：%d\n", count);
	printf("循環リストの総回転数：%d\n", count / (RBUF_NR_STEP * RBUF_NR_STEP_COMPOSED));
	printf("データのpushをリトライした回数：%d\n", spilled);

	return 0;
}

