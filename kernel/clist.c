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
	@len コピーするデータの長さ
	@clist_ctl 管理構造体のアドレス

	データ長の検査はこの関数内では行っていない この関数内はクリティカルセクション
*/
static void clist_wmemcpy(const void *src, size_t len, struct clist_controler *clist_ctl)
{
	memcpy(clist_ctl->w_curr->curr_ptr, src, len);
	clist_ctl->w_curr->curr_ptr += len;

	if(clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data == clist_ctl->node_len){
		clist_ctl->w_curr = clist_ctl->w_curr->next_node;		/* ノードが一杯になったので、次のノードにアドレスをつなぐ */
		clist_ctl->read_wait_length++;
	}
}

/*
	循環リストからデータをコピーする関数
	@dest コピー先のアドレス
	@len コピーするデータの長さ
	@clist_ctl 管理構造体のアドレス

	データ長の検査はこの関数内では行っていない この関数内はクリティカルセクション
*/
static void clist_rmemcpy(void *dest, size_t len, struct clist_controler *clist_ctl)
{
	void *seek_head;

	/* curr_ptrとdataから読み出すアドレスを計算する */
	seek_head = clist_ctl->r_curr->data + clist_ctl->node_len - (clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data);

	memcpy(dest, seek_head, len);
	clist_ctl->r_curr->curr_ptr -= len;

	if(clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data == 0){
		clist_ctl->r_curr = clist_ctl->r_curr->next_node;		/* w_currにノード1つ分だけ近づける */
		clist_ctl->read_wait_length--;
	}
}


/***********************************
*
*		公開用関数
*
************************************/

/*
	read可能なデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@first_len ノードに残っているバイト数を格納する関数（任意）
	@nr_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return read可能なデータのサイズ（バイト）

	※現在書き込み中のノードはread対象にはならない
*/
size_t clist_readable_len(const struct clist_controler *clist_ctl, int *first_len, int *nr_burst)
{
	int first, burst;

	if(clist_ctl->read_wait_length == 0){
		first = 0;
		burst = 0;
	}
	else if(clist_ctl->read_wait_length >= 1){
			/* 読み残しのバイト数を計算 */
			first = clist_ctl->r_curr->curr_ptr - clist_ctl->r_curr->data;

		if(first > 0){

			if(first == clist_ctl->node_len){
				first = 0;
				burst = clist_ctl->read_wait_length;
			}
			else{
				/* 読み残しの分もsub_nodeに含まれているので1を引く */
				burst = clist_ctl->read_wait_length - 1;
			}
		}
		else{	/* 読み残し無し *first == 0 */
			burst = clist_ctl->read_wait_length;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_readable_len read_wait_length:%d first:%d nr_burst:%d\n", clist_ctl->read_wait_length, first, burst);
#endif

	/* NULLでなかったら引数のアドレスに代入 */
	if(first_len){
		*first_len = first;
	}
	if(nr_burst){
		*nr_burst = burst;
	}

	return first + (burst * clist_ctl->node_len);
}
EXPORT_SYMBOL(clist_readable_len);


/*
	write可能なデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@first_len ノードに残っているバイト数を格納する関数（任意）
	@nr_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return write可能なデータのサイズ（バイト）

	※現在読み込み中のノードはwrite対象にはならない
*/
size_t clist_writable_len(const struct clist_controler *clist_ctl, int *first_len, int *nr_burst)
{
	int curr_len, flen, burst;

	if(clist_ctl->read_wait_length == clist_ctl->nr_node){
		/* w_currがr_currに追いついているなら0 */
		flen = 0;
		burst = 0;
	}
	else{
		/* w_currに何バイトまで書き込みされているか計算 */
		curr_len = clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data;
		burst = clist_ctl->nr_node - clist_ctl->read_wait_length;

		/* w_currにあと何バイト書き込めるか計算 */
		if(clist_ctl->node_len > curr_len){
#ifdef DEBUG
			printk(KERN_INFO "clist_writable_len() curr_len:%d, burst:%d\n", curr_len, burst);
#endif
			flen = clist_ctl->node_len - curr_len;
			burst -= 1;	/* burstには書き込み中のノードも含んでいるため-1 */
		}
		else{	/* curr_ptr - dataは0未満にはならないのでここを通るということはcurr_ptr == data */
			;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_writable_len() nr_node:%d - read_wait_length:%d = %d\n", clist_ctl->nr_node, clist_ctl->read_wait_length, clist_ctl->nr_node - clist_ctl->read_wait_length);
#endif

	/* 引数のアドレスが有効なら代入する */
	if(first_len){
		*first_len = flen;
	}

	if(nr_burst){
		*nr_burst = burst;
	}

#ifdef DEBUG
	printk(KERN_INFO "clist_writable_len flen:%d, burst:%d\n", flen, burst);
#endif

	return flen + (burst * clist_ctl->node_len);
}
EXPORT_SYMBOL(clist_writable_len);

/*
	循環リスト内に存在するすべてのデータのサイズを返す関数
	@clist_ctl 管理用構造体のアドレス
	@first_len ノードに残っているバイト数を格納する関数（任意）
	@nr_burst ノード丸ごと読む場合、いくつのノードか（任意）
	return w_currに存在しているデータ量（バイト）

	※read中、write中すべてのデータを計算する。この関数を呼び出すとclistは入出力禁止モードに突入する clist_free()の直前に呼び出すこと
*/
size_t clist_set_cold(struct clist_controler *clist_ctl, int *first_len, int *nr_burst)
{
	int first, burst;

	clist_ctl->state = CLIST_STATE_COLD;	/* 入出力禁止状態に遷移させる */

	clist_readable_len(clist_ctl, &first, &burst);

#ifdef DEBUG
	printk(KERN_INFO "clist_current_len read_wait_length:%d first:%d nr_burst:%d\n", clist_ctl->read_wait_length, first, burst);
#endif

	/* NULLでなかったら引数のアドレスに代入 */
	if(first_len){
		*first_len = first;
	}
	if(nr_burst){
		*nr_burst = burst;
	}

	return (size_t)(clist_ctl->w_curr->curr_ptr - clist_ctl->w_curr->data);
}
EXPORT_SYMBOL(clist_set_cold);

/*
	メモリをallocして循環リストを構築する関数
	@nr_node 循環リストの段数
	@nr_composed 循環リスト１段に含まれるオブジェクトの数

	return 成功:clist_controlerのアドレス 失敗:NULL
*/
struct clist_controler *clist_alloc(int nr_node, int nr_composed, size_t object_size)
{
	int i;
	struct clist_controler *clist_ctl;

	clist_ctl = (struct clist_controler *)kzalloc(sizeof(struct clist_controler), GFP_KERNEL);

	if(clist_ctl == NULL){	/* エラー */
		return NULL;
	}

	clist_ctl->read_wait_length = 0;

	clist_ctl->nr_node = nr_node;
	clist_ctl->node_len = object_size * nr_composed;
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
	循環リストにデータを追加する関数
	@data データが入っているアドレス
	@len データの長さ（バイト）
	return 成功：追加したデータの大きさ　失敗：マイナスのエラーコード

	※この関数がlen以下の値を返した時は循環リストが一周しているのでユーザ側で再送するか、データ量を再検討する必要がある
*/
int clist_push(const void *data, size_t len, struct clist_controler *clist_ctl)
{
	int i;
	int write_scope, remain_len = 0, nr_burst = 0;
	size_t ret = 0;

	write_scope = clist_writable_len(clist_ctl, &remain_len, &nr_burst);

	if(len % clist_ctl->object_size){	/* エラー処理 */
		/* オブジェクトサイズの整数倍じゃなかったらエラー */
		return -EINVAL;
	}

	if(len >= write_scope){
#ifdef DEBUG
		printk(KERN_INFO "clist_push() len:%ld, write_scope:%d, remain_len:%d, nr_burst:%d\n", len, write_scope, remain_len, nr_burst);
#endif

		/* 現在のノードに書き込めるだけ書き込む */
		if(remain_len > 0){
			clist_wmemcpy(data, remain_len, clist_ctl);
			ret += remain_len;
		}

		if(clist_ctl->w_curr == clist_ctl->r_curr){
#ifdef DEBUG
				printk(KERN_INFO "clist_push() w_curr == r_curr. alloc more memory or retry. read_wait_length:%d\n", clist_ctl->read_wait_length);
#endif	
			return remain_len;
		}

		/* ノード単位で書き込む */
		for(i = 0; i < nr_burst; i++){
			clist_wmemcpy(data + ret, clist_ctl->node_len, clist_ctl);
			ret += clist_ctl->node_len;
		}
	}
	else{	/* len < write_scope */

		if(len >= remain_len){		/* 現在のノードに書き込めるだけ書き込む */

			/* nr_burstを再計算 */
			nr_burst = (len - remain_len) / clist_ctl->node_len;
#ifdef DEBUG
			printk(KERN_INFO "clist_push() len:%ld, write_scope:%d, remain_len:%d, nr_burst:%d\n", len, write_scope, remain_len, nr_burst);
#endif

			if(remain_len > 0){
				clist_wmemcpy(data, remain_len, clist_ctl);
				ret += remain_len;
			}

			if(clist_ctl->w_curr == clist_ctl->r_curr){
#ifdef DEBUG
				printk(KERN_INFO "clist_push() w_curr == r_curr. alloc more memory or retry. read_wait_length:%d\n", clist_ctl->read_wait_length);
#endif	
				return remain_len;
			}

			/* ノード単位で書き込む */
			for(i = 0; i < nr_burst; i++){
				clist_wmemcpy(data + ret, clist_ctl->node_len, clist_ctl);
				ret += clist_ctl->node_len;
			}

			/* 最後に残った半端なものを書き込む */
			if(len - ret > 0){
				clist_wmemcpy(data + ret, len - ret, clist_ctl);
				ret += len - ret;
			}
		}
		else{	/* len < remain_len */
			/* lenだけ書き込む */
			clist_wmemcpy(data + ret, len, clist_ctl);
			ret += len;
		}
	}

#ifdef DEBUG
	printk(KERN_INFO "len:%lu ret:%ld\n", len, ret);
#endif

	return ret;
}
EXPORT_SYMBOL(clist_push);

/*
	循環リストからノードを一つ取り除く関数
	@data データを格納するアドレス
	@len データの長さ
	return dataに格納したデータサイズ

	※書き込みが完了したノードしか読まない仕様
*/
int clist_pull(void *data, size_t len, struct clist_controler *clist_ctl)
{
	int i, first_len = 0, nr_burst = 0;
	size_t ret = 0, read_scope;

	/* 読める最大サイズを計算する */
	read_scope = clist_readable_len(clist_ctl, &first_len, &nr_burst);

#ifdef DEBUG
	printk(KERN_INFO "pick_node() read_scope:%lu, len:%lu\n", read_scope, len);
#endif

	if(len % clist_ctl->object_size){	/* エラー処理 */
		/* オブジェクトサイズの整数倍じゃなかったらエラー */
		return -EINVAL;
	}

	if(len >= read_scope){	/* 読める上限（read_scope）だけ読む */

		if(first_len){
			clist_rmemcpy(data, first_len, clist_ctl);
			ret += first_len;
		}

		/* ノード単位で読む */
		if(nr_burst){

			for(i = 0; i < nr_burst; i++){
				clist_rmemcpy(data + ret, clist_ctl->node_len, clist_ctl);
				ret += clist_ctl->node_len;
			}
		}
	}
	else{	/* len < read_scope */

		if(len >= first_len){

			/* 読み残しを読む */
			if(first_len){
				clist_rmemcpy(data, first_len, clist_ctl);
				ret += first_len;
			}
#ifdef DEBUG
			printk(KERN_INFO "pick_node() loop number:%lu\n", (len - first_len) / clist_ctl->node_len);
#endif

			/* ノード単位で読む */
			for(i = 0; i < (len - first_len) / clist_ctl->node_len; i++){
				clist_rmemcpy(data + ret, clist_ctl->node_len, clist_ctl);
				ret += clist_ctl->node_len;
			}
#ifdef DEBUG
			printk(KERN_INFO "pick_node() odd number:%lu\n", (len - first_len) % clist_ctl->node_len);
#endif

			/* 半端な長さのものを読む */
			if(((len - first_len) % clist_ctl->node_len) > 0){
				clist_rmemcpy(data + ret, (len - first_len) % clist_ctl->node_len, clist_ctl);
				ret += (len - first_len) % clist_ctl->node_len;
			}
		}
		else{	/* len < first_len */
			clist_rmemcpy(data, len, clist_ctl);
			ret += len;
		}
	}

	return ret;
}
EXPORT_SYMBOL(clist_pull);

/*
	循環リストからw_currのノードからデータを読む関数
	@data データを格納するアドレス
	@len データの長さ
	return dataに格納したデータサイズ

	※最後に呼び出される関数
*/
int clist_pull_end(void *data, int len, struct clist_controler *clist_ctl)
{
	if(clist_ctl->state == CLIST_STATE_HOT){
		return -ECANCELED;
	}

	memcpy(data, clist_ctl->w_curr->data, len);
	clist_ctl->w_curr->curr_ptr -= len;

	return len;	
}
EXPORT_SYMBOL(clist_pull_end);



