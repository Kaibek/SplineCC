#ifndef SPLINE_CC_H
#define SPLINE_CC_H

#include "ns3/tcp-congestion-ops.h"

namespace ns3 {

enum ProbeMode {
    MODE_START_PROBE,
    MODE_PROBE_BW,
    MODE_PROBE_RTT,
    MODE_DRAIN_PROBE
};

struct SplineCC {
    uint32_t last_rtt;           // Последний измеренный RTT (мкс)
    uint32_t curr_rtt;           // Текущий RTT (мкс)
    uint32_t curr_cwnd;          // Текущее окно перегрузки (байты)
    uint32_t last_max_cwnd;      // Максимальный cwnd
    uint32_t last_cwnd;          // Предыдущее окно перегрузки (байты)
    uint64_t throughput;         // Пропускная способность (байт/с)
    uint32_t eps;                // Эпсилон RTT
    uint64_t bw;                 // Оценка пропускной способности (сегменты)
    uint32_t last_bw;            // Последний bw
    uint32_t next_cwnd;          // Следующее окно перегрузки
    uint32_t last_min_rtt;       // Самый минимальный RTT
    uint32_t curr_ack;           // Текущий ack
    uint32_t last_ack;           // Последний ack
    uint32_t epp;                // Кол-во прошедших эпох
    uint32_t epp_min_rtt;        // Полный оборот эпох, обновление min_rtt
    uint32_t probe_mode;         // Режим
    uint32_t gamma;              // Гамма от ACK
    uint32_t pacing_rate;        // Темп отправки
    uint32_t rtt_avg;            // Среднее минимального и последнее RTT
    ProbeMode current_mode;      // Текущий режим
};

class SplineCcNew : public TcpCongestionOps
{
public:
    static TypeId GetTypeId(void);
    SplineCcNew();
    SplineCcNew(const SplineCcNew& other);
    virtual ~SplineCcNew();

    virtual std::string GetName() const override;
    virtual uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    virtual void IncreaseWindow(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;
    virtual Ptr<TcpCongestionOps> Fork() override;

    uint32_t SplineCCAlgo(Ptr<TcpSocketState> tcb, uint32_t curr_rtt, uint64_t throughput, uint32_t num_acks);

private:
    SplineCC m_state;
    uint32_t __epsilone_rtt(uint32_t rtt, uint32_t ack);
    uint64_t __bw(uint64_t tp, Ptr<TcpSocketState> tcb);
    uint32_t __gamma_ack();
    uint32_t stable_rtt_bw(Ptr<TcpSocketState> tcb);
    uint32_t fairness_rtt_bw(Ptr<TcpSocketState> tcb);
    uint32_t overload_rtt_bw(uint32_t ack, Ptr<TcpSocketState> tcb);
    uint32_t prob_bw(uint32_t ack, Ptr<TcpSocketState> tcb);
    uint32_t stable_rtt(Ptr<TcpSocketState> tcb);
    uint32_t overload_rtt(uint32_t ack, Ptr<TcpSocketState> tcb);
    uint32_t fairness_rtt(Ptr<TcpSocketState> tcb);
    uint32_t prob_rtt(uint32_t ack, Ptr<TcpSocketState> tcb);
    uint32_t drain_probe(Ptr<TcpSocketState> tcb);
    uint32_t start_probe(Ptr<TcpSocketState> tcb);
    uint64_t pacing_gain_rate(Ptr<TcpSocketState> tcb);
    uint32_t cwnd_next_gain(Ptr<TcpSocketState> tcb);
    uint32_t probs(uint32_t ack, Ptr<TcpSocketState> tcb);
};

} // namespace ns3

#endif // SPLINE_CC_H

