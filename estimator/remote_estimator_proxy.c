#include "remote_estimator_proxy.h"

#define DEFAULT_PROXY_INTERVAL_TIME 100
#define BACK_WINDOWS_MS 500

estimator_proxy_t* estimator_proxy_create(size_t packet_size, uint32_t ssrc)
{
	estimator_proxy_t* proxy = calloc(1, sizeof(estimator_proxy_t));
	proxy->header_size = packet_size;
	proxy->ssrc = ssrc;
	proxy->hb_ts = -1;
	proxy->wnd_start_seq = -1;
	proxy->max_arrival_seq = -1;
	proxy->send_interval_ms = DEFAULT_PROXY_INTERVAL_TIME;

	proxy->arrival_times = skiplist_create(id64_compare, NULL, NULL);

	init_unwrapper16(&proxy->unwrapper);
}

void estimator_proxy_destroy(estimator_proxy_t* proxy)
{
	if (proxy == NULL)
		return;

	if (proxy->arrival_times != NULL){
		skiplist_destroy(proxy->arrival_times);
		proxy->arrival_times = NULL;
	}

	free(proxy);
}

#define MAX_IDS_NUM 1000
void estimator_proxy_incoming(estimator_proxy_t* proxy, int64_t arrival_ts, uint32_t ssrc, uint16_t seq)
{
	int64_t sequence, ids[MAX_IDS_NUM];

	skiplist_iter_t* iter;
	skiplist_item_t key, val;
	int num, i;

	if (arrival_ts < 0)
		return;

	proxy->ssrc = ssrc;
	sequence = wrap_uint16(&proxy->unwrapper, seq);

	if (sequence > proxy->wnd_start_seq + 32767)
		return;

	if (proxy->max_arrival_seq <= proxy->wnd_start_seq){
		num = 0;
		/*删除过期的到达时间统计，因为UDP会乱序，这里只会删除时间超过500毫秒且是当前报文之前的报文的记录*/
		SKIPLIST_FOREACH(proxy->arrival_times, iter){
			if (iter->key.i64 < sequence && arrival_ts >= iter->val.i64 + BACK_WINDOWS_MS && num < MAX_IDS_NUM)
				ids[num++] = iter->key.i64;
		}

		for (i = 0; i < num; ++i){
			key.i64 = sequence;
			skiplist_remove(proxy->arrival_times, key);
		}
	}

	if (proxy->wnd_start_seq == -1)
		proxy->wnd_start_seq = seq;
	else if (sequence < proxy->wnd_start_seq)
		proxy->wnd_start_seq = sequence;

	/*保存接收到的最大sequence*/
	proxy->max_arrival_seq = SU_MAX(proxy->max_arrival_seq, sequence);

	key.i64 = sequence;
	val.i64 = arrival_ts;
	skiplist_insert(proxy->arrival_times, key, val);
}

#define MAX_FEELBACK_COUNT 200
static int proxy_bulid_feelback_packet(estimator_proxy_t* proxy, bin_stream_t* strm)
{
	uint16_t size, i, detla;
	skiplist_iter_t* iter;

	int64_t min_ts = 0xffffffffffffffff;

	size = skiplist_size(proxy->arrival_times);
	if (proxy->max_arrival_seq <= proxy->wnd_start_seq && size == 0)
		return -1;
	
	/*feelback信息进行打包*/
	mach_uint32_write(strm, proxy->ssrc);						/*ssrc*/
	mach_uint16_write(strm, proxy->wnd_start_seq & 0xffff);		/*base*/
	size = SU_MIN(size, MAX_FEELBACK_COUNT);
	mach_uint16_write(strm, size);

	/*找到最早到达的报文时间戳*/
	SKIPLIST_FOREACH(proxy->arrival_times, iter){
		if (min_ts > iter->val.i64)
			min_ts = iter->val.i64;
	}

	i = 0;
	SKIPLIST_FOREACH(proxy->arrival_times, iter){
		if (++i >= size)
			break;

		/*将序号和时间戳打包到网络报文中发送给发送方*/
		mach_uint16_write(strm, iter->key.i64 & 0xffff);
		detla = iter->val.i64 - min_ts;
		mach_uint16_write(strm, detla);

		/*更新下一个feelback的起始位置*/
		proxy->wnd_start_seq = iter->key.i64 + 1;
	}

	return 0;
}

int estimator_proxy_heartbeat(estimator_proxy_t* proxy, int64_t cur_ts, bin_stream_t* strm)
{
	if (cur_ts >= proxy->hb_ts + proxy->send_interval_ms){
		proxy->hb_ts = cur_ts;
		return proxy_bulid_feelback_packet(proxy, strm);
	}
	return -1;
}

#define kMaxSendIntervalMs 250
#define kMinSendIntervalMs 50

/*码率发生变化时重新评估发送间隔时间*/
void estimator_proxy_bitrate_changed(estimator_proxy_t* proxy, uint32_t bitrate)
{
	double rate = bitrate * 0.05;
	proxy->send_interval_ms = (proxy->header_size * 8.0 * 1000.0) / rate + 0.5;
	proxy->send_interval_ms = SU_MAX(SU_MIN(proxy->send_interval_ms, kMaxSendIntervalMs), kMinSendIntervalMs);

}








