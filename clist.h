#define CLIST_STATE_COLD	0
#define CLIST_STATE_HOT	1


#define CLIST_IS_HOT(ctl)	(ctl->state)
#define CLIST_IS_COLD(ctl)	(ctl->state ? 0 : 1)


#define clist_wlen(ctl)	ctl->pull_wait_length
#define objs_to_byte(ctl, n)	(ctl->object_size * n)
#define byte_to_objs(ctl, byte)	(byte / ctl->object_size)


/* 循環リストのノード */
struct clist_node{
	void *data;		/* ここにメモリを確保する */
	struct clist_node *next_node;

	void *curr_ptr;	/* dataに次に格納するべきアドレス */
};

/* 循環リスト管理用構造体 */
struct clist_controler{
	int state;		/* CLIST_STATE_COLD:循環リストに対しての入出力禁止 CLIST_STATE_HOT:循環リストに対しての入出力可能*/

	int pull_wait_length;	/* pull待ちのnodeの数 */
	int nr_node, node_len;
	int nr_composed, object_size;

	struct clist_node *nodes;

	/*
		w_curr:書き込み中のclist_nodeのアドレス
		r_curr:読み込み中のclist_nodeのアドレス
	*/
	struct clist_node *w_curr, *r_curr;
};

/* プロトタイプ宣言 */
int clist_pullable_objects(const struct clist_controler *clist_ctl, int *n_first, int *n_burst);
int clist_pushable_objects(const struct clist_controler *clist_ctl, int *n_first, int *n_burst);

/* データ構造のalloc/free */
struct clist_controler *clist_alloc(int nr_node, int nr_composed, int object_size);
void clist_free(struct clist_controler *clist_ctl);

/* 循環リストにデータを書き込む/読み込む関数 */

/* 単一オブジェクト版 */
int clist_push_one(const void *data, struct clist_controler *clist_ctl);
int clist_pull_one(void *data, struct clist_controler *clist_ctl);
/* 複数オブジェクト版 */
int clist_push_order(const void *data, int n, struct clist_controler *clist_ctl);
int clist_pull_order(void *data, int n, struct clist_controler *clist_ctl);

/* 最後にデータを読みきる関数 */
int clist_set_cold(struct clist_controler *clist_ctl, int *n_first, int *n_burst);
int clist_pull_end(void *data, struct clist_controler *clist_ctl);
