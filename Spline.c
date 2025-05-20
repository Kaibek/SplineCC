#include <stdio.h>

#define FIXED_SHIFT 10
#define THRESHOLD_PERCENT 100              
#define DIV3(x) (((x) * 171) >> 9) 
#define DIV100(x) (((x) * 3) >> 8) 
#define U32_MAX (~0U)               
#define MAX_RTT 1000000                                  // 1 с в микросекундах
#define MIN_RTT 1                                        // 10 мкс                                        
#define MAX_SSHTHRESH  900000                            // 10Gbit/s = 856 184 (MSS)
#define MULT0_9(x) ((x * 15) >> 4)

typedef enum
{
    MODE_START_PROBE,
    MODE_PROBE_BW,
    MODE_PROBE_RTT,
    MODE_DRAIN_PROBE
} ProbeMode;


typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct SplineCC {
    u32 last_rtt;           // Последний измеренный RTT (мкс)
    u32 curr_rtt;           // Текущий RTT (мкс)
    u32 curr_cwnd;          // Текущее окно перегрузки (сегменты)
    u32 last_max_cwnd;      // Максимальный cwnd
    u32 last_cwnd;          // Предыдущее окно перегрузки (сегменты)
    u64 throughput;         // Пропускная способность (байт/с)
    u64 throughput_t;       // Временная пропускная способность
    u32 eps;                // эпсилон RTT
    u64 bw;                 // пропускной способности
    u32 last_bw;
    u32 next_cwnd;          // Следующее окно перегрузки
    u32 last_min_rtt;       // Самый минимальный RTT
    u64 cached_throughput;  // Кэшированное значение throughput_t
    u32 ssthresh;           // Порог для slow-start
    u32 curr_ack;
    u32 last_ack;
    u32 max_ssthresh;
    u32 epp;                // кол-во прошедших эпох
    u32 epp_min_rtt;        // полный оборот 10 эпох, в один из эпох может измениться min_rtt, тем самым применяется режим PROBE_BW
    u32 probe_mode;         // режим
    u32 gamma;              // гамма от ACK
    ProbeMode current_mode; // Track the current probing mode
} sCC;

static u32 __epsilone_rtt(u32 rtt, u32 ack, sCC* state)
{
    if (rtt == 0) rtt = 1;
    state->last_ack = state->curr_ack;
    state->curr_ack = ack;
    if (state->last_rtt < MIN_RTT)
        state->last_rtt = MIN_RTT;

    state->eps = (((rtt + state->curr_rtt) / rtt) + 1);

    state->last_rtt = state->curr_rtt;
    state->curr_rtt = rtt;

    if (state->last_min_rtt >= state->curr_rtt)
    {
        state->last_min_rtt = state->curr_rtt;
        state->epp_min_rtt++;
    }
    return state->eps;
}

static u64 __bw(u64 tp, sCC* state)
{
    u32 rtt_scaling = (state->last_min_rtt + state->last_rtt) >> 1;
    state->throughput = tp;
    u64 bw = (state->throughput * rtt_scaling) >> FIXED_SHIFT;
    // EMA (Exponential Moving Average)
    state->last_bw = (state->last_bw * 3 + state->bw) >> 2; // EMA 75%
    state->bw = bw / 1460;

    if (state->last_bw)
    {
        // Фильтр — минимальное значение 75% от предыдущего bw
        u64 min_allowed = (state->last_bw * 3) >> 2;  // 75%
        if (state->bw < min_allowed)
            state->bw = min_allowed;

        u64 max_allowed = (state->last_bw * 6) >> 2;
        if (state->bw > max_allowed)
            state->bw = max_allowed;

        if (state->curr_rtt > (state->last_min_rtt * 2))
            state->bw = state->last_bw;
    }
    return state->bw;
}

static u32 __gamma_ack(sCC* state)
{
    state->gamma = (((state->curr_ack + state->last_ack) / state->curr_ack) + 1);
    return state->gamma;
}

static inline u64 DIVu64(u64 x, u64 y)
{
    return ((x << FIXED_SHIFT) / y) >> FIXED_SHIFT;
}


static u32 stable_rtt_bw(sCC* state)
{
    if (state->eps >= 2 && state->eps < 3)
    {
        if (state->curr_cwnd < 1)
            state->curr_cwnd = 1;

        state->curr_cwnd = state->next_cwnd + (state->bw >> state->eps);
        if (((state->curr_cwnd * 3) >> 2) > state->last_cwnd)
        {
            state->curr_cwnd = (state->curr_cwnd * 2) >> 2;
        }
        return state->curr_cwnd;
    }
    return 0;
}


static u32 fairness_rtt_bw(sCC* state)
{
    if (state->eps > 1 && state->eps <= 2)
    {
        state->curr_cwnd = state->last_cwnd - (state->curr_cwnd >> 2);
        return state->curr_cwnd;
    }
    return 0;
}


static u32 favorable_rtt_bw(sCC* state)
{
    if (state->eps >= 3)
    {
        state->curr_cwnd += (state->bw >> state->eps / 2);
        if (((state->curr_cwnd * 3) >> 2) > state->last_cwnd)
        {
            state->curr_cwnd = (state->curr_cwnd * 3) >> 2;
        }
        return state->curr_cwnd;
    }
    return 0;
}

static u32 overload_rtt_bw(u32 ack, sCC* state)
{
    {
        if (state->eps <= 1)
        {

            if (state->curr_rtt > state->last_rtt + state->eps)
                state->curr_cwnd = state->curr_cwnd * 12 >> 4; // Уменьшение на 30%
            else
                state->curr_cwnd = state->curr_cwnd * 15 >> 4; // Уменьшение на 10%

            if (state->curr_cwnd >= state->ssthresh || ack <= state->last_ack)
            {
                if (!state->ssthresh)
                    state->curr_cwnd = state->last_cwnd;
                else
                    state->curr_cwnd = state->last_max_cwnd - (state->ssthresh) + state->curr_ack;

                return state->curr_cwnd;
            }
            return state->curr_cwnd;
        }

        return 0;
    }

}

static u32 prob_bw(u32 ack, sCC* state)
{
    u32 stab, over, fairness, favorable;

    stab = stable_rtt_bw(state);
    if (stab)
        return stab;

    fairness = fairness_rtt_bw(state);
    if (fairness)
        return fairness;

    over = overload_rtt_bw(ack, state);
    if (over)
        return over;

    favorable = favorable_rtt_bw(state);
    if (favorable)
        return favorable;

    return state->curr_cwnd;
}


static u32 stable_rtt(sCC* state)
{
    if (state->eps < 3 && state->gamma == 2)
    {
        if (state->curr_cwnd < 1)
            state->curr_cwnd = 1;

        state->curr_cwnd = state->next_cwnd;
        return state->curr_cwnd;
    }
    return 0;
}


static u32 overload_rtt(u32 ack, sCC* state)
{
    if (state->eps <= 1 || state->gamma <= 1)
    {

        if (state->curr_rtt > state->last_rtt * 6 >> 2 || ack < state->last_ack * 3 >> 2)
            state->curr_cwnd = state->last_cwnd * 12 >> 4; // Уменьшение на 30%

        else
            state->curr_cwnd = state->last_cwnd * 15 >> 4; // Уменьшение на 10%

        if (state->curr_cwnd >= state->ssthresh || ack <= state->last_ack)
        {
            if (!state->ssthresh)
                state->curr_cwnd = state->last_cwnd;
            else
                state->curr_cwnd = state->last_max_cwnd - (state->ssthresh) + state->curr_ack;

            return state->curr_cwnd;
        }

        return state->curr_cwnd;
    }

    return 0;
}


static u32 fairness_rtt(sCC* state)
{
    if (state->eps <= 2 || state->gamma < 2)
    {
        state->curr_cwnd = state->last_cwnd - (state->curr_cwnd >> 2);
        return state->curr_cwnd;
    }
    return 0;
}


static u32 favorable_rtt(sCC* state)
{
    if (state->eps >= 3 || state->gamma >= 3)
    {
        state->curr_cwnd = state->last_cwnd + (state->curr_ack >> 2);
        return state->curr_cwnd;
    }
    return 0;
}

static u32 prob_rtt(u32 ack, sCC* state)
{
    u32 stab, over, fairness, favorable;

    stab = stable_rtt(state);
    if (stab)
        return stab;

    fairness = fairness_rtt(state);
    if (fairness)
        return fairness;

    over = overload_rtt(ack, state);
    if (over)
        return over;

    favorable = favorable_rtt(state);
    if (favorable)
        return favorable;

    return state->curr_cwnd;
}



static u32 drain_probe(sCC* state)
{
    if (state->curr_cwnd > state->bw)
        state->curr_cwnd -= (state->curr_cwnd - state->bw) >> 1;
    return state->curr_cwnd;
}

static u32 start_probe(sCC* state)
{
    state->curr_cwnd = state->curr_cwnd + state->curr_ack;
    return state->curr_cwnd;
}

static u32 probs(u32 ack, sCC* state)
{
    // Увеличиваем эпоху
    if (state->epp < 10) {
        state->epp++;
    }

    // Первая инициализация
    if (!state->probe_mode) {
        printf("probe_start!\n");
        state->probe_mode = 1;
        state->current_mode = MODE_START_PROBE;
        return start_probe(state);
    }

    // Плохое состояние — вход в дренаж
    if (state->eps <= 1 && state->gamma <= 1 && state->curr_rtt > state->last_rtt) {
        printf("drain_probe! Problem RTT/ACK!\n");
        state->current_mode = MODE_DRAIN_PROBE;
        return drain_probe(state);
    }

    // Каждые 10 эпох — смена режима
    if (state->epp == 10) {
        state->epp = 0;

        if (state->epp_min_rtt) {
            state->epp_min_rtt = 0;
            state->current_mode = MODE_PROBE_BW;
            printf("probe_bw!\n");
        }
        else {
            // Циклический переход
            switch (state->current_mode) {
            case MODE_PROBE_BW:
                state->current_mode = MODE_PROBE_RTT;
                printf("probe_rtt!\n");
                break;
            case MODE_PROBE_RTT:
                state->current_mode = MODE_DRAIN_PROBE;
                printf("drain_probe!\n");
                break;
            case MODE_DRAIN_PROBE:
                state->current_mode = MODE_START_PROBE;
                printf("probe_start!\n");
                break;
            default:
                state->current_mode = MODE_PROBE_BW;
                printf("probe_bw!\n");
                break;
            }
        }
    }

    // Выполняем текущий режим
    switch (state->current_mode) {
    case MODE_START_PROBE:
        printf("probe_start raw!\n");
        return start_probe(state);
    case MODE_PROBE_BW:
        printf("probe_bw raw!\n");
        return prob_bw(ack, state);
    case MODE_PROBE_RTT:
        printf("probe_rtt raw!\n");
        return prob_rtt(ack, state);
    case MODE_DRAIN_PROBE:
        printf("drain_probe raw!\n");
        return drain_probe(state);
    default:
        printf("probe_bw fallback!\n");
        return prob_bw(ack, state);
    }
}

u32 inline SplineCC(u32 curr_rtt, u64 throughput, u32 num_acks, sCC* state)
{
    if (state->last_max_cwnd < state->curr_cwnd)
        state->last_max_cwnd = state->curr_cwnd;

    u32 prev_cwnd = state->curr_cwnd;

    __epsilone_rtt(curr_rtt, num_acks, state);
    __bw(throughput, state);
    __gamma_ack(state);

    state->last_cwnd = prev_cwnd;
    state->next_cwnd = probs(num_acks, state);
    if (state->bw <= state->curr_cwnd && state->current_mode == MODE_PROBE_BW)
    {
        state->next_cwnd = state->bw;
        return state->next_cwnd;
    }

    if (state->bw <= state->curr_cwnd && state->current_mode == MODE_PROBE_RTT)
    {
        state->next_cwnd = state->bw + (state->curr_ack >> 2);
        return state->next_cwnd;
    }

    return state->next_cwnd;
}
