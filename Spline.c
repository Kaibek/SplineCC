#include <stdio.h>

#define FIXED_SHIFT 10
#define THRESHOLD_PERCENT 100        
#define MULu64_FAST(x, y) ((x) * (y) >> FIXED_SHIFT)      
#define DIV3(x) (((x) * 171) >> 9) 
#define DIV100(x) (((x) * 3) >> 8) 
#define U32_MAX (~0U)               
#define MAX_RTT 1000000                                   // 1 с в микросекундах
#define MIN_RTT 10                                        // 10 мкс                                        
#define MAX_SSHTHRESH  900000                             // 10Gbit/s = 856 184 (MSS)
#define MULT0_9(x) ((x * 15) >> 4)

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
    u32 c, d;               // Коэффициенты кубического сплайна
    u64 b;                  // Коэффициент для пропускной способности
    u32 d_initial;          // Начальное значение d из find_cof_rtt
    u32 full_cof;           // Сумма коэффициентов
    u32 next_cwnd;          // Следующее окно перегрузки
    u32 cwnd_x, cwnd_y;     // Составляющие для вычисления endl_cof_cwnd 
    u32 cached_ratio;       // Кэшированное значение ratio
    u32 last_min_rtt;       // Самый минимальный RTT
    u64 cached_throughput;  // Кэшированное значение throughput_t
    u32 last_c_d_initial;   // Для проверки c + d_initial
    u32 ssthresh;           // Порог для slow-start
    u32 curr_ack;
    u32 last_ack;
    u32 max_ssthresh;
} sCC;

static inline u64 DIVu64(u64 x, u64 y)
{
    return ((x << FIXED_SHIFT) / y) >> FIXED_SHIFT;
}

static inline u32 err_r(u32 curr_rtt, u32 last_min_rtt)
{
    u32 RTT_SOLV = (curr_rtt + (last_min_rtt)) >> 1;
    static u32 smoothed_err_r = 10;
    smoothed_err_r = (smoothed_err_r * 3 + ((curr_rtt + RTT_SOLV) >> 1)) >> 2; // 0.75*старое + 0.25*новое
    return smoothed_err_r < 10 ? 10 : smoothed_err_r;
}

static inline u32 detect_fast_growth(u32 min, u32 curr)
{
    if (min == 0) return 0; // Избегаем деления на ноль
    return (u32)(MULu64_FAST(min, 2) > MULu64_FAST(curr, 3));
}

static u32 ratio_rtt(u32 curr_rtt, sCC* state)
{
    if (state->curr_rtt < state->last_min_rtt)
        state->last_min_rtt = state->curr_rtt;

    if (!state->last_rtt || !curr_rtt || curr_rtt > MAX_RTT) return 1;

    u32 ERR_R = err_r(state->curr_rtt, state->last_min_rtt);
    state->curr_rtt = curr_rtt;

    if (state->last_rtt < MIN_RTT)
        state->last_rtt = MIN_RTT;

    u32 ratio_u32;

    if (state->last_rtt == state->curr_rtt && state->cached_ratio != 0)
        ratio_u32 = state->cached_ratio;
    else
    {
        u64 ratio = (state->curr_rtt << 3) / state->last_rtt;
        ratio_u32 = (u32)ratio;
    }

    u32 ratio_cubed = (ratio_u32 * ratio_u32 * ratio_u32) >> 1;
    u32 loc_rtt;

    if (state->curr_rtt > state->last_rtt)
    {
        if (state->curr_rtt < state->last_rtt + ERR_R)
        {
            state->d = 1;
            state->d_initial = state->d;
            state->cached_ratio = 0;
            return state->d;
        }
        loc_rtt = ratio_cubed + (state->curr_rtt >> 1);
    }
    else if (state->curr_rtt < state->last_rtt)
    {
        if (state->curr_rtt + ERR_R > state->last_rtt)
        {
            state->d = 1;
            state->d_initial = state->d;
            state->cached_ratio = 0;
            return state->d;
        }
        loc_rtt = (ratio_cubed + (state->curr_rtt >> 1) + DIVu64(DIV3(state->curr_rtt), state->curr_rtt));
        state->cached_ratio = 0;
    }
    else
    {
        state->d = 1;
        state->d_initial = state->d;
        state->cached_ratio = 0;
        return state->d;
    }

    u32 result = loc_rtt + (loc_rtt >> 1);

    if (!loc_rtt)
    {
        return 1;
    }

    state->d = (ratio_u32 << 1) + ((result + loc_rtt) / loc_rtt);
    state->d_initial = state->d;

    if (state->last_min_rtt >= state->curr_rtt) state->last_min_rtt = state->curr_rtt;

    state->last_rtt = state->curr_rtt;

    if (!state->d) return 1;

    return state->d;
}

static u32 ratio_cwnd(sCC* state)
{
    if (!state->curr_cwnd || !state->last_rtt || !state->curr_rtt) return 1;

    if (!state->last_cwnd)
    {
        state->last_cwnd = state->curr_cwnd;
        return state->c = 1;
    }

    if (state->last_cwnd < state->curr_cwnd)
    {
        state->last_cwnd = (3 * state->last_cwnd + state->curr_cwnd) >> 2;
        state->c = (state->last_cwnd + state->curr_cwnd) > state->d ?
            (state->last_cwnd + state->curr_cwnd) - state->d : 1;
        return state->c;
    }

    state->cwnd_x = state->last_cwnd + state->curr_cwnd;
    u32 diff = (state->last_cwnd > state->curr_cwnd)
        ? (state->last_cwnd - state->curr_cwnd)
        : (state->curr_cwnd - state->last_cwnd);

    state->cwnd_y = diff > DIV3(diff) ? diff - DIV3(diff) : 0;
    u32 endl_cof_cwnd = (state->cwnd_x - state->cwnd_y) - state->d;

    if (!endl_cof_cwnd) return 1;

    if (state->last_rtt > state->curr_rtt && endl_cof_cwnd >= state->curr_cwnd)
    {
        state->c = (state->curr_cwnd + 1);
        return state->c;
    }

    state->c = endl_cof_cwnd > state->curr_cwnd ? state->curr_cwnd : endl_cof_cwnd;
    state->last_cwnd = state->curr_cwnd;

    return state->c;
}

static u32 ratio_bw(u64 tp, sCC* state)
{
    if (!tp || !state->d || !state->c) return 1;

    if (tp < state->cached_throughput * 8 / 10 && state->cached_throughput != 0) {
        state->throughput_t = state->cached_throughput;
    }
    else {
        state->throughput_t = tp;
    }

    u32 c_d_initial = state->c + state->d;

    if (state->last_c_d_initial == c_d_initial && state->cached_throughput != 0) {
        state->throughput = state->cached_throughput;
    }
    else {
        state->throughput = DIVu64(tp, c_d_initial);
        state->cached_throughput = state->throughput;
        state->last_c_d_initial = c_d_initial;
    }

    state->b = DIVu64(state->throughput_t, state->throughput);
    if (!state->b) return 1;

    return state->b;
}

static u32 handle_slow_start(sCC* state, u32 num_ack)
{
    state->last_ack = state->curr_ack;
    state->curr_ack = num_ack;

    if (state->curr_cwnd < state->ssthresh)
    {
        if (!state->curr_cwnd)
        {
            state->curr_cwnd = 5;
            state->next_cwnd = 5;
            state->last_cwnd = 5;
            return state->next_cwnd;
        }

        state->curr_cwnd = 10;

        if (state->curr_rtt > (state->last_min_rtt * 39) >> 5 || state->last_ack > state->curr_ack)
        {
            state->curr_cwnd = state->last_cwnd - (state->last_cwnd - state->last_max_cwnd) + ((num_ack) >> 2);
        }
        else
            state->curr_cwnd += state->last_cwnd + ((num_ack + 4) >> 1);

        state->next_cwnd = state->curr_cwnd;
        state->last_cwnd = state->next_cwnd;

        if (state->curr_cwnd > state->last_max_cwnd)
            state->last_max_cwnd = state->curr_cwnd;

        if (state->curr_cwnd > state->ssthresh)
        {
            state->curr_cwnd = state->ssthresh;
            state->next_cwnd = state->ssthresh;
            state->last_cwnd = state->next_cwnd;
            state->last_max_cwnd = state->curr_cwnd;
        }

        return state->next_cwnd;
    }

    return 0;
}

static u32 ssthresh_comp(sCC* state)
{
    if (!state->ssthresh)
        state->ssthresh = state->b + DIV100(state->last_max_cwnd * THRESHOLD_PERCENT);

    if (state->ssthresh > MAX_SSHTHRESH)
        state->ssthresh = state->b + MAX_SSHTHRESH;

    if (!state->max_ssthresh)
        state->max_ssthresh = state->ssthresh * state->d;

    if (state->curr_rtt > (state->last_min_rtt * 39) >> 5 && state->curr_ack < state->last_ack)
        state->ssthresh = state->curr_cwnd;
    else if (state->next_cwnd > state->ssthresh && state->curr_rtt > state->last_min_rtt)
    {
        state->ssthresh = state->ssthresh + state->next_cwnd;
        state->max_ssthresh = (state->ssthresh + state->max_ssthresh);
    }

    if (state->curr_ack > state->last_ack && state->ssthresh > state->max_ssthresh)
        state->max_ssthresh = state->ssthresh;

    return state->ssthresh;
}

static u32 stable_rtt(sCC* state)
{
    if (state->curr_rtt <= state->last_rtt && state->curr_ack > state->last_ack)
    {
        if (state->curr_cwnd > state->last_max_cwnd)
            state->last_max_cwnd = state->curr_cwnd;

        if (state->next_cwnd < 1)
            state->next_cwnd = 1;

        state->next_cwnd = (state->next_cwnd > state->last_max_cwnd << 1) ? state->last_max_cwnd << 1 : state->next_cwnd;
        state->last_cwnd = state->curr_cwnd;
        state->last_max_cwnd = state->last_max_cwnd + (state->last_max_cwnd >> 2);

        return state->next_cwnd;
    }

    return 0;
}

static u32 overload_rtt(sCC* state)
{
    u32 ERR_R = err_r(state->curr_rtt, state->last_min_rtt);

    if (state->curr_rtt > state->last_rtt + ERR_R || state->curr_ack < state->last_ack)
    {
        state->last_cwnd = state->curr_cwnd;

        if (state->curr_rtt > state->last_rtt * (6 >> 2) || state->curr_ack < state->last_ack * (3 >> 2))
        {
            state->next_cwnd = state->curr_cwnd * (12 >> 4); // Уменьшение на 30%
        }
        else
        {
            state->next_cwnd = state->curr_cwnd * (15 >> 4); // Уменьшение на 10%
        }

        state->next_cwnd = state->next_cwnd < 5 ? 5 : state->next_cwnd;

        if (state->next_cwnd >= state->ssthresh)
        {
            state->next_cwnd = state->ssthresh;
            state->ssthresh = ssthresh_comp(state);
            state->last_cwnd = state->next_cwnd;
            return state->next_cwnd;
        }

        state->last_cwnd = state->next_cwnd;
        return state->next_cwnd;
    }

    return 0;
}

static u32 fairness_rtt(sCC* state)
{
    u32 ERR_R = err_r(state->curr_rtt, state->last_min_rtt);
    if (state->curr_cwnd == state->last_cwnd && state->last_rtt + ERR_R == state->curr_rtt)
    {
        state->next_cwnd = state->curr_cwnd + DIV3(state->b);
        state->last_cwnd = state->curr_cwnd;
        state->last_max_cwnd = state->next_cwnd;
        return state->next_cwnd;
    }
    return 0;
}

static u32 favorable_rtt(sCC* state)
{
    u32 ERR_R = err_r(state->curr_rtt, state->last_min_rtt);
    if (state->curr_cwnd >= state->last_cwnd && (state->curr_rtt - state->last_rtt) < ERR_R)
    {
        state->last_max_cwnd = state->curr_cwnd;
        state->next_cwnd = state->curr_cwnd + state->d;
        state->last_cwnd = state->next_cwnd;
        return state->next_cwnd;
    }
    return 0;
}

static u32 resolve_next_cwnd(sCC* state)
{
    u32 ERR_R = err_r(state->curr_rtt, state->last_min_rtt);
    u32 stab, over, fairness, favorable;
    if (state->d == 0) return 1;

    stab = stable_rtt(state);
    if (stab)
        return stab;

    fairness = fairness_rtt(state);
    if (fairness)
        return fairness;

    over = overload_rtt(state);
    if (over)
        return over;

    favorable = favorable_rtt(state);
    if (favorable)
        return favorable;

    state->last_cwnd = state->next_cwnd;
    state->next_cwnd = DIV3(state->b) + ((state->next_cwnd > state->last_max_cwnd << 1) ? state->last_max_cwnd >> 1 : state->next_cwnd);

    return state->next_cwnd;
}

static void handle_dup_ack(sCC* state)
{
    state->ssthresh = state->curr_cwnd >> 1;
    state->curr_cwnd = state->ssthresh;
    state->next_cwnd = DIV3(state->last_max_cwnd);
    state->last_max_cwnd = state->curr_cwnd;
    state->curr_ack = 0;
}

u32 inline SplineCC(u32 curr_rtt, u64 throughput, u32 num_acks, sCC* state)
{
    u32 prev_cwnd = state->curr_cwnd;
    u32 prev_rtt = state->curr_rtt;

    state->curr_rtt = curr_rtt;

    ratio_rtt(curr_rtt, state);

    u32 slow_start_cwnd = handle_slow_start(state, num_acks);
    state->ssthresh = ssthresh_comp(state);

    if (slow_start_cwnd) {
        state->curr_cwnd = slow_start_cwnd;
        // Update last_cwnd and last_rtt with previous values
        state->last_cwnd = prev_cwnd;
        state->last_rtt = prev_rtt;
        return slow_start_cwnd;
    }

    ratio_cwnd(state);
    ratio_bw(throughput, state);

    state->curr_cwnd = resolve_next_cwnd(state);

    // Update last_cwnd and last_rtt with previous values
    state->last_cwnd = prev_cwnd;
    state->last_rtt = prev_rtt;

    return state->curr_cwnd;
}
