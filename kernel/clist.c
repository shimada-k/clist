#include <linux/kernel.h>
#include <linux/string.h>	/* memcpy */
#include <linux/slab.h>	/* kzalloc */
#include <linux/module.h>	/* EXPORT_SYMBOL */

#include <linux/clist.h>

/***********************************
*
*	ライブラリ内部関数
*
************************************/

/*
	循環リストにデータをコピーする関数
	@src コピーするデータ
	@len コピーするオブジェクトの個数
	@clist_ctl 管理構造体のアドレス

	データ長の検査はこの関数内では行っていない この関数内はクリティカルセクション
*/
static void clist_wmemcpy(const void *src, int n, struct clist_controler *clist_ctl)
{
	spin_lock(&clist_ctl->lock);

	memcpy(clist_ctl->w_curr->curr_ptr, src, objs_to_byte(clist_ctl, n));
	clist_ctl->w_curr->curr_ptr += objs_to_byte(clist_ctl, n);

	if(clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data == clist_ctl->node_len){
		clist_ctl->w_curr = clist_ctl->w_curr->next_node;		/* ノードが一杯になったので、次のノードにアドレスをつなぐ */
		smp_wmb();
		clist_ctl->pull_wait_length++;
	}

	spin_unlock(&clist_ctl->lock);
}

/*
	循環リストからデータをコピーする関数
	@dest コピー先のアドレス
	@len コピーするデータの長さ（バイト）
	@clist_ctl 管理構造体のアドレス

	データ長の検査はこの関数内では行っていない この関数内はクリティカルセクション
*/
static void clist_rmemcpy(void *dest, int n, struct clist_controler *clist_ctl)
{
	void *seek_head;

	spin_lock(&clist_ctl->lock);

	/* curr_ptrとdataから読み出すアドレスを計算する */
	seek_head = clist_ctl->r_curr->data + clist_ctl->node_len - (clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data);

	memcpy(dest, seek_head, objs_to_byte(clist_ctl, n));
	clist_ctl->r_curr->curr_ptr -= objs_to_byte(clist_ctl, n);

	if(clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data == 0){
		clist_ctl->r_curr = clist_ctl->r_curr->next_node;		/* w_currにノード1つ分だけ近づける */
		smp_wmb();
		clist_ctl->pull_wait_length--;
	}

	spin_unlock(&clist_ctl->lock);
}

/***********************************
*
*		公開用関数
*
************************************/

/*
	read可能なデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@n_first ノードに残っているバイト数を格納する関数（任意）
	@n_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return read可能なオブジェクトの個数

	※現在書き込み中のノードはread対象にはならない
*/
int clist_pullable_objects(const struct clist_controler *clist_ctl, int *n_first, int *n_burst)
{
	int first, burst;

	if(clist_ctl->pull_wait_length == 0){
		first = 0;
		burst = 0;
	}
	else if(clist_ctl->pull_wait_length >= 1){
			/* 読み残しのバイト数を計算 */
			first = (clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data) / clist_ctl->object_size;

		if(first > 0){

			if(first == clist_ctl->node_len){
				first = 0;
				burst = clist_ctl->pull_wait_length;
			}
			else{
				/* 読み残しの分もsub_nodeに含まれているので1を引く */
				burst = clist_ctl->pull_wait_length - 1;
			}
		}
		else{	/* 読み残し無し *first == 0 */
			burst = clist_ctl->pull_wait_length;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_pullable_objects pull_wait_length:%d first:%d n_burst:%d\n", clist_ctl->pull_wait_length, first, burst);
#endif

	/* NULLでなかったら引数のアドレスに代入 */
	if(n_first){
		*n_first = first;
	}
	if(n_burst){
		*n_burst = burst;
	}

	return first + (burst * clist_ctl->nr_composed);
}
EXPORT_SYMBOL(clist_pullable_objects);

/*
	write可能なデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@n_first ノードに残っているバイト数を格納する関数（任意）
	@n_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return write可能なオブジェクトの個数

	※現在読み込み中のノードはwrite対象にはならない
*/
int clist_pushable_objects(const struct clist_controler *clist_ctl, int *n_first, int *n_burst)
{
	int curr_len, flen, burst;

	if(clist_ctl->pull_wait_length == clist_ctl->nr_node){
		/* w_currがr_currに追いついているなら0 */
		flen = 0;
		burst = 0;
	}
	else{
		/* w_currに何バイトまで書き込みされているか計算 */
		curr_len = clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data;
		burst = clist_ctl->nr_node - clist_ctl->pull_wait_length;

		/* w_currにあと何バイト書き込めるか計算 */
		if(clist_ctl->node_len > curr_len){
#ifdef DEBUG
			printk(KERN_INFO "clist_pushable_objects() curr_len:%d, burst:%d\n", curr_len, burst);
#endif
			flen = (clist_ctl->node_len - curr_len) / clist_ctl->object_size;
			burst -= 1;	/* burstには書き込み中のノードも含んでいるため-1 */
		}
		else{	/* curr_ptr - dataは0未満にはならないのでここを通るということはcurr_ptr == data */
			;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_pushable_objects() nr_node:%d - pull_wait_length:%d = %d\n", clist_ctl->nr_node, clist_ctl->pull_wait_length, clist_ctl->nr_node - clist_ctl->pull_wait_length);
#endif

	/* 引数のアドレスが有効なら代入する */
	if(n_first){
		*n_first = flen;
	}

	if(n_burst){
		*n_burst = burst;
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_pushable_objects flen:%d, burst:%d\n", flen, burst);
#endif

	return flen + (burst * clist_ctl->nr_composed);
}
EXPORT_SYMBOL(clist_pushable_objects);

/*
	循環リスト内に存在するすべてのデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@n_first ノードに残っているバイト数を格納する関数（任意）
	@n_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return w_currに存在しているオブジェクトの個数

	※read中、write中すべてのデータを計算する。この関数を呼び出すとclistは入出力禁止モードに突入する clist_kfree()の直前に呼び出すこと
*/
int clist_set_cold(struct clist_controler *clist_ctl, int *n_first, int *n_burst)
{
	int first, burst;

	clist_ctl->state = CLIST_STATE_COLD;	/* 入出力禁止状態に遷移させる */

	clist_pullable_objects(clist_ctl, &first, &burst);

#ifdef DEBUG
	printk(KERN_INFO "clist_set_cold pull_wait_length:%d first:%d n_burst:%d\n", clist_ctl->pull_wait_length, first, burst);
#endif

	/* NULLでなかったら引数のアドレスに代入 */
	if(n_first){
		*n_first = first;
	}
	if(n_burst){
		*n_burst = burst;
	}

	return (int)(clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data) / clist_ctl->object_size;
}
EXPORT_SYMBOL(clist_set_cold);


/*
	メモリをallocして循環リストを構築する関数
	@nr_node 循環リストの段数
	@nr_composed 循環リスト１段に含まれるオブジェクトの数

	return 成功:clist_controlerのアドレス 失敗:NULL
*/
struct clist_controler *clist_alloc(int nr_node, int nr_composed, int object_size)
{
	int i;
	struct clist_controler *clist_ctl;

	clist_ctl = (struct clist_controler *)kzalloc(sizeof(struct clist_controler), GFP_KERNEL);

	if(clist_ctl == NULL){	/* エラー */
		return NULL;
	}

	clist_ctl->pull_wait_length = 0;

	clist_ctl->nr_node = nr_node;
	clist_ctl->node_len = object_size * nr_composed;

	clist_ctl->nr_composed = nr_composed;
	clist_ctl->object_size = object_size;

#ifdef DEBUG
	printk(KERN_INFO "alloc_clist() nr_node:%d, node_len:%d\n", clist_ctl->nr_node, clist_ctl->node_len);
#endif

	/* メモリを確保 */
	clist_ctl->nodes = (struct clist_node *)kzalloc(nr_node * sizeof(struct clist_node), GFP_KERNEL);

	if(clist_ctl->nodes == NULL){	/* エラー */
		return NULL;
	}

	for(i = 0; i < clist_ctl->nr_node; i++){
		clist_ctl->nodes[i].data = (void *)kzalloc(clist_ctl->node_len, GFP_KERNEL);

		if(clist_ctl->nodes[i].data == NULL){	/* エラー */
			return NULL;
		}
	}

	/* アドレスをつなぐ */
	for(i = 0; i < clist_ctl->nr_node; i++){
		if(i < clist_ctl->nr_node - 1){
			clist_ctl->nodes[i].next_node = &clist_ctl->nodes[i + 1];
		}
		else{	/* 最後のcellは最初のcellにつなぐ */
			clist_ctl->nodes[i].next_node = &clist_ctl->nodes[0];
		}

		clist_ctl->nodes[i].curr_ptr = clist_ctl->nodes[i].data;
	}

	/* 初期値を代入 */
	clist_ctl->w_curr = &clist_ctl->nodes[0];
	clist_ctl->r_curr = &clist_ctl->nodes[0];

	/* 入出力可能フラグ */
	clist_ctl->state = CLIST_STATE_HOT;

	/* spinlock初期化 */
	spin_lock_init(&clist_ctl->lock);

	return clist_ctl;
}
EXPORT_SYMBOL(clist_alloc);

/*
	メモリを解放する関数
	@clist_ctl ユーザがallocしたclist_controler構造体のアドレス
*/
void clist_free(struct clist_controler *clist_ctl)
{
	int i;

	/* データを解放 */
	for(i = 0; i < clist_ctl->nr_node; i++){
		kfree(clist_ctl->nodes[i].data);
	}

	/* ノードを解放 */
	kfree(clist_ctl->nodes);
	kfree(clist_ctl);
}
EXPORT_SYMBOL(clist_free);

/*
	循環リストに1オブジェクトだけデータを追加する関数
	@data データが入っているアドレス
	@clist_ctl 管理用構造体のアドレス
	return 実際に書き込んだオブジェクトの個数
*/
int clist_push_one(const void *data, struct clist_controler *clist_ctl)
{
	int write_scope;

	write_scope = clist_pushable_objects(clist_ctl, NULL, NULL);

	if(write_scope){
		clist_wmemcpy(data, 1, clist_ctl);
		return 1;
	}
	else{
		return 0;
	}
}
EXPORT_SYMBOL(clist_push_one);

/*
	循環リストに1オブジェクトだけデータを読み取る関数
	@data データを格納するアドレス
	@clist_ctl 管理用構造体のアドレス
	return 実際に読み込んだバイト数
*/
int clist_pull_one(void *data, struct clist_controler *clist_ctl)
{
	int read_scope;

	read_scope = clist_pullable_objects(clist_ctl, NULL, NULL);

	if(read_scope){
		clist_rmemcpy(data, 1, clist_ctl);
		return 1;
	}
	else{
		return 0;
	}
}
EXPORT_SYMBOL(clist_pull_one);

/*
	循環リストにデータを追加する関数
	@data データが入っているアドレス
	@n オブジェクトの個数
	return 成功：追加したオブジェクトの個数　失敗：マイナスのエラーコード

	※この関数がn以下の値を返した時は循環リストが一周しているのでユーザ側で再送するか、データ量を再検討する必要がある
*/
int clist_push_order(const void *data, int n, struct clist_controler *clist_ctl)
{
	int i;
	int write_scope, n_first = 0, n_burst = 0;
	int ret = 0;

	write_scope = clist_pushable_objects(clist_ctl, &n_first, &n_burst);

	if(CLIST_IS_COLD(clist_ctl)){
		ret = -EAGAIN;	/* push禁止だったらエラー */
	}
	else if(n >= write_scope){
#ifdef DEBUG
		printk(KERN_INFO "clist_push() 1st-if, n:%d, write_scope:%d, n_first:%d, n_burst:%d\n", n, write_scope, n_first, n_burst);
#endif
		/* 現在のノードに書き込めるだけ書き込む */
		if(n_first > 0){
			clist_wmemcpy(data, n_first, clist_ctl);
			ret += n_first;
		}

		if(clist_ctl->w_curr == clist_ctl->r_curr){	/* w_currがr_currに追いついた */
#ifdef DEBUG
			printk(KERN_INFO "clist_push() w_curr == r_curr. alloc more memory or retry. pull_wait_length:%d\n", clist_ctl->pull_wait_length);
			clist_ctl->state = CLIST_STATE_COLD;	/* push禁止にする */
#endif	
			return n_first;
		}

		/* ノード単位で書き込む */
		for(i = 0; i < n_burst; i++){
			clist_wmemcpy(data + ret, clist_ctl->nr_composed, clist_ctl);
			ret += clist_ctl->nr_composed;
		}
	}
	else{	/* len < write_scope */

		if(n >= n_first){		/* 現在のノードに書き込めるだけ書き込む */

			/* n_burstを再計算 */
			n_burst = (n - n_first) / clist_ctl->nr_composed;
#ifdef DEBUG
			printk(KERN_INFO "clist_push() 2nd-if, n:%d, write_scope:%d, n_first:%d, n_burst:%d\n", n, write_scope, n_first, n_burst);
#endif

			if(n_first > 0){
				clist_wmemcpy(data, n_first, clist_ctl);
				ret += n_first;
			}

			if(clist_ctl->w_curr == clist_ctl->r_curr){	/* w_currがr_currに追いついた */
#ifdef DEBUG
				printk(KERN_INFO "clist_push() w_curr == r_curr. alloc more memory or retry. pull_wait_length:%d\n", clist_ctl->pull_wait_length);
				clist_ctl->state = CLIST_STATE_COLD;	/* push禁止にする */
#endif	
				return n_first;
			}

			/* ノード単位で書き込む */
			for(i = 0; i < n_burst; i++){
				clist_wmemcpy(data + ret, clist_ctl->nr_composed, clist_ctl);
				ret += clist_ctl->nr_composed;
			}

			/* 最後に残った半端なものを書き込む */
			if(n - ret > 0){
				clist_wmemcpy(data + ret, n - ret, clist_ctl);
				ret += n - ret;
			}
		}
		else{	/* len < n_first */
			/* lenだけ書き込む */
			printk(KERN_INFO "ret:%d, n:%d\n", ret, n);

			clist_wmemcpy(data + ret, n, clist_ctl);
			ret += n;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "n:%d ret:%d\n", n, ret);
#endif

	return ret;
}
EXPORT_SYMBOL(clist_push_order);

/*
	循環リストからlenだけデータを読む関数
	@data データを格納するアドレス
	@n オブジェクトの個数
	return dataに格納したデータサイズ

	※書き込みが完了したノードしか読まない仕様
*/
int clist_pull_order(void *data, int n, struct clist_controler *clist_ctl)
{
	int i, n_first = 0, n_burst = 0;
	int ret = 0, read_scope;

	/* 読める最大サイズを計算する */
	read_scope = clist_pullable_objects(clist_ctl, &n_first, &n_burst);

	if(n >= read_scope){	/* 読める上限（read_scope）だけ読む */

		if(n_first){
			clist_rmemcpy(data, n_first, clist_ctl);
			ret += n_first;
		}

		/* ノード単位で読む */
		if(n_burst){

			for(i = 0; i < n_burst; i++){
				clist_rmemcpy(data + ret, clist_ctl->nr_composed, clist_ctl);
				ret += clist_ctl->nr_composed;
			}
		}
	}
	else{	/* n < read_scope */

		if(n >= n_first){

			/* n_burstを再計算 */
			n_burst = (n - n_first) / clist_ctl->nr_composed;

			/* 読み残しを読む */
			if(n_first){
				clist_rmemcpy(data, n_first, clist_ctl);
				ret += n_first;
			}
#ifdef DEBUG
			printk(KERN_INFO "pick_node() loop number:%d\n", (n - n_first) / clist_ctl->node_len);
#endif

			/* ノード単位で読む */
			for(i = 0; i < n_burst; i++){
				clist_rmemcpy(data + ret, clist_ctl->nr_composed, clist_ctl);
				ret += clist_ctl->nr_composed;
			}
#ifdef DEBUG
			printk(KERN_INFO "pick_node() odd number:%d\n", (n - n_first) % clist_ctl->nr_composed);
#endif

			/* 半端な長さのものを読む */
			if(n - ret > 0){
				clist_rmemcpy(data + ret, n - ret, clist_ctl);
				ret += n - ret;
			}
		}
		else{	/* len < n_first */
			clist_rmemcpy(data, n, clist_ctl);
			ret += n;
		}
	}

	if(CLIST_IS_COLD(clist_ctl)){
		clist_ctl->state = CLIST_STATE_HOT;	/* push許可に設定 */
	}

	return ret;
}
EXPORT_SYMBOL(clist_pull_order);


/*
	循環リストからw_currのノードからデータを読む関数
	@data データを格納するアドレス
	return 成功：dataに格納したオブジェクトの個数 失敗：マイナスのエラーコード

	※clist_set_cold()の後に呼び出されないといけない
	※この関数は1度だけ呼ぶ
	※dataは書き込み中ノードにあるデータサイズ分のメモリ領域が確保されている必要がある
*/
int clist_pull_end(void *data, struct clist_controler *clist_ctl)
{
	int len;

	len = clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data;

	if(CLIST_IS_HOT(clist_ctl)){
		return -ECANCELED;
	}

	memcpy(data, clist_ctl->w_curr->data, len);
	clist_ctl->w_curr->curr_ptr -= len;

	return byte_to_objs(clist_ctl, len);
}
EXPORT_SYMBOL(clist_pull_end);

