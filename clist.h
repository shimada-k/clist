#define CLIST_STATE_HOT	1
#define CLIST_STATE_COLD	0

/* 循環リストのノード */
struct clist_node{
	void *data;		/* ここにメモリを確保する */
	struct clist_node *next_node;

	void *curr_ptr;	/* dataに次に格納するべきアドレス */
};

/* 循環リスト管理用構造体 */
struct clist_controler{
	int state;		/* CLIST_STATE_COLD:循環リストに対しての入出力禁止 CLIST_STATE_HOT:循環リストに対しての入出力可能*/

	int read_wait_length;	/* read待ちのエントリの数 */
	int nr_node, node_len;
	int object_size;

	struct clist_node *nodes;

	/*
		w_curr:書き込み用clist_nodeの先頭アドレス
		r_curr:読み込み用clist_nodeの先頭アドレス
	*/
	struct clist_node *w_curr, *r_curr;
};

/* プロトタイプ宣言 */
size_t clist_readable_len(const struct clist_controler *clist_ctl, int *first_len, int *nr_entry);
size_t clist_writable_len(const struct clist_controler *clist_ctl, int *first_len, int *nr_burst);

/* データ構造のalloc/free */
struct clist_controler *clist_alloc(int nr_node, int nr_composed, size_t object_size);
void clist_free(struct clist_controler *clist_ctl);

/* 循環リストにデータを書き込む/読み込む関数 */
int clist_push(const void *data, size_t len, struct clist_controler *clist_ctl);
int clist_pull(void *data, size_t len, struct clist_controler *clist_ctl);

/* 最後にデータを読みきる関数 */
size_t clist_set_cold(struct clist_controler *clist_ctl, int *first_len, int *nr_burst);
int clist_pull_end(void *data, int len, struct clist_controler *clist_ctl);
