#include "spline-cc.h"
#include "ns3/log.h"
#include "ns3/tcp-socket-state.h"

NS_LOG_COMPONENT_DEFINE("SplineCcNew");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(SplineCcNew);

TypeId
SplineCcNew::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::SplineCcNew")
        .SetParent<TcpCongestionOps>()
        .SetGroupName("Internet")
        .AddConstructor<SplineCcNew>();
    return tid;
}

SplineCcNew::SplineCcNew()
    : TcpCongestionOps(),
      m_state{0}
{
    NS_LOG_FUNCTION(this);
    m_state.last_rtt = 50; // 100 мс
    m_state.last_min_rtt = 10;
    m_state.curr_cwnd = 10 * 1448; // Начальное окно 10 MSS
    m_state.last_cwnd = m_state.curr_cwnd;
    m_state.last_max_cwnd = m_state.curr_cwnd;
}

SplineCcNew::SplineCcNew(const SplineCcNew& other)
    : TcpCongestionOps(other),
      m_state(other.m_state)
{
    NS_LOG_FUNCTION(this);
}

SplineCcNew::~SplineCcNew()
{
    NS_LOG_FUNCTION(this);
}

uint32_t
SplineCcNew::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);
    uint32_t ssthresh = std::max(m_state.curr_cwnd * 14 >> 4, 2 * tcb->m_segmentSize);
    NS_LOG_INFO("CA_LOSS detected, setting ssthresh to " << ssthresh);
    return ssthresh;
}

void
SplineCcNew::IncreaseWindow(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
    NS_LOG_FUNCTION(this << tcb << segmentsAcked);
    uint32_t curr_rtt = tcb->m_lastRtt.Get().GetMicroSeconds();
    if (curr_rtt == 0) curr_rtt = 1; // MIN_RTT = 1 мс
    uint64_t throughput = tcb->m_bytesInFlight << 20 / (curr_rtt ? curr_rtt : 1000);

    if (m_state.curr_cwnd == 0)
    {
        m_state.curr_cwnd = 50 * tcb->m_segmentSize; // 50 MSS
    }
    if (tcb->m_congState == TcpSocketState::CA_LOSS)
    {
        // При CA_LOSS переключаемся в MODE_DRAIN_PROBE
        m_state.current_mode = MODE_DRAIN_PROBE;
        m_state.curr_cwnd = drain_probe(tcb); // Сокращаем cwnd до bw * MSS * 3
        NS_LOG_INFO("CA_LOSS: Switching to MODE_DRAIN_PROBE, cwnd=" << m_state.curr_cwnd);
    }
    else if (tcb->m_congState == TcpSocketState::CA_RECOVERY)
    {
        // Быстрое восстановление: увеличиваем cwnd на 3 * MSS за ACK
        m_state.curr_cwnd += segmentsAcked * tcb->m_segmentSize << 2;
        if (m_state.curr_cwnd > m_state.last_max_cwnd << 1) // Увеличен лимит
            m_state.curr_cwnd = m_state.last_max_cwnd << 1;
        NS_LOG_INFO("CA_RECOVERY: cwnd increased to " << m_state.curr_cwnd);
    }
    else
    {
        m_state.curr_cwnd = SplineCCAlgo(tcb, curr_rtt, throughput, segmentsAcked);
    }
    tcb->m_cWnd = m_state.curr_cwnd;
}

uint32_t
SplineCcNew::SplineCCAlgo(Ptr<TcpSocketState> tcb, uint32_t curr_rtt, uint64_t throughput, uint32_t num_acks)
{
    NS_LOG_FUNCTION(this << curr_rtt << throughput << num_acks);

    if (m_state.last_max_cwnd < m_state.curr_cwnd)
        m_state.last_max_cwnd = m_state.curr_cwnd;

    uint32_t prev_cwnd = m_state.curr_cwnd;

    __epsilone_rtt(curr_rtt, num_acks);
    __bw(throughput, tcb);
    __gamma_ack();

    m_state.last_cwnd = prev_cwnd;
    m_state.next_cwnd = probs(num_acks, tcb);

    if (m_state.bw * tcb->m_segmentSize <= m_state.curr_cwnd && m_state.current_mode == MODE_PROBE_BW)
    {
        m_state.next_cwnd = m_state.bw * tcb->m_segmentSize; // С запасом 100%
    }
    else if (m_state.bw * tcb->m_segmentSize <= m_state.curr_cwnd && m_state.current_mode == MODE_PROBE_RTT)
    {
        m_state.next_cwnd = m_state.bw * tcb->m_segmentSize;
    }

    // Если был CA_LOSS, сохраняем MODE_DRAIN_PROBE до стабилизации RTT
    if (m_state.current_mode == MODE_DRAIN_PROBE && curr_rtt < m_state.last_min_rtt << 1)
    {
        m_state.current_mode = MODE_PROBE_BW;
        NS_LOG_INFO("Exiting MODE_DRAIN_PROBE, switching to MODE_PROBE_BW");
    }

    NS_LOG_INFO("Mode=" << m_state.current_mode << ", cwnd=" << m_state.next_cwnd
                        << ", bw=" << m_state.bw << ", rtt=" << m_state.curr_rtt
                        << ", eps=" << m_state.eps << ", gamma=" << m_state.gamma
                        << ", pacing_rate=" << m_state.pacing_rate);
    return m_state.next_cwnd;
}

uint32_t
SplineCcNew::__epsilone_rtt(uint32_t rtt, uint32_t ack)
{
    if (rtt == 0) rtt = 1; // MIN_RTT = 1 мс
    m_state.last_ack = m_state.curr_ack;
    m_state.curr_ack = ack;
    if (m_state.last_rtt < 1)
        m_state.last_rtt = 1;

    m_state.eps = ((rtt + m_state.curr_rtt) / (rtt ? rtt : 1)) + 1;
    if (m_state.eps > 4) m_state.eps = 4;
    if (m_state.eps < 1) m_state.eps = 1;

    m_state.last_rtt = m_state.curr_rtt;
    m_state.curr_rtt = rtt;

    if (m_state.last_min_rtt > m_state.curr_rtt)
    {
        m_state.last_min_rtt = m_state.curr_rtt;
        m_state.epp_min_rtt++;
    }
    return m_state.eps;
}

uint64_t
SplineCcNew::__bw(uint64_t tp, Ptr<TcpSocketState> tcb)
{
    m_state.rtt_avg = (m_state.last_min_rtt + m_state.last_rtt) >> 1;
    m_state.throughput = tp;
    uint64_t bw = (m_state.throughput * m_state.rtt_avg) / 1000;
    m_state.last_bw = m_state.last_bw;
    m_state.bw = bw / tcb->m_segmentSize;

    if (m_state.last_bw)
    {
        uint64_t min_allowed = (m_state.last_bw * 3) >> 2;
        if (m_state.bw < min_allowed)
            m_state.bw = min_allowed;
        uint64_t max_allowed = (m_state.last_bw * 6) >> 2;
        if (m_state.bw > max_allowed)
            m_state.bw = max_allowed;
        if (m_state.curr_rtt > (m_state.last_min_rtt * 2))
            m_state.bw = m_state.last_bw;
    }
    else
    {
        m_state.bw = std::max(m_state.bw, static_cast<uint64_t>(50)); // Увеличено для старта
    }
    return m_state.bw;
}

uint32_t
SplineCcNew::__gamma_ack()
{
    if (m_state.curr_ack <= m_state.last_ack)
    {
        m_state.gamma = 1;
        return m_state.gamma;
    }
    m_state.gamma = ((m_state.curr_ack + m_state.curr_ack) / m_state.last_ack) + 1;
    if (m_state.gamma > 4) m_state.gamma = 4;
    if (m_state.gamma < 1) m_state.gamma = 1;
    return m_state.gamma;
}

uint32_t
SplineCcNew::stable_rtt_bw(Ptr<TcpSocketState> tcb)
{
    if (m_state.gamma > 2)
    {
        m_state.curr_cwnd = m_state.next_cwnd + (m_state.bw * tcb->m_segmentSize >> 1);
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::fairness_rtt_bw(Ptr<TcpSocketState> tcb)
{
    if (m_state.gamma == 1)
    {
        m_state.curr_cwnd = m_state.curr_cwnd * 14 >> 4; // Смягчённое сокращение
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::overload_rtt_bw(uint32_t ack, Ptr<TcpSocketState> tcb)
{
    if (m_state.gamma <= 1)
    {
        m_state.curr_cwnd = m_state.last_cwnd * 14 >> 4;
        if (ack <= m_state.last_ack)
            m_state.curr_cwnd = m_state.curr_cwnd * 14 >> 4;
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::prob_bw(uint32_t ack, Ptr<TcpSocketState> tcb)
{
    uint32_t stab = stable_rtt_bw(tcb);
    if (stab) return stab;
    uint32_t fairness = fairness_rtt_bw(tcb);
    if (fairness) return fairness;
    uint32_t over = overload_rtt_bw(ack, tcb);
    if (over) return over;
    return m_state.curr_cwnd;
}

uint32_t
SplineCcNew::stable_rtt(Ptr<TcpSocketState> tcb)
{
    if (m_state.eps >= 3 && m_state.gamma == 4)
    {
        m_state.curr_cwnd = m_state.curr_cwnd + (m_state.bw * tcb->m_segmentSize >> 1);
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::overload_rtt(uint32_t ack, Ptr<TcpSocketState> tcb)
{
    if (m_state.eps == 2 && m_state.gamma <= 3)
    {
        m_state.curr_cwnd = m_state.last_cwnd;
        if (ack < m_state.last_ack * 12 >> 4)
            m_state.curr_cwnd = m_state.curr_cwnd - (14 >> 4);
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::fairness_rtt(Ptr<TcpSocketState> tcb)
{
    if (m_state.eps == 2 && m_state.gamma == 4)
    {
        m_state.curr_cwnd = m_state.curr_cwnd * 14 >> 4;
        if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
        return m_state.curr_cwnd;
    }
    return 0;
}

uint32_t
SplineCcNew::prob_rtt(uint32_t ack, Ptr<TcpSocketState> tcb)
{
    uint32_t stab = stable_rtt(tcb);
    if (stab) return stab;
    uint32_t fairness = fairness_rtt(tcb);
    if (fairness) return fairness;
    uint32_t over = overload_rtt(ack, tcb);
    if (over) return over;
    return m_state.curr_cwnd;
}

uint32_t
SplineCcNew::drain_probe(Ptr<TcpSocketState> tcb)
{
    if (m_state.curr_cwnd > m_state.bw * tcb->m_segmentSize)
        m_state.curr_cwnd = m_state.bw * tcb->m_segmentSize;;
    if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
    m_state.curr_cwnd = m_state.curr_cwnd * 12 >> 4;
    return m_state.curr_cwnd;
}

uint32_t
SplineCcNew::start_probe(Ptr<TcpSocketState> tcb)
{
    m_state.curr_cwnd = m_state.curr_cwnd + (m_state.curr_ack >> 1); // Ещё агрессивнее
    if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
    return m_state.curr_cwnd;
}

uint64_t
SplineCcNew::pacing_gain_rate(Ptr<TcpSocketState> tcb)
{
    uint32_t pacing_gain = (m_state.eps + m_state.gamma) / (m_state.eps ? m_state.eps : 1);
    pacing_gain = std::min(std::max(pacing_gain, 1U), 4U);
    m_state.pacing_rate = m_state.bw * tcb->m_segmentSize * pacing_gain * m_state.last_min_rtt / 1000; // Увеличен коэффициент
    if (m_state.current_mode == MODE_PROBE_RTT)
        m_state.pacing_rate = m_state.pacing_rate * 14 >> 4;
    NS_LOG_INFO("pacing_gain=" << pacing_gain << ", pacing_rate=" << m_state.pacing_rate);
    return m_state.pacing_rate;
}

uint32_t
SplineCcNew::cwnd_next_gain(Ptr<TcpSocketState> tcb)
{
    uint32_t cwnd_gain = (m_state.last_cwnd + m_state.last_max_cwnd) / (m_state.last_max_cwnd ? m_state.last_max_cwnd : 1);
    cwnd_gain = std::min(std::max(cwnd_gain, 1U), 4U); // Увеличен максимум
    m_state.curr_cwnd = cwnd_gain * m_state.bw * tcb->m_segmentSize; // Более агрессивный рост
    if (m_state.curr_cwnd < m_state.last_cwnd)
        m_state.curr_cwnd = m_state.last_cwnd * 14 >> 4;
    if (m_state.curr_cwnd < tcb->m_segmentSize) m_state.curr_cwnd = tcb->m_segmentSize;
    NS_LOG_INFO("cwnd_gain=" << cwnd_gain << ", curr_cwnd=" << m_state.curr_cwnd);
    return m_state.curr_cwnd;
}

uint32_t
SplineCcNew::probs(uint32_t ack, Ptr<TcpSocketState> tcb)
{
    if (m_state.epp < 4) { // Увеличено до 15
        m_state.epp++;
    }

    if (!m_state.probe_mode) {
        NS_LOG_INFO("Entering MODE_START_PROBE");
        m_state.probe_mode = 1;
        m_state.current_mode = MODE_START_PROBE;
        return start_probe(tcb);
    }

    if (m_state.eps == 1 || m_state.gamma == 1 && m_state.curr_rtt > (m_state.last_rtt << 1)) { // Увеличен порог
        NS_LOG_INFO("Entering MODE_DRAIN_PROBE due to high RTT");
        m_state.current_mode = MODE_DRAIN_PROBE;
        return drain_probe(tcb);
    }

    if (m_state.epp == 3) {
        m_state.epp = 0;
        if (m_state.epp_min_rtt) {
            m_state.epp_min_rtt = 0;
            m_state.current_mode = MODE_PROBE_BW;
            NS_LOG_INFO("Entering MODE_PROBE_BW due to new min RTT");
        } else {
            switch (m_state.current_mode) {
                case MODE_PROBE_BW:
                    m_state.current_mode = MODE_PROBE_RTT;
                    NS_LOG_INFO("Entering MODE_PROBE_RTT");
                    break;
                case MODE_PROBE_RTT:
                    m_state.current_mode = MODE_DRAIN_PROBE;
                    NS_LOG_INFO("Entering MODE_DRAIN_PROBE");
                    break;
                case MODE_DRAIN_PROBE:
                    m_state.current_mode = MODE_START_PROBE;
                    NS_LOG_INFO("Entering MODE_START_PROBE");
                    break;
                default:
                    m_state.current_mode = MODE_PROBE_BW;
                    NS_LOG_INFO("Entering MODE_PROBE_BW (default)");
                    break;
            }
        }
    }

    switch (m_state.current_mode) {
        case MODE_START_PROBE:
            NS_LOG_INFO("MODE_START_PROBE");
            pacing_gain_rate(tcb);
            return start_probe(tcb);
        case MODE_PROBE_BW:
            NS_LOG_INFO("MODE_PROBE_BW");
            pacing_gain_rate(tcb);
            if (m_state.eps >= 2) {
                return cwnd_next_gain(tcb);
            }
            return prob_bw(ack, tcb);
        case MODE_PROBE_RTT:
            NS_LOG_INFO("MODE_PROBE_RTT");
            pacing_gain_rate(tcb);
            return prob_rtt(ack, tcb);
        case MODE_DRAIN_PROBE:
            NS_LOG_INFO("MODE_DRAIN_PROBE");
            pacing_gain_rate(tcb);
            return drain_probe(tcb);
        default:
            NS_LOG_INFO("MODE_PROBE_BW (fallback)");
            pacing_gain_rate(tcb);
            return prob_bw(ack, tcb);
    }
}

std::string
SplineCcNew::GetName() const
{
    return "SplineCcNew";
}

Ptr<TcpCongestionOps>
SplineCcNew::Fork()
{
    return CreateObject<SplineCcNew>(*this);
}

} // namespace ns3

