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
	pid_t pid;
	unsigned long seconds;
	int src_cpu, dst_cpu;
};


int main(int argc, char *argv[])
{
	int i, counter;
	struct object obj;

	f_objs = fopen(argv[1], "rb");
	f_csv = fopen(argv[2], "w");


	while(fread(&obj, sizeof(struct object), 1, f_objs) == 1){
		printf("[%lu] PID:%d, CPU#%d -> CPU#%d\n", obj.seconds, obj.pid, obj.src_cpu, obj.dst_cpu);
	}

	fclose(f_objs);
	fclose(f_csv);

	return 0;
}

