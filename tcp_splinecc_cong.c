#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <net/tcp.h>

#define SPLINE_SCALE 10
#define epoch_round 4
#define SCALE_BW_RTT 4
#define BW_SCALE 12

// Минимальные значения для предотвращения деления на ноль
#define MIN_RTT_US 50000  // 1 мс
#define MIN_BW 1448    // 1 сегмент

struct scc {
    u32 curr_cwnd;          /* Current congestion window (segments) */
    u32 throughput;         /* Throughput from in_flight */
    u64 bw;                 /* Throughput from ACK */
    u32 last_max_cwnd;      /* Maximum window */
    u32 last_min_rtt_stamp;
    u32 last_bw;            /* Cached bandwidth */
    u32 last_min_rtt;       /* Minimum RTT */
    u32 last_ack;           /* Last acknowledged segments */
    u16 prev_ca_state:3;     /* Исправлено: добавлен тип u8 */
    u32 LastAckedSacked;    /* ACK and SACK in bytes */
    u32 mss;                /* MSS */
    u32 prior_cwnd;
    u32 min_cwnd;           /* Minimum window */
    u32 curr_rtt;           /* Current RTT (microseconds) */
    u64 pacing_rate;
    u32 cwnd_gain;          /* Gain for cwnd in BW/RTT modes */
    u32 max_could_cwnd;     /* Max cwnd for bandwidth/loss balance */
    u32 curr_ack;           /* Current acknowledged segments */
    u32 fairness_rat;       /* Fairness coefficient */
    u8 current_mode;        /* Current mode (START_PROBE, etc.) */
    u8 epp_min_rtt;         /* Counter for new min RTT */
    u8 epp;                 /* Epoch cycle (max 10) */
    u32 delivered;    /* Theoretical delivered segments */
    u32 bytesInFlight;
};

// Глобальный спинлок для защиты доступа к struct scc
static DEFINE_SPINLOCK(spline_cc_lock);

static const u32 scc_min_rtt_win_sec = 10;
static const u32 scc_min_allowed_cwnd = 1;
static const u32 scc_min_segment_size = 1440;
static const u32 scc_min_snd_cwnd = scc_min_segment_size * scc_min_allowed_cwnd;
static const u32 scc_min_rtt_allowed_us = 100000;

enum spline_cc_mode {
    MODE_START_PROBE,
    MODE_PROBE_BW,
    MODE_PROBE_RTT,
    MODE_DRAIN_PROBE
};

/* Forward declarations */
static void update_bytesInFlight(struct sock *sk);
static void update_LastAckedSacked_cwnd_mss(struct sock *sk, const struct rate_sample *rs);

static void stable_rtt_BW(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);

    if (scc->fairness_rat >= 2 << BW_SCALE || (scc->bytesInFlight << 1) < scc->curr_cwnd) {
        scc->curr_cwnd = (scc->curr_cwnd * 18) >> SCALE_BW_RTT;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);

        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }
}

static void fairness_rtt_BW(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);

    if (scc->fairness_rat < 2 << BW_SCALE) {
        scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * SPLINE_SCALE >> SCALE_BW_RTT) >> BW_SCALE;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }

}

static void overload_rtt_BW(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);

    if (scc->prev_ca_state == TCP_CA_Loss && scc->bytesInFlight > scc->curr_cwnd) {
        scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * SPLINE_SCALE >> SCALE_BW_RTT) >> BW_SCALE;
        if (scc->curr_ack < (scc->last_ack * 3) >> 2)
            scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * SPLINE_SCALE >> SCALE_BW_RTT) >> BW_SCALE;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }
}

static void prob_bw(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    printk(KERN_DEBUG "prob_bw: scc->curr_cwnd=%u\n",
        scc->curr_cwnd);
    stable_rtt_BW(sk);
    fairness_rtt_BW(sk);
    overload_rtt_BW(sk);
    printk(KERN_DEBUG "prob_bw: scc->curr_cwnd=%u\n",
           scc->curr_cwnd);
}

static void stable_rtt(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat >= 2 || (scc->bytesInFlight << 1) < scc->min_cwnd) {
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }
}

static void fairness_rtt(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat < 2) {
        scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * 8 >> SCALE_BW_RTT) >> BW_SCALE;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }
}

static void overload_rtt(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->prev_ca_state == TCP_CA_Loss && scc->bytesInFlight > scc->curr_cwnd) {
        scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * 8 >> SCALE_BW_RTT) >> BW_SCALE;
        if (scc->curr_ack << SCALE_BW_RTT < ((scc->last_ack << SCALE_BW_RTT) * 3) >> 2)
            scc->curr_cwnd = ((scc->curr_cwnd << BW_SCALE) * SPLINE_SCALE >> SCALE_BW_RTT) >> BW_SCALE;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    }
}

static void prob_rtt(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    stable_rtt(sk);
    fairness_rtt(sk);
    overload_rtt(sk);

    printk(KERN_DEBUG "prob_rtt: scc->curr_cwnd=%u\n",
           scc->curr_cwnd);
}

static void update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
    struct scc *scc = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    bool new_min_rtt = after(tcp_jiffies32, scc->last_min_rtt_stamp + scc_min_rtt_win_sec * HZ);

    if (tp->srtt_us)
        scc->curr_rtt = tp->srtt_us >> 3;
    else
        scc->curr_rtt = scc_min_rtt_allowed_us;

    if (scc->curr_rtt < scc->last_min_rtt || scc->last_min_rtt == 0)
        printk(KERN_DEBUG "update_min_rtt: why fucking last_min_rtt = 23?????%u\n", scc->last_min_rtt);
        scc->last_min_rtt = scc->curr_rtt;

    if (rs && rs->rtt_us > 0 && (rs->rtt_us < scc->last_min_rtt || (new_min_rtt && !rs->is_ack_delayed))) {
        scc->last_min_rtt = rs->rtt_us;
        scc->last_min_rtt_stamp = tp->srtt_us ? tp->srtt_us : tcp_jiffies32;
    }
    if (scc->last_min_rtt == 0) { // Дополнительная защита
        scc->last_min_rtt = scc_min_rtt_allowed_us;
        printk(KERN_DEBUG "update_min_rtt: last_min_rtt was 0, set to %u\n", scc->last_min_rtt);
    }
    scc->epp_min_rtt++;
    printk(KERN_DEBUG "update_min_rtt: last_min_rtt=%u, curr_rtt=%u, rs->rtt_us=%lld\n",
           scc->last_min_rtt, scc->curr_rtt, rs ? rs->rtt_us : -1);
}


static void bw__tp(struct sock *sk, const struct rate_sample *rs)
{
    struct scc *scc = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 gamma;
    u32 beta;

    if (scc->last_min_rtt == 0) {
        scc->last_min_rtt = scc_min_rtt_allowed_us;
        printk(KERN_DEBUG "bw__tp: last_min_rtt was 0, set to %u\n", scc->last_min_rtt);
    }

    printk(KERN_DEBUG "bw__tp: before calc: last_min_rtt=%u, bytesInFlight=%u, scc->curr_cwnd=&u\n",
           scc->last_min_rtt, scc->bytesInFlight, scc->curr_cwnd);

    if (!tcp_packets_in_flight(tp))
        scc->throughput = 0;

    else
        scc->throughput = div_u64((u64)(scc->bytesInFlight * USEC_PER_SEC) << BW_SCALE, scc->last_min_rtt);


    scc->bw = div_u64((u64)((scc->curr_ack + scc->LastAckedSacked * scc->mss) * USEC_PER_SEC) << BW_SCALE, scc->curr_rtt);
    gamma = scc->bw;
    if(!tcp_packets_in_flight(tp))
        beta = scc->bw >> 4;
    else
        beta = scc->throughput;

    if (beta == 0)
        scc->fairness_rat = 1;
    else
        scc->fairness_rat = (gamma / beta) + 1;

    if ((scc->throughput * 14) >> SCALE_BW_RTT > scc->bw)
        scc->current_mode = MODE_DRAIN_PROBE;

    if (scc->last_bw != 0) {
        u64 min_allowed = (scc->last_bw * 3) >> 2;

        if (scc->bw < min_allowed)
            scc->bw = min_allowed;

        u64 max_allowed = (scc->last_bw * 6) >> 2;
        if (scc->bw > max_allowed)
            scc->bw = max_allowed;

        if (scc->curr_rtt > (scc->last_min_rtt << 1))
            scc->bw = scc->last_bw;
    }
    scc->bw = scc->bw >> BW_SCALE;
    scc->last_bw = scc->bw;
    scc->throughput =scc->throughput >> BW_SCALE;
    scc->bw = max(scc->bw, (u64)MIN_BW);

    printk(KERN_DEBUG "bw__tp: fairness_rat=%u, bw=%llu, last_min_rtt=%u, throughput=%llu, bytesInFlight=%u, curr_cwnd=%u, LastAckedSacked=%u\n",
           scc->fairness_rat, scc->bw, scc->last_min_rtt, scc->throughput, scc->bytesInFlight, scc->curr_cwnd, scc->LastAckedSacked);
}

static void reset_bw__tp(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    scc->bw = 0;
    scc->throughput = 0;
    scc->fairness_rat = 0;
}

static bool Is_Loss(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    return (scc->prev_ca_state == TCP_CA_Loss && scc->curr_ack < scc->last_ack);
}

static void spline_max_cwnd(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    u64 rtt = scc->last_min_rtt ? scc->last_min_rtt : scc_min_rtt_allowed_us;
    if (rtt == 0) {
        rtt = scc_min_rtt_allowed_us;
    }

    scc->max_could_cwnd = ((scc->fairness_rat * (scc->bw - (scc->bw * 15 >> 4)) * USEC_PER_SEC) / rtt) >> BW_SCALE;
    scc->max_could_cwnd = scc->max_could_cwnd ? scc->max_could_cwnd : scc->min_cwnd;
}

static void drain_probe(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->curr_cwnd > scc->bw)
        scc->curr_cwnd = scc->bw;

    scc->curr_cwnd = (scc->curr_cwnd * 12) >> SCALE_BW_RTT;
    scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
    printk(KERN_DEBUG "drain_probe: after curr_cwnd=%u\n", scc->curr_cwnd); // FIX: отладочное сообщение
}

static void start_probe(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    scc->curr_cwnd += scc->curr_ack << 1;

    if (Is_Loss(sk) || scc->curr_cwnd > scc->bytesInFlight)
        scc->curr_cwnd = min(scc->curr_cwnd, scc->max_could_cwnd);
    else
        scc->curr_cwnd = max(scc->curr_cwnd, scc->max_could_cwnd);
    printk(KERN_DEBUG "start_probe: after curr_cwnd=%u\n", scc->curr_cwnd);
}

static void check_start_prob(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (!scc->current_mode)
        scc->current_mode = MODE_START_PROBE;
}

static void check_drain_prob(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->bytesInFlight > scc->curr_ack && Is_Loss(sk))
        scc->current_mode = MODE_DRAIN_PROBE;
    else
        scc->current_mode = MODE_PROBE_BW;
}

static void check_epoch_probs_rtt_bw(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    if (scc->epp_min_rtt) {
        scc->epp_min_rtt = 0;
        scc->current_mode = MODE_PROBE_BW;
    } else {
        scc->current_mode = MODE_PROBE_RTT;
    }
}

static void check_probs(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    check_start_prob(sk);
    check_drain_prob(sk);

    if (scc->epp == epoch_round) {
        scc->epp = 0;
        check_epoch_probs_rtt_bw(sk);
    }
}

static u32 spline_cwnd_gain(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    u64 rtt = scc->last_min_rtt ? scc->last_min_rtt : MIN_RTT_US;
    u64 denom = (scc->bw * USEC_PER_SEC) / rtt;

    if (denom == 0)
        denom = MIN_BW;

    return div_u64((u64)scc->curr_cwnd << (BW_SCALE + BW_SCALE), denom);
}

static u32 spline_cwnd_next_gain(struct sock *sk, const struct rate_sample *rs)
{
    struct scc *scc = inet_csk_ca(sk);
    spline_max_cwnd(sk);
    scc->cwnd_gain = spline_cwnd_gain(sk);

    u64 denom = scc->last_min_rtt ? scc->last_min_rtt : MIN_RTT_US;
    scc->curr_cwnd = div_u64(scc->cwnd_gain * scc->bw * USEC_PER_SEC, denom);
    scc->curr_cwnd >>= BW_SCALE << 1;

    if (Is_Loss(sk))
        scc->curr_cwnd = min(scc->curr_cwnd, scc->max_could_cwnd);
    else
        scc->curr_cwnd = max(scc->curr_cwnd, scc->max_could_cwnd);

    return scc->curr_cwnd;
}

static void spline_cc_save_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);

    if (scc->prev_ca_state < TCP_CA_Recovery && scc->current_mode != MODE_PROBE_RTT)
        scc->prior_cwnd = tcp_snd_cwnd(tp);
    else
        scc->prior_cwnd = max(scc->prior_cwnd, scc_min_snd_cwnd);
}

static void spline_cc_check_main(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    scc->curr_cwnd = scc->curr_cwnd ? scc->curr_cwnd : scc_min_segment_size;
    scc->delivered = scc->delivered ? scc->delivered : scc_min_segment_size;
    scc->mss = scc->mss ? scc->mss : scc_min_segment_size;
}

static void probs(struct sock *sk, const struct rate_sample *rs)
{
    struct scc *scc = inet_csk_ca(sk);
    check_probs(sk);
    switch (scc->current_mode) {
    case MODE_START_PROBE:
        start_probe(sk);
        break;
    case MODE_PROBE_BW:
        prob_bw(sk);
        spline_cwnd_next_gain(sk, rs);
        break;
    case MODE_PROBE_RTT:
        prob_rtt(sk);
        spline_cwnd_next_gain(sk, rs);
        break;
    case MODE_DRAIN_PROBE:
        drain_probe(sk);
        break;
    default:
        prob_bw(sk);
        spline_cwnd_next_gain(sk, rs);
    }
}

static void update_bytesInFlight(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);
    scc->bytesInFlight = tcp_packets_in_flight(tp) * scc->mss;
}

static void update_LastAckedSacked_cwnd_mss(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);
    if (scc->mss == 0) {
        scc->mss = scc_min_segment_size;
    }
    scc->delivered = tp->delivered;
    scc->last_ack = scc->curr_ack;
    if (!rs) { // FIX: безопасные значения по умолчанию для curr_ack и LastAckedSacked
        scc->curr_ack = 0;
        scc->LastAckedSacked = 0;
    } else {
        scc->curr_ack = rs->delivered;
        scc->LastAckedSacked = rs->acked_sacked;
    }
    scc->min_cwnd = scc_min_snd_cwnd;
    scc->mss = scc->mss ? scc->mss : scc_min_segment_size;

    spline_cc_check_main(sk);
    scc->curr_cwnd = tcp_snd_cwnd(tp) * scc->mss;

    if (scc->curr_cwnd > scc->last_max_cwnd)
        scc->last_max_cwnd = scc->curr_cwnd;
}

static void spline_cc_update(struct sock *sk, const struct rate_sample *rs)
{
    update_min_rtt(sk, rs);
    update_bytesInFlight(sk);
    update_LastAckedSacked_cwnd_mss(sk, rs);
    bw__tp(sk, rs);
    probs(sk, rs);
}

static void spline_cwnd_send(struct sock *sk, const struct rate_sample *rs)
{
    struct scc *scc = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    u32 cwnd;
    spline_cc_update(sk, rs);
    printk(KERN_DEBUG "spline_cwnd_send: curr_cwnd=%u, mss=%u after update\n", scc->curr_cwnd, scc->mss); // FIX: отладочное сообщение

    if (scc->mss == 0) { // Защита от деления на ноль
        scc->mss = scc_min_segment_size;
    }
    cwnd = scc->curr_cwnd / scc->mss;
    printk(KERN_DEBUG "spline_cwnd_send: final cwnd=%u\n", cwnd); // FIX: отладочное сообщение

    tcp_snd_cwnd_set(tp, min(cwnd, tp->snd_cwnd_clamp));
}

static void spline_cc_main(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);

    scc->mss = tp->mss_cache;
    scc->curr_cwnd = tcp_snd_cwnd(tp) * tp->mss_cache;
    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
    scc->delivered = tp->delivered;
    scc->min_cwnd = scc_min_snd_cwnd;
    scc->prev_ca_state = TCP_CA_Open;
    scc->current_mode = MODE_START_PROBE;
    scc->last_min_rtt_stamp = tcp_jiffies32;

    spline_cwnd_send(sk, rs);
}

static u32 spline_cc_undo_cwnd(struct sock *sk)
{
    struct scc *scc = inet_csk_ca(sk);
    scc->curr_cwnd = tcp_snd_cwnd(tcp_sk(sk)) * scc_min_segment_size;
    reset_bw__tp(sk);
    return tcp_snd_cwnd(tcp_sk(sk));
}

static void spline_cc_set_state(struct sock *sk, u8 new_state)
{
    struct scc *scc = inet_csk_ca(sk);
    if (new_state == TCP_CA_Loss) {
        scc->prev_ca_state = TCP_CA_Loss;
        struct rate_sample rs = { .losses = 1 };

        bw__tp(sk, &rs);
    } else {
        scc->prev_ca_state = TCP_CA_Open;
    }
    printk(KERN_DEBUG "spline_cc_set_state: sk=%p\n", sk);
}


static void spline_cc_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);
    scc->last_max_cwnd = 0;
    printk(KERN_DEBUG "spline_cc_init: why fucking last_min_rtt = 23?????%u\n", scc->last_min_rtt);
    scc->last_min_rtt = 0;
    scc->bw = 0;
    scc->last_bw = 0;
    scc->throughput = 0;
    scc->curr_rtt = 0;
    scc->curr_ack = 0;
    scc->last_ack = 0;
    scc->fairness_rat = 0;
    scc->prior_cwnd = 0;
    scc->epp = 0;
    scc->epp_min_rtt = 0;
    scc->pacing_rate = 0;
    scc->bytesInFlight = 0;
    scc->max_could_cwnd = 0;
    scc->cwnd_gain = 0;
}

static u32 spline_cc_ssthresh(struct sock *sk)
{
    spline_cc_save_cwnd(sk);
    return tcp_sk(sk)->snd_ssthresh;
}

static u32 spline_cc_sndbuf_expand(struct sock *sk)
{
    return 3;
}

static void spline_cc_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct scc *scc = inet_csk_ca(sk);
    if (event == CA_EVENT_CWND_RESTART || event == CA_EVENT_TX_START) {
        scc->prev_ca_state = TCP_CA_Open; 
        scc->curr_cwnd = scc_min_snd_cwnd;
        scc->current_mode = MODE_START_PROBE;

        if (scc->mss == 0) {
            scc->mss = scc_min_segment_size;
        }
    }
}
static struct tcp_congestion_ops spline_cc_ops __read_mostly = {
    .init           = spline_cc_init,
    .ssthresh       = spline_cc_ssthresh,
    .cong_control   = spline_cc_main,
    .sndbuf_expand  = spline_cc_sndbuf_expand,
    .cwnd_event     = spline_cc_cwnd_event,
    .undo_cwnd      = spline_cc_undo_cwnd,
    .set_state      = spline_cc_set_state,
    .owner          = THIS_MODULE,
    .name           = "spline_cc",
};

static int __init spline_cc_register(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct scc) > ICSK_CA_PRIV_SIZE);

    ret = tcp_register_congestion_control(&spline_cc_ops);
    if (ret < 0) {
        printk(KERN_ERR "spline_cc: registration failed with error %d\n", ret);
        return ret;
    }

    printk(KERN_INFO "spline_cc: successfully registered as '%s'\n", spline_cc_ops.name);
    return 0;
}

static void __exit spline_cc_unregister(void)
{
    tcp_unregister_congestion_control(&spline_cc_ops);
}

module_init(spline_cc_register);
module_exit(spline_cc_unregister);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bekzhan Kalimollayev");
MODULE_DESCRIPTION("Spline Congestion Control for Linux Kernel");