#include <stdio.h>

#define FIXED_SHIFT 10
#define MSS 1460
#define THRESHOLD_PERCENT 100              
#define DIV3(x) (((x) * 171) >> 9) 
#define DIV100(x) (((x) * 3) >> 8) 
#define U32_MAX (~0U)               
#define MAX_RTT 1000                            
#define MIN_RTT 1                                                                         
#define MAX_SSHTHRESH 900000                        // 10Gbit/s = 856 184 (MSS)
#define MULT0_9(x) ((x * 15) >> 4)
#define MAX_CWND MAX_SSHTHRESH                      // Добавлено: максимальное значение для cwnd
#define MIN_CWND 1                                  // Добавлено: минимальное значение для cwnd
#define MAX_BW (MAX_SSHTHRESH * MSS)               // Добавлено: максимальная пропускная способность

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
    u32 eps;                // эпсилон RTT
    u64 bw;                 // пропускной способности
    u32 last_bw;            // последний bw
    u32 next_cwnd;          // Следующее окно перегрузки
    u32 last_min_rtt;       // Самый минимальный RTT
    u32 curr_ack;           // текущий ack
    u32 last_ack;           // последний ack
    u32 epp;                // кол-во прошедших эпох
    u32 epp_min_rtt;        // полный оборот 10 эпох
    u32 probe_mode;         // режим
    u32 gamma;              // гамма от ACK
    u32 pacing_rate;        // 
    u32 rtt_avg;            // среднее минимального и последнее rtt
    ProbeMode current_mode; // Track the current probing mode
} sCC;

static u32 __epsilone_rtt(u32 rtt, u32 ack, sCC* state)
{
    if (rtt == 0 || rtt > MAX_RTT) rtt = MIN_RTT;
    state->last_ack = state->curr_ack;
    state->curr_ack = ack;
    if (state->last_rtt < MIN_RTT)
        state->last_rtt = MIN_RTT;

    if (rtt == 0) {
        state->eps = 1; // Устанавливаем минимальное значение epsilon
    }
    else {
        state->eps = (((rtt + state->curr_rtt) / rtt) + 1);
    }
    if (state->eps > 10) state->eps = 10;

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
    state->rtt_avg = (state->last_min_rtt + state->last_rtt) >> 1;
    state->throughput = tp;
    if (tp == 0) {
        state->bw = 0;
        return state->bw;
    }
    u64 bw = (state->throughput * state->rtt_avg) >> FIXED_SHIFT;
    state->last_bw = (state->last_bw * 3 + state->bw) >> 2; // EMA 75%
    state->bw = bw / MSS;

    if (state->last_bw)
    {
        u64 min_allowed = (state->last_bw * 3) >> 2;  // 75%
        u64 max_allowed = (state->last_bw * 6) >> 2;  // 150%
        if (state->bw < min_allowed)
            state->bw = min_allowed;
        if (state->bw > max_allowed)
            state->bw = max_allowed;
        if (state->bw > MAX_BW)
            state->bw = MAX_BW;
    }
    if (state->bw < 1)
        state->bw = 1;

    if (state->curr_rtt > (state->last_min_rtt * 2))
        state->bw = state->last_bw;

    return state->bw;
}

static u32 __gamma_ack(sCC* state)
{
    if (state->curr_ack <= state->last_ack)
    {
        state->gamma = 1;
        return state->gamma;
    }
    if (state->curr_ack == 0) {
        state->gamma = 1;
    }
    else 
        state->gamma = (((state->curr_ack + state->last_ack) / state->curr_ack) + 1);
    
    if (state->gamma > 10) state->gamma = 10;
    return state->gamma;
}

static inline u64 DIVu64(u64 x, u64 y)
{
    if (y == 0) 
        return x >> FIXED_SHIFT; // Возвращаем x с учётом FIXED_SHIFT
    return ((x << FIXED_SHIFT) / y) >> FIXED_SHIFT;
}

static u32 stable_rtt_bw(sCC* state)
{
    if (state->eps >= 2 && state->eps < 3)
    {
        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;

        state->curr_cwnd = state->next_cwnd + (state->bw >> state->eps);
        if (((state->curr_cwnd * 3) >> 2) > state->last_cwnd)
            state->curr_cwnd = (state->curr_cwnd * 2) >> 2;
  
        if (state->curr_cwnd > MAX_CWND)
            state->curr_cwnd = MAX_CWND;
        return state->curr_cwnd;
    }
    return 0;
}

static u32 fairness_rtt_bw(sCC* state)
{
    if (state->eps > 1 && state->eps <= 2)
    {
        state->curr_cwnd = state->last_cwnd - (state->curr_cwnd >> 2);

        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;
        return state->curr_cwnd;
    }
    return 0;
}

static u32 favorable_rtt_bw(sCC* state)
{
    if (state->eps >= 3)
    {
        state->curr_cwnd += (state->bw >> state->eps >> 1);
        if (((state->curr_cwnd * 3) >> 2) > state->last_cwnd)
            state->curr_cwnd = (state->curr_cwnd * 3) >> 2;

        if (state->curr_cwnd > MAX_CWND)
            state->curr_cwnd = MAX_CWND;
        return state->curr_cwnd;
    }
    return 0;
}

static u32 overload_rtt_bw(u32 ack, sCC* state)
{
    if (state->eps <= 1)
    {
        if (state->curr_rtt > state->last_rtt + state->eps)
            state->curr_cwnd = state->curr_cwnd * 12 >> 4; // Уменьшение на 30%
        else
            state->curr_cwnd = state->curr_cwnd * 15 >> 4; // Уменьшение на 10%
        if (ack <= state->last_ack)
        {
            state->curr_cwnd = state->last_max_cwnd - state->curr_ack;

            if (state->curr_cwnd > state->last_max_cwnd) // Предотвращаем переполнение
                state->curr_cwnd = MIN_CWND;
        }
        // Добавлено: минимальное значение cwnd
        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;
        return state->curr_cwnd;
    }
    return 0;
}

static u32 prob_bw(u32 ack, sCC* state) {
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

    if (state->curr_cwnd < MIN_CWND)
        state->curr_cwnd = MIN_CWND;
    return state->curr_cwnd;
}

static u32 stable_rtt(sCC* state)
{
    if (state->eps < 3 && state->gamma == 2)
    {
        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;

        state->curr_cwnd = state->next_cwnd;

        if (state->curr_cwnd > MAX_CWND)
            state->curr_cwnd = MAX_CWND;
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
        if (ack <= state->last_ack)
        {
            state->curr_cwnd = state->last_max_cwnd - state->curr_ack;

            if (state->curr_cwnd > state->last_max_cwnd) // Предотвращаем переполнение
                state->curr_cwnd = MIN_CWND;
        }
        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;

        return state->curr_cwnd;
    }
    return 0;
}

static u32 fairness_rtt(sCC* state)
{
    if (state->eps <= 2 || state->gamma < 2)
    {
        state->curr_cwnd = state->last_cwnd - (state->curr_cwnd >> 2);

        if (state->curr_cwnd < MIN_CWND)
            state->curr_cwnd = MIN_CWND;
        return state->curr_cwnd;
    }
    return 0;
}

static u32 favorable_rtt(sCC* state)
{
    if (state->eps >= 3 || state->gamma >= 3)
    {
        state->curr_cwnd = state->last_cwnd + (state->curr_ack >> 2);

        if (state->curr_cwnd > MAX_CWND)
            state->curr_cwnd = MAX_CWND;
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

    if (state->curr_cwnd < MIN_CWND)
        state->curr_cwnd = MIN_CWND;
    return state->curr_cwnd;
}

static u32 drain_probe(sCC* state)
{
    if (state->curr_cwnd > state->bw)
        state->curr_cwnd -= (state->curr_cwnd - state->bw) >> 1;

    if (state->curr_cwnd < MIN_CWND)
        state->curr_cwnd = MIN_CWND;
    return state->curr_cwnd;
}

static u32 start_probe(sCC* state)
{
    state->curr_cwnd = state->curr_cwnd + state->curr_ack;

    if (state->curr_cwnd > MAX_CWND)
        state->curr_cwnd = MAX_CWND;
    return state->curr_cwnd;
}

static u64 pacing_gain_rate(sCC* state)
{

    u64 pacing_gain = state->eps == 0 ? 1 : (state->eps + state->gamma) / state->eps;
    if (pacing_gain < 1) pacing_gain = 1;
    if (pacing_gain > 3) pacing_gain = 3;
    state->pacing_rate = (state->bw * (pacing_gain + pacing_gain)) >> 2;
 
    if (state->pacing_rate < 1)
        state->pacing_rate = 1;
    return state->pacing_rate;
}

static u32 cwnd_next_gain(sCC* state) {
    u32 cwnd_gain = state->last_max_cwnd == 0 ? 1 : (state->last_cwnd + state->last_max_cwnd) / state->last_max_cwnd;
    if (cwnd_gain < 1) cwnd_gain = 1;
    if (cwnd_gain > 3) cwnd_gain = 3;
    state->curr_cwnd = (cwnd_gain * state->bw * state->last_min_rtt) >> state->eps;

    if (state->eps == 0)
        state->curr_cwnd = (cwnd_gain * state->bw * state->last_min_rtt) >> 1;

    if ((state->curr_cwnd * 15 >> 4) < state->last_cwnd)
        state->curr_cwnd = state->last_cwnd * 15 >> 4;

    if (state->curr_cwnd < MIN_CWND)
        state->curr_cwnd = MIN_CWND;

    if (state->curr_cwnd > MAX_CWND)
        state->curr_cwnd = MAX_CWND;
    return state->curr_cwnd;
}

static u32 probs(u32 ack, sCC* state)
{
    if (state->epp < 10) 
        state->epp++;
    
    if (!state->probe_mode) 
    {
        state->probe_mode = 1;
        state->current_mode = MODE_START_PROBE;
        return start_probe(state);
    }
    if (state->eps <= 1 && state->gamma <= 1 && state->curr_rtt > state->last_rtt) 
    {
        state->current_mode = MODE_DRAIN_PROBE;
        return drain_probe(state);
    }
    if (state->epp == 10) 
    {
        state->epp = 0;
        if (state->epp_min_rtt) {
            state->epp_min_rtt = 0;
            state->current_mode = MODE_PROBE_BW;
        }
        else 
        {
            switch (state->current_mode) 
            {
            case MODE_PROBE_BW:
                state->current_mode = MODE_PROBE_RTT;
                break;
            case MODE_PROBE_RTT:
                state->current_mode = MODE_DRAIN_PROBE;
                break;
            case MODE_DRAIN_PROBE:
                state->current_mode = MODE_START_PROBE;
                break;
            default:
                state->current_mode = MODE_PROBE_BW;
                break;
            }
        }
    }

    switch (state->current_mode) 
    {
    case MODE_START_PROBE:
        pacing_gain_rate(state);
        return start_probe(state);
    case MODE_PROBE_BW:
        pacing_gain_rate(state);
        if (state->eps >= 2) {
            return cwnd_next_gain(state);
        }
        return prob_bw(ack, state);
    case MODE_PROBE_RTT:
        pacing_gain_rate(state);
        state->pacing_rate = (state->pacing_rate * 12 >> 4);
        return prob_rtt(ack, state);
    case MODE_DRAIN_PROBE:
        pacing_gain_rate(state);
        return drain_probe(state);
    default:
        pacing_gain_rate(state);
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
        if (state->next_cwnd < MIN_CWND)
            state->next_cwnd = MIN_CWND;

        if (state->next_cwnd > MAX_CWND)
            state->next_cwnd = MAX_CWND;
        return state->next_cwnd;
    }
    if (state->bw <= state->curr_cwnd && state->current_mode == MODE_PROBE_RTT)
    {
        state->next_cwnd = state->bw + (state->curr_ack >> 2);
        if (state->next_cwnd < MIN_CWND)
            state->next_cwnd = MIN_CWND;

        if (state->next_cwnd > MAX_CWND)
            state->next_cwnd = MAX_CWND;
        return state->next_cwnd;
    }
    if (state->next_cwnd < MIN_CWND)
        state->next_cwnd = MIN_CWND;

    if (state->next_cwnd > MAX_CWND)
        state->next_cwnd = MAX_CWND;
    return state->next_cwnd;
}
