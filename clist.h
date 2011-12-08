/* 循環リストのノード */
struct clist_node{
	void *data;		/* ここにメモリを確保する */
	struct clist_node *next_node;

	void *curr_ptr;	/* dataに次に格納するべきアドレス */
};

/* 循環リスト管理用構造体 */
struct clist_controler{
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
struct clist_controler *clist_alloc(int nr_node, int nr_composed, size_t object_size);
void clist_free(struct clist_controler *clist_ctl);
int clist_push(const void *data, size_t len, struct clist_controler *clist_ctl);
int clist_pull(void *data, size_t len, struct clist_controler *clist_ctl);
