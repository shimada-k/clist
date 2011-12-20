#include <stdio.h>
#include <stdlib.h>	/* exit(3) */
#include <unistd.h>	/* sysconf(3) */
#include <string.h>	/* memset(3) */

/*
+	オブジェクト列のファイルからCSVファイルを書き出すプログラム
+
+	./objs2csv "入力元(オブジェクトファイル名)" "出力先(CSVファイル名)" 
+
*/

FILE *f_objs, *f_csv;

struct object{	/* やりとりするオブジェクト */
	pid_t pid, padding;
	int src_cpu, dst_cpu;
	long sec, usec;
};


int main(int argc, char *argv[])
{
	int i, counter = 0;
	struct object obj;

	/* 引数からファイルをオープン */
	f_objs = fopen(argv[1], "rb");
	f_csv = fopen(argv[2], "w");

	printf("sizeof(struct object):%d\n", sizeof(struct object));

	while(fread(&obj, sizeof(struct object), 1, f_objs) == 1){
		printf("[%d.%d] PID:%d, CPU#%d --> CPU#%d\n", obj.sec, obj.usec, obj.pid, obj.src_cpu, obj.dst_cpu);
		counter++;
	}

	putchar('\n');
	printf("総オブジェクト数：%d\n", counter);

	fclose(f_objs);
	fclose(f_csv);

	return 0;
}

