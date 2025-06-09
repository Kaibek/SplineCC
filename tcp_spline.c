#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <net/tcp.h>

#define SPLINE_SCALE 10
#define eppoch_round 9
#define SCALE_BW_RTT 4    

struct scc {
    u32 curr_cwnd;          // Текущее окно перегрузки (в сегментах)
    u64 throughput;         // относительная пропускная способность от in_flight
    u64 bw;                 // Пропускная способность от ack
    u32 last_max_cwnd;      // Максимальное окно
    u32 last_min_rtt_stamp;
    u64 last_bw;            // кэшированное bw
    u32 last_min_rtt;       // Минимальный RTT
    u32 last_ack;           // Последние подтверждённые сегменты
    u32 LastAckedSacked;    // ACK и SACK в байтах
    u32 mss;                // MSS
    u32 min_cwnd;           // Минимальное окно
    u32 curr_rtt;           // Текущий RTT (в микросекундах)
    u64 pacing_rate;
    u64 cwnd_gain;          // усилиение к cwnd для режимов BW и RTT
    u32 max_could_cwnd;     // максимальное допустимое cwnd в рамках "жонглирование" между пропускной способности и потерями
    u32 curr_ack;           // Текущие подтверждённые сегменты
    u32 fairness_rat;       // Коэффициент справедливости
    u8  current_mode;       // Текущий режим (START_PROBE, DRAIN_PROBE и т.д.)
    u8  epp_min_rtt;        // счетчик новых минимальных rtt
    u8  epp;                // круговой оборот эпохи (макс 10)
    u64 could_delivered;    // теоретическое доставленных сегментов
    u32 bytesInFlight;      
};

static const u32 scc_min_rtt_win_sec = 10;
static const u32 scc_min_snd_cwnd = 4;
static const u32 scc_min_allowed_cwnd = 4;
static const u32 scc_min_segment_size = 1448;
static const u32 scc_min_rtt_allowed_us = 150000;

enum spline_cc_mode {
    MODE_START_PROBE,
    MODE_PROBE_BW,
    MODE_PROBE_RTT,
    MODE_DRAIN_PROBE
};

static u32 stable_rtt_BW(struct sock *sk)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat >= 2 || (scc->bytesInFlight << 1) < scc->curr_cwnd)
    {
        scc->curr_cwnd = scc->curr_cwnd * 18 >> 4;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 fairness_rtt_BW(struct sock *sk)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat < 2)
    {
        scc->curr_cwnd = scc->curr_cwnd * 10 >> SCALE_BW_RTT;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 overload_rtt_BW(struct sock *sk, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (event == CA_EVENT_LOSS && scc->bytesInFlight > scc->curr_cwnd)
    {
        scc->curr_cwnd = scc->curr_cwnd * 10 >> SCALE_BW_RTT;
        if (scc->curr_ack < scc->last_ack * 3 >> 2)
        {
            scc->curr_cwnd = scc->curr_cwnd * 10 >> SCALE_BW_RTT;
        }
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 prob_bw(struct sock *sk, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    u32 stab = stable_rtt_BW(sk);
    if (stab) return stab;
    u32 fairness = fairness_rtt_BW(sk);
    if (fairness) return fairness;
    u32 over = overload_rtt_BW(sk, event);
    if (over) return over;
    return scc->curr_cwnd;
}

static u32 stable_rtt(struct sock *sk)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat >= 2 || (scc->bytesInFlight << 1) < scc->curr_cwnd)
    {
        scc->curr_cwnd = scc->curr_cwnd;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 fairness_rtt(struct sock *sk)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (scc->fairness_rat < 2)
    {
        scc->curr_cwnd = scc->curr_cwnd * 8 >> SCALE_BW_RTT;
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 overload_rtt(struct sock *sk, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    if (event == CA_EVENT_LOSS && scc->bytesInFlight > scc->curr_cwnd)
    {
        scc->curr_cwnd = scc->curr_cwnd * 8 >> SCALE_BW_RTT;
        if (scc->curr_ack < scc->last_ack * 3 >> 2)
        {
            scc->curr_cwnd = scc->curr_cwnd * 10 >> SCALE_BW_RTT; // Переводим в сегменты
        }
        scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
        return scc->curr_cwnd;
    }
    return 0;
}

static u32 prob_rtt(struct sock *sk, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_snd_cwnd;

    struct scc *scc = inet_csk_ca(sk);
    u32 stab = stable_rtt(sk);
    if (stab) return stab;
    u32 fairness = fairness_rtt(sk);
    if (fairness) return fairness;
    u32 over = overload_rtt(sk, event);
    if (over) return over;
    return scc->curr_cwnd;
}

static void update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    bool new_min_rtt = after(tcp_jiffies32, scc->last_min_rtt_stamp + scc_min_rtt_win_sec * HZ);

    // Обновляем curr_rtt из srtt_us (сглаженное RTT)
    if (tp->srtt_us)
        scc->curr_rtt = max(tp->srtt_us >> 3, 1U);  // srtt_us хранится в формате ×8

    // Обновляем last_min_rtt, если curr_rtt меньше предыдущего
    if (scc->curr_rtt < scc->last_min_rtt || scc->last_min_rtt == 0)
        scc->last_min_rtt = scc->curr_rtt;

    // Если пришёл свежий RTT от ACK, обновляем при необходимости
    if (rs->rtt_us > 0 && (rs->rtt_us < scc->last_min_rtt || (new_min_rtt && !rs->is_ack_delayed))) {
        scc->last_min_rtt = rs->rtt_us;
        scc->last_min_rtt_stamp = tcp_jiffies32;
        scc->epp_min_rtt++;
    scc->last_min_rtt = scc->last_min_rtt ? scc->last_min_rtt : scc_min_rtt_allowed_us;
    }
}


static void bw__tp(struct sock *sk, const struct rate_sample *rs)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    if (scc->last_min_rtt == 0) {
        scc->throughput = 0;
        scc->bw = scc->min_cwnd * scc->mss;
    }
    if(scc->last_bw == scc->bw) 
    {
        scc->bw = scc->bw;
        scc->throughput = scc->throughput;
    }
    else
    {
        scc->throughput = div_u64(scc->bytesInFlight * USEC_PER_SEC, scc->last_min_rtt);
        scc->bw = div_u64((u64)rs->delivered * USEC_PER_SEC, scc->last_min_rtt);
    }

    if (scc->bytesInFlight == 0)
        scc->fairness_rat = 2;
    else {
        u32 gamma = scc->curr_cwnd * scc->curr_cwnd;
        u32 beta = 2 * scc->bytesInFlight * scc->bytesInFlight;
        scc->fairness_rat = (((gamma << SCALE_BW_RTT) / beta) >> SCALE_BW_RTT) + 1;
    }
    if (scc->throughput * 12 >> SCALE_BW_RTT > scc->bw)
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
    scc->last_bw = scc->bw;
    scc->bw = max(scc->bw, scc->min_cwnd * scc->mss);
}

static void spline_cc_update(struct sock *sk, const struct rate_sample *rs)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    update_bytesInFlight(sk);
    update_LastAckedSacked_cwnd_mss(sk, rs);
    scc->bw = bw__tp(sk);
    update_min_rtt(sk, rs);
}

static bool Is_Loss(struct sock *sk, enum tcp_ca_event event)
{
    struct scc *scc = inet_csk_ca(sk);
    if (event == CA_EVENT_TX_START || event == CA_EVENT_TX_START && 
        scc->curr_ack < scc->last_ack)
        return true;
    else
        return false;
}

static void spline_max_cwnd(struct sock *sk)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);

    scc->max_could_cwnd = (scc->fairness_rat * (scc->bw - (scc->bw * 14 >> SCALE_BW_RTT)) * USEC_PER_SEC) / 
    (scc->last_min_rtt ? scc->last_min_rtt : USEC_PER_SEC);
    scc->max_could_cwnd = scc->max_could_cwnd ? scc->max_could_cwnd : scc->min_cwnd * scc->mss;
}

static void drain_probe(struct sock *sk)
{
    if (!sk)
        return;                                                                  
    struct scc *scc = inet_csk_ca(sk);
    if (scc->curr_cwnd > scc->bw) {
        scc->curr_cwnd = scc->bw;
    }
    scc->curr_cwnd = scc->curr_cwnd * 10 >> SCALE_BW_RTT;
    scc->curr_cwnd = max(scc->curr_cwnd, scc->min_cwnd);
}

static void start_probe(struct sock *sk, enum tcp_ca_event event) {

    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    scc->curr_cwnd += scc->curr_ack;

    if ((event == CA_EVENT_LOSS && scc->curr_ack < scc->last_ack) || scc->curr_cwnd > scc->bytesInFlight) {
        scc->curr_cwnd = min(scc->curr_cwnd, scc->max_could_cwnd);
    } else {
        scc->curr_cwnd = max(scc->curr_cwnd, scc->max_could_cwnd);
    }
}

static void check_start_prob(struct sock *sk)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    if (!scc->probe_mode)
        scc->current_mode = MODE_START_PROBE;
}

static void check_drain_prob(struct sock *sk)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    if ((scc->bytesInFlight > scc->curr_ack &&
            scc->bytesInFlight > scc->curr_cwnd) || scc->LastAckedSacked < scc->mss)
            scc->current_mode = MODE_DRAIN_PROBE;
    else
        scc->current_mode = MODE_PROBE_BW;
}

static void check_epoch_probs_rtt_bw(struct sock *sk)
{
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    if (scc->epp_min_rtt) {
        scc->epp_min_rtt = 0;
        scc->current_mode = MODE_PROBE_BW;
        }
    else
        scc->current_mode = MODE_PROBE_RTT;
}

static void check_probs(struct sock *sk)
{
    if (!sk)
        return;
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
    if (!sk)
        return scc_min_allowed_cwnd;
    struct scc *scc = inet_csk_ca(sk);
    u32 cwnd_gain_result = div_u64(scc->curr_cwnd << SPLINE_SCALE, (scc->bw * USEC_PER_SEC) / scc->last_min_rtt);
    return cwnd_gain_result;
}

static u32 spline_cwnd_next_gain(struct sock *sk, const struct rate_sample *rs, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_allowed_cwnd;
    struct scc *scc = inet_csk_ca(sk);
    spline_max_cwnd(sk);
        if (scc->mss == 0) 
            return scc->curr_cwnd ? scc->curr_cwnd : scc_min_snd_cwnd;
        
        scc->cwnd_gain = cwnd_gain(sk);
        scc->curr_cwnd = div_u64(scc->cwnd_gain * scc->bw * USEC_PER_SEC, scc->last_min_rtt);
        scc->curr_cwnd = scc->curr_cwnd >> SPLINE_SCALE;
        if (Is_Loss(sk, event))
            scc->curr_cwnd = min(scc->curr_cwnd, scc->max_could_cwnd);
        else
            scc->curr_cwnd = max(scc->curr_cwnd, scc->max_could_cwnd);

        return scc->curr_cwnd;
}


static void spline_pacing_rate(struct sock *sk, const struct rate_sample *rs) {
    if (!sk)
        return;
    struct scc *scc = inet_csk_ca(sk);
    u64 pacing_rate, max_cwnd;

    u64 pacing_rate = div_u64(scc->bw * scc->fairness_rat * scc->mss * USEC_PER_SEC,
                             scc->last_min_rtt ? scc->last_min_rtt : USEC_PER_SEC);

    if (scc->current_mode == MODE_PROBE_RTT) {
        pacing_rate = pacing_rate * 12 >> SCALE_BW_RTT;
    }
    max_cwnd = scc->max_could_cwnd * scc->fairness_rat;
    scc->pacing_rate = max(pacing_rate, max_cwnd);
    sk->sk_pacing_rate = pacing_rate;
}

static u32 probs(struct sock *sk, const struct rate_sample *rs, enum tcp_ca_event event)
{
    if (!sk)
        return scc_min_allowed_cwnd;
    struct scc *scc = inet_csk_ca(sk);
    check_probs(sk);

    switch (scc->current_mode)
    {
    case MODE_START_PROBE:
        return start_probe(sk, event);
    case MODE_PROBE_BW:
        prob_bw(sk);
        spline_pacing_rate(sk, rs);
        return spline_cwnd_next_gain(sk, rs, event);
    case MODE_PROBE_RTT:
        prob_rtt(sk);
        spline_pacing_rate(sk, rs);
        return spline_cwnd_next_gain(sk, rs, event);
    case MODE_DRAIN_PROBE:
        return drain_probe(sk);
    default:
        prob_bw(sk);
        spline_pacing_rate(sk, rs);
        return spline_cwnd_next_gain(sk, rs, event);
    }
}

static void spline_cwnd_send(struct sock *sk, const struct rate_sample *rs, enum tcp_ca_event event)
{
    if (!sk)
        return;
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);
    check_probs(sk);
    probs(sk, rs, event);
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

    scc->LastAckedSacked = rs->acked_sacked * scc->mss;
    scc->mss = scc->mss ? scc->mss : scc_min_segment_size;
    scc->curr_cwnd = tcp_snd_cwnd(tp) * scc->mss;
    scc->min_cwnd = tcp_init_cwnd(tp, sk);
    scc->min_cwnd = max(scc_min_allowed_cwnd * scc->mss, scc->min_cwnd)

    if (scc->curr_cwnd > scc->last_max_cwnd) {
        scc->last_max_cwnd = scc->curr_cwnd;
    }
}

// Увеличение окна перегрузки
static void spline_cc_main(struct sock *sk, const struct rate_sample *rs, enum tcp_ca_event event) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);
    spline_cc_update(sk, rs);
    spline_cwnd_send(sk, rs, event);
    tcp_snd_cwnd_set(tp, max(scc->min_cwnd, scc->curr_cwnd / scc->mss));
}

// Инициализация состояния
static void spline_cc_init(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct scc *scc = inet_csk_ca(sk);

    scc->curr_cwnd = tcp_init_cwnd(tp, sk);
    scc->mss = tcp_mss_to_bytes(tp);
    scc->could_delivered = tp->delivered;
    scc->min_cwnd = 0;
    scc->last_max_cwnd = 0;
    scc->bw = 0;
    scc->last_bw = 0;
    scc->throughput = 0;
    scc->curr_rtt = 0;
    scc->last_min_rtt = tcp_min_rtt(tp);
    scc->curr_ack = 0;
    scc->last_ack = 0;
    scc->fairness_rat = 2;
    scc->current_mode = MODE_START_PROBE;
    scc->probe_mode = MODE_START_PROBE;
    scc->epp = 0;
    scc->epp_min_rtt = 0;
    scc->last_min_rtt_stamp = tcp_jiffies32;
    scc->pacing_rate = 0;
}

// Вычисление порога медленного старта
static u32 spline_cc_ssthresh(struct sock *sk) {
    struct scc *scc = inet_csk_ca(sk);
    return max(scc->curr_cwnd * 14 >> SCALE_BW_RTT, 1U); // Логика из GetSsThresh
}

// Обработка подтверждений
static void spline_cc_pkts_acked(struct sock *sk, const struct ack_sample *sample) {
    struct scc *scc = inet_csk_ca(sk);

    scc->last_ack = scc->curr_ack;
    scc->curr_ack = sample->pkts_acked;
}

// Обработка событий
static void spline_cc_cwnd_event(struct sock *sk, enum tcp_ca_event event) {
    struct scc *scc = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    switch (event) {
    case CA_EVENT_CWND_RESTART:
    case CA_EVENT_TX_START:
        scc->curr_cwnd = tcp_init_cwnd(tp, sk);
        scc->current_mode = MODE_START_PROBE;
        break;
    default:
        break;
    }
}

// Регистрация алгоритма
static struct tcp_congestion_ops spline_cc_ops = {
    .init           = spline_cc_init,
    .ssthresh       = spline_cc_ssthresh,
    .cong_control   = spline_cc_main,
    .cwnd_event     = spline_cc_cwnd_event,
    .pkts_acked     = spline_cc_pkts_acked,
    .owner          = THIS_MODULE,
    .name           = "spline_cc",
    .sshm           = sizeof(struct scc),
};

// Инициализация модуля
static int __init spline_cc_register(void) {
    return tcp_register_congestion_control(&spline_cc_ops);
}

// Выгрузка модуля
static void __exit spline_cc_unregister(void) {
    tcp_unregister_congestion_control(&spline_cc_ops);
}

module_init(spline_cc_register);
module_exit(spline_cc_unregister);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bekzhan Kalimollayev");
MODULE_DESCRIPTION("Spline Congestion Control for Linux Kernel");
