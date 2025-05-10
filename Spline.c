#include <stdio.h>

#define FIXED_SHIFT 10
#define ERR_R 5                                           // Макс. погрешность RTT
// для threshold (в разработке)
#define THRESHOLD_PERCENT 120
#define THRESHOLD_PERCENT_HIGH (THRESHOLD_PERCENT - 60)   // 60% порог
#define THRESHOLD_PERCENT_LOW (THRESHOLD_PERCENT - 10)    // 110% порог          

#define MULu64_FAST(x, y) ((x) * (y) >> FIXED_SHIFT)
#define DIV3(x) (((x) * 171) >> 9)                              
#define U32_MAX (~0U)               
#define MAX_RTT 1000000                                   // 1 с в микросекундах
#define MIN_RTT 10                                        // 10 мкс


typedef unsigned int u32;
typedef unsigned long long u64;

// Структура для управления кубическим сплайном
typedef struct SplineCC {
    u32 last_rtt;           // Последний измеренный RTT (мкс)
    u32 curr_rtt;           // Текущий RTT (мкс)
    u32 curr_cwnd;          // Текущее окно перегрузки (сегменты)
    u32 last_max_cwnd;      // максимальный cwnd
    u32 last_cwnd;          // Предыдущее окно перегрузки (сегменты)
    u64 throughput;         // Пропускная способность (байт/с)
    u64 throughput_t;       // Временная пропускная способность
    u32 a, c, d;            // Коэффициенты кубического сплайна
    u32 d_initial;          // Начальное значение d из find_cof_rtt
    u32 full_cof;           // Сумма коэффициентов
    u32 inter_cwnd;         // Промежуточное окно перегрузки
    u32 next_cwnd;          // Следующее окно перегрузки
    u32 cwnd_x, cwnd_y;     // составляющее для вычисления endl_cof_cwnd = (state->cwnd_x - state->cwnd_y) - state->d_initial
    u64 b;                  // коэффициент для пропускной способности 
    u32 cached_ratio;       // Кэшированное значение ratio
    u32 cached_last_rtt;    // Последнее min_rtt для кэширования
    u32 last_min_rtt;       // самый минимальный RTT
    u64 cached_throughput;  // Кэшированное значение throughput_t
    u32 last_c_d_initial;   // Для проверки c + d_initial
} sCC;



static inline u64 DIVu64(u64 x, u64 y)
{
    return ((x << FIXED_SHIFT) / y) >> FIXED_SHIFT;
}


// Проверка на быстрое увеличение аргументов
static inline u32 detect_fast_growth(u32 min, u32 curr)
{
    if (min == 0) return 0; // Избегаем деления на ноль
    return (u32)(MULu64_FAST(min, 2) > MULu64_FAST(curr, 3));
}


// Вычисление коэффициента d на основе RTT
static u32 find_cof_rtt(u32 curr_rtt, sCC* state)
{
    if (!state->last_rtt || !curr_rtt || curr_rtt > MAX_RTT) return 1;

    state->curr_rtt = curr_rtt;

    // Защита от некорректных значений
    if (state->last_rtt < MIN_RTT)
        state->last_rtt = MIN_RTT;

    u32 ratio_u32;


    // Проверка кэша
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
            state->cached_ratio = 0; // Сброс кэша при изменении логики
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
        state->cached_ratio = 0; // Сброс кэша при изменении min_rtt
    }

    else
    {
        state->d = 1;
        state->d_initial = state->d;
        state->cached_ratio = 0; // Сброс кэша при изменении логики

        return state->d;
    }

    u32 result = loc_rtt + (loc_rtt >> 1);
    
    if (!loc_rtt)
    {
        printf("loc_rtt = %d", loc_rtt);
        return 1;
    }

    state->d = (ratio_u32 << 1) + ((result + loc_rtt) / loc_rtt);
    state->d_initial = state->d;

    if (state->last_min_rtt >= state->curr_rtt) state->last_min_rtt = state->curr_rtt;

    state->last_rtt = state->curr_rtt;

    if (!state->d) return 1;

    return state->d;
}


// Вычисление коэффициента c на основе cwnd
static u32 find_cof_cwnd(u32 curr_cwnd, sCC* state)
{
    if (!curr_cwnd || !state->last_rtt || !state->curr_rtt) return 1;

    if (detect_fast_growth(state->last_rtt, state->curr_rtt))
        return curr_cwnd;

    if (!state->last_cwnd)
    {
        state->last_cwnd = state->curr_cwnd;
        return state->c = 1;
    }

    if (detect_fast_growth(state->last_cwnd, state->curr_cwnd))
    {
        return state->last_cwnd;
    }

    if (state->last_cwnd < state->curr_cwnd)
    {
        state->last_cwnd = (3 * state->last_cwnd + state->curr_cwnd) >> 2;
        state->c = (state->last_cwnd + state->curr_cwnd) > state->d_initial ?
            (state->last_cwnd + state->curr_cwnd) - state->d_initial : 1;
        return state->c;
    }

    // Вычисление для last_cwnd >= curr_cwnd
    state->cwnd_x = state->last_cwnd + state->curr_cwnd;

    u32 diff = (state->last_cwnd > state->curr_cwnd)
        ? (state->last_cwnd - state->curr_cwnd)
        : (state->curr_cwnd - state->last_cwnd);

    state->cwnd_y = diff > DIV3(diff) ? diff - DIV3(diff) : 0;

    u32 endl_cof_cwnd = (state->cwnd_x - state->cwnd_y) - state->d_initial;

    if (!endl_cof_cwnd)
    {
        return 1;
    }

    if (state->last_rtt > state->curr_rtt && endl_cof_cwnd >= state->curr_cwnd)
    {
        state->c = (state->curr_cwnd + 1);
        return state->c;
    }

    state->c = endl_cof_cwnd > state->curr_cwnd ? state->curr_cwnd : endl_cof_cwnd;
    state->last_cwnd = state->curr_cwnd;

    return state->c;
}


// Вычисление коэффициента b на основе пропускной способности
static u32 find_cof_bw(u64 tp, sCC* state) 
{
    if (!tp || !state->d_initial || !state->c) return 1;
    if (state->throughput_t != tp)
    {
        state->throughput_t = tp;
    }

    u32 c_d_initial = state->c + state->d_initial;

    if (state->last_c_d_initial == c_d_initial && state->cached_throughput != 0)
        state->throughput = state->cached_throughput;

    else
    {
        state->throughput = DIVu64(tp, c_d_initial);
        state->cached_throughput = state->throughput;
        state->last_c_d_initial = c_d_initial;
    }

    state->b = DIVu64(state->throughput_t, state->throughput);
    if (!state->b) return 1;

    return state->b;
}


// Вычисление коэффициента a
static inline int find_cof_a(sCC* state)
{
    state->a = (state->b + state->c - state->d_initial) >> 2;
    return state->a;
}


// Вычисление промежуточного cwnd
static u32 inline resolve_inter_cwnd(sCC* state)
{
    if (!state->last_cwnd) return 1;
    state->inter_cwnd = (state->a + state->b + state->c - state->d_initial);

    return state->inter_cwnd;
}


// Вычисление следующего cwnd
static inline u32 resolve_next_cwnd(sCC* state)
{
    if (state->d_initial == 0) return 1;

    if (state->last_min_rtt >= state->curr_rtt)
    {

        if (state->curr_cwnd >= state->last_max_cwnd)
        {
            state->last_max_cwnd = state->curr_cwnd;
            state->next_cwnd = state->curr_cwnd + state->d;
            state->last_cwnd = state->next_cwnd;

            return state->next_cwnd;
        }
    }

    if ((state->curr_cwnd - state->last_cwnd) > 1 || (state->curr_rtt - state->last_min_rtt) > ERR_R)
    {
        state->last_cwnd = state->curr_cwnd;
        state->next_cwnd = state->curr_cwnd - 1;

        return state->next_cwnd;
    }


    state->next_cwnd = (state->inter_cwnd + (2 * (state->curr_cwnd) / state->d_initial)) - state->d_initial;
    return state->next_cwnd;
}



u32 SplineCC(u32 curr_cwnd, u32 curr_rtt, u32 throughput, sCC* state)
{
    find_cof_rtt(curr_rtt, state);
    find_cof_cwnd(curr_cwnd, state);
    find_cof_bw(throughput, state);
    find_cof_a(state);
    resolve_inter_cwnd(state);
    resolve_next_cwnd(state);

    return resolve_next_cwnd(state);
}

/*
    sCC cc_state = { 0 };
    sCC* tmp = &cc_state;
    tmp->curr_rtt = 40;    // мкс
    tmp->last_rtt = 50;    // мкс
    tmp->last_cwnd = 10;   // сегменты
    tmp->curr_cwnd = 50;   // сегменты

    //Первый обхож параметров
    u64 throughput = 1250000000; // 10 Гбит/с = 1.25e9 байт/с
    tmp->last_max_cwnd = 120;
    tmp->last_min_rtt = 100;
 

    // Второй набор параметров
    tmp->curr_rtt = 30;    // мкс
    tmp->curr_cwnd = 49;   // сегменты
    tmp->throughput = 1250000000; // 10 Гбит/с = 1.25e9 байт/с


    // Третий набор параметров
    tmp->curr_rtt = 120;    // мкс
    tmp->curr_cwnd = 115;   // сегменты
    tmp->throughput = 1250000000; // 10 Гбит/с = 1.25e9 байт/с



    // Четвертый набор параметров
    tmp->curr_rtt = 50;    // мкс
    tmp->curr_cwnd = 14;   // сегменты
    tmp->throughput = 1250000000; // 10 Гбит/с = 1.25e9 байт/с

    
Выводы:
next_cwnd = 133

next_cwnd = 115

next_cwnd = 114

next_cwnd = 13
*/
