#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
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
        : TcpCongestionOps()
    {
        NS_LOG_FUNCTION(this);
        m_state =
        {
            .current_mode = MODE_START_PROBE,
        };
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

    void SplineCcNew::SplineCCAlgo(Ptr<TcpSocketState> tcb, uint32_t curr_rtt, uint64_t throughput, uint32_t num_acks) {
        NS_LOG_FUNCTION(this << tcb << curr_rtt << throughput << num_acks);
        if (!tcb || tcb->m_segmentSize == 0) 
        {
            tcb->m_cWnd = m_state.curr_cwnd * tcb->m_segmentSize; // Переводим в байты
            NS_LOG_DEBUG("Invalid tcb or segmentSize=0, setting tcb->m_cWnd = " << tcb->m_cWnd);
            return;
        }

        __epsilone_rtt(tcb);
        m_state.bw = __bw(tcb);
        uint32_t cwnd_segments = probs(tcb);
        tcb->m_cWnd = cwnd_segments * tcb->m_segmentSize;
        m_state.curr_cwnd = cwnd_segments; // Сохраняем в сегментах
        NS_LOG_DEBUG("probs(tcb) = " << cwnd_segments << ", tcb->m_cWnd = " << tcb->m_cWnd);

        if (m_state.last_max_cwnd < m_state.curr_cwnd)
        {
            m_state.last_max_cwnd = m_state.curr_cwnd;
        }
        if (m_state.next_cwnd == 0)
        {
            m_state.next_cwnd = m_state.curr_cwnd;
        }
    }

    void SplineCcNew::__epsilone_rtt(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (m_state.last_min_rtt > m_state.curr_rtt && m_state.curr_rtt > 0)
        {
            m_state.last_min_rtt = m_state.curr_rtt;
            m_state.epp_min_rtt++;
        }
    }

    uint64_t SplineCcNew::__bw(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.bw;
        }
        if (tcb->m_minRtt.GetSeconds() == 0)
        {
            m_state.throughput = 0;
        }
        else
        {
            m_state.throughput = tcb->m_bytesInFlight / tcb->m_minRtt.GetSeconds();
        }
        if (tcb->m_minRtt.GetSeconds() == 0)
        {
            m_state.bw = 0;
        }
        else
        {
            m_state.bw = m_state.curr_ack * tcb->m_segmentSize / tcb->m_minRtt.GetSeconds();
        }

        if (tcb->m_bytesInFlight == 0) {
            m_state.fairness_rat = 2;
        }
        else
        {
            double numerator = static_cast<double>(m_state.curr_cwnd) * m_state.curr_cwnd * tcb->m_segmentSize;
            double denominator = 2.0 * tcb->m_bytesInFlight * tcb->m_bytesInFlight;
            m_state.fairness_rat = static_cast<uint32_t>(numerator / denominator + 1.0);
        }
        if (m_state.throughput * 12 >> 4 > m_state.bw)
        {
            m_state.current_mode = MODE_DRAIN_PROBE;
        }
        if (m_state.last_bw)
        {
            uint64_t min_allowed = (m_state.last_bw * 3) >> 2;
            if (m_state.bw < min_allowed)
            {
                m_state.bw = min_allowed;
            }
            uint64_t max_allowed = (m_state.last_bw * 6) >> 2;
            if (m_state.bw > max_allowed)
            {
                m_state.bw = max_allowed;
            }
            if (m_state.curr_rtt > (m_state.last_min_rtt << 1))
            {
                m_state.bw = m_state.last_bw;
            }
        }
        else
        {
            m_state.bw = std::max(m_state.bw, static_cast<uint64_t>(tcb->m_initialCWnd));
        }
        NS_LOG_DEBUG("Calculated bw = " << m_state.bw);
        return m_state.bw;
    }

    uint32_t SplineCcNew::stable_rtt_bw(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (m_state.fairness_rat >= 2 || (tcb->m_bytesInFlight << 1) < m_state.curr_cwnd)
        {
            m_state.curr_cwnd = m_state.curr_cwnd * 18 >> 4;
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::fairness_rtt_bw(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (m_state.fairness_rat < 2) {
            m_state.curr_cwnd = m_state.curr_cwnd * 8 >> 4;
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, 1U);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::overload_rtt_bw(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (tcb->m_congState == TcpSocketState::CA_LOSS || tcb->m_bytesInFlight > m_state.curr_cwnd)
        {
            m_state.curr_cwnd = m_state.curr_cwnd * 10 >> 4;
            if (m_state.curr_ack <= m_state.last_ack)
            {
                m_state.curr_cwnd = (tcb->m_cWnd.Get() / tcb->m_segmentSize) * 8 >> 4; // Переводим в сегменты
            }
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::prob_bw(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        uint32_t stab = stable_rtt_bw(tcb);
        if (stab) return stab;
        uint32_t fairness = fairness_rtt_bw(tcb);
        if (fairness) return fairness;
        uint32_t over = overload_rtt_bw(tcb);
        if (over) return over;
        return m_state.curr_cwnd;
    }

    uint32_t SplineCcNew::stable_rtt(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (m_state.fairness_rat >= 2 || (tcb->m_bytesInFlight << 1) < m_state.curr_cwnd)
        {
            m_state.curr_cwnd = m_state.curr_cwnd;
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::overload_rtt(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (tcb->m_congState == TcpSocketState::CA_LOSS || tcb->m_bytesInFlight > m_state.curr_cwnd)
        {
            m_state.curr_cwnd = m_state.curr_cwnd * 8 >> 4;
            if (m_state.curr_ack < m_state.last_ack * 3 >> 2)
            {
                m_state.curr_cwnd = (tcb->m_cWnd.Get() / tcb->m_segmentSize) * 8 >> 4; // Переводим в сегменты
            }
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::fairness_rtt(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd;
        }
        if (m_state.fairness_rat < 2)
        {
            m_state.curr_cwnd = m_state.curr_cwnd * 8 >> 4;
            m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
            return m_state.curr_cwnd;
        }
        return 0;
    }

    uint32_t SplineCcNew::prob_rtt(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        uint32_t stab = stable_rtt(tcb);
        if (stab) return stab;
        uint32_t fairness = fairness_rtt(tcb);
        if (fairness) return fairness;
        uint32_t over = overload_rtt(tcb);
        if (over) return over;
        return m_state.curr_cwnd;
    }

    uint32_t SplineCcNew::drain_probe(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd ? m_state.curr_cwnd : 2;
        }
        if (m_state.curr_cwnd > m_state.bw) {
            m_state.curr_cwnd = m_state.bw;
        }
        m_state.curr_cwnd = m_state.curr_cwnd * 12 >> 4;
        m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
        return m_state.curr_cwnd;
    }

    uint32_t SplineCcNew::start_probe(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd ? m_state.curr_cwnd : m_state.last_cwnd;
        }
        // Увеличиваем cwnd на основе подтвержденных сегментов
        m_state.curr_cwnd += tcb->m_lastAckedSackedBytes / tcb->m_segmentSize;
        m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
        // Ограничиваем максимальное значение
        uint32_t MAX_CWND_SEGMENTS = m_state.fairness_rat * (m_state.bw - (m_state.bw * 14 >> 4)) * (tcb->m_minRtt.GetSeconds() ? 
            tcb->m_minRtt.GetSeconds() : 1);
        MAX_CWND_SEGMENTS = MAX_CWND_SEGMENTS ? MAX_CWND_SEGMENTS :  m_state.curr_cwnd;
        m_state.curr_cwnd = std::min(m_state.curr_cwnd, MAX_CWND_SEGMENTS);
        NS_LOG_DEBUG("start_probe: m_state.curr_cwnd = " << m_state.curr_cwnd);
        return m_state.curr_cwnd;
    }

    uint64_t SplineCcNew::pacing_gain_rate(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb->m_pacing)
            tcb->m_pacing = true;

        uint32_t pacing_gain = m_state.fairness_rat;
        m_state.pacing_rate = m_state.bw * pacing_gain * tcb->m_minRtt.GetSeconds();
        if (m_state.current_mode == MODE_PROBE_RTT) {
            m_state.pacing_rate = m_state.pacing_rate * 14 >> 4;
        }
        tcb->m_pacingRate = DataRate(m_state.pacing_rate);
        NS_LOG_DEBUG("pacing_rate = " << m_state.pacing_rate);
        return m_state.pacing_rate;
    }

    uint32_t SplineCcNew::cwnd_next_gain(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0) {
            return m_state.curr_cwnd ? m_state.curr_cwnd : 2;
        }
        double cwnd_gain = static_cast<double>(tcb->m_cWnd.Get()) / (m_state.bw * tcb->m_minRtt.GetSeconds());
        m_state.curr_cwnd = static_cast<uint32_t>(cwnd_gain * m_state.bw * tcb->m_minRtt.GetSeconds() / tcb->m_segmentSize);
        if (m_state.curr_cwnd > tcb->m_cWnd / tcb->m_segmentSize)
            m_state.curr_cwnd = tcb->m_cWnd.Get() / tcb->m_segmentSize;

        m_state.curr_cwnd = std::max(m_state.curr_cwnd, m_state.last_cwnd);
        const uint32_t MAX_CWND_SEGMENTS = 1000;
        m_state.curr_cwnd = std::min(m_state.curr_cwnd, m_state.last_max_cwnd);
        NS_LOG_DEBUG("cwnd_next_gain: m_state.curr_cwnd = " << MAX_CWND_SEGMENTS);
        return m_state.curr_cwnd;
    }

    uint32_t SplineCcNew::probs(Ptr<TcpSocketState> tcb)
    {
        NS_LOG_FUNCTION(this << tcb);
        if (!tcb || tcb->m_segmentSize == 0)
        {
            return m_state.curr_cwnd ? m_state.curr_cwnd : 2;
        }

        if (m_state.epp < 10)
        {
            m_state.epp++;
        }

        if (!m_state.probe_mode)
        {
            NS_LOG_INFO("Entering MODE_START_PROBE");
            m_state.probe_mode = 1;
            m_state.current_mode = MODE_START_PROBE;
            uint32_t cwnd = start_probe(tcb);
            if (m_state.curr_cwnd > tcb->m_bytesInFlight / tcb->m_segmentSize)
                m_state.curr_cwnd = m_state.curr_cwnd * 10 >> 4;
            NS_LOG_DEBUG("probs: MODE_START_PROBE, cwnd = " << m_state.curr_cwnd);
            return m_state.curr_cwnd;
        }

        if ((tcb->m_bytesInFlight > m_state.curr_ack * tcb->m_segmentSize &&
            tcb->m_bytesInFlight > m_state.curr_cwnd) || tcb->m_lastAckedSackedBytes < tcb->m_segmentSize)
        {
            NS_LOG_INFO("Entering MODE_DRAIN_PROBE due to high RTT");
            m_state.current_mode = MODE_DRAIN_PROBE;
        }

        if (m_state.epp == 9)
        {
            m_state.epp = 0;
            if (m_state.epp_min_rtt)
            {
                m_state.epp_min_rtt = 0;
                m_state.current_mode = MODE_PROBE_BW;
                NS_LOG_INFO("Entering MODE_PROBE_BW due to new min RTT");
            }
            else
            {
                switch (m_state.current_mode)
                {
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

        switch (m_state.current_mode)
        {
        case MODE_START_PROBE:
            NS_LOG_INFO("MODE_START_PROBE");
            return start_probe(tcb);
        case MODE_PROBE_BW:
            NS_LOG_INFO("MODE_PROBE_BW");
            prob_bw(tcb);
            pacing_gain_rate(tcb);
            return cwnd_next_gain(tcb);
        case MODE_PROBE_RTT:
            NS_LOG_INFO("MODE_PROBE_RTT");
            prob_rtt(tcb);
            pacing_gain_rate(tcb);
            return cwnd_next_gain(tcb);
        case MODE_DRAIN_PROBE:
            NS_LOG_INFO("MODE_DRAIN_PROBE");
            drain_probe(tcb);
            pacing_gain_rate(tcb);
            return cwnd_next_gain(tcb);
        default:
            NS_LOG_INFO("MODE_PROBE_BW (fallback)");
            prob_bw(tcb);
            pacing_gain_rate(tcb);
            return cwnd_next_gain(tcb);
        }
    }

    uint32_t SplineCcNew::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
    {
        NS_LOG_FUNCTION(this << tcb << bytesInFlight);
        uint32_t ssthresh = std::max(m_state.curr_cwnd * 14 >> 4, 1U);
        NS_LOG_DEBUG("GetSsThresh: ssthresh = " << ssthresh);
        return ssthresh;
    }

    void SplineCcNew::PktsAcked(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt) {
        NS_LOG_FUNCTION(this << tcb << segmentsAcked << rtt.GetSeconds());
        if (!tcb || tcb->m_segmentSize == 0) {
            return;
        }
        if (rtt.IsZero())
        {
            return;
        }

        m_state.curr_rtt = rtt.GetSeconds();
        if (m_state.last_min_rtt == 0 || m_state.curr_rtt < m_state.last_min_rtt) {
            m_state.last_min_rtt = m_state.curr_rtt;
        }

        m_state.last_ack = m_state.curr_ack;
        m_state.curr_ack = segmentsAcked;
        NS_LOG_DEBUG("PktsAcked: curr_rtt = " << m_state.curr_rtt << ", curr_ack = " << m_state.curr_ack);
    }

    void SplineCcNew::CwndEvent(Ptr<TcpSocketState> tcb, const ns3::TcpSocketState::TcpCAEvent_t event)
    {
        NS_LOG_FUNCTION(this << tcb << event);
        if (!tcb) {
            NS_LOG_INFO("CwndEvent: tcb is null");
            return;
        }
        switch (event) {
        case ns3::TcpSocketState::CA_EVENT_CWND_RESTART:
            m_state.curr_cwnd = tcb->m_initialCWnd; // Используем начальное значение в сегментах
            m_state.current_mode = MODE_START_PROBE;
            NS_LOG_INFO("CwndEvent: CA_EVENT_CWND_RESTART, entering MODE_START_PROBE, curr_cwnd = " << m_state.curr_cwnd);
            break;
        default:
            break;
        }
    }

    void SplineCcNew::IncreaseWindow(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) {
        NS_LOG_FUNCTION(this << tcb << segmentsAcked);
        if (!tcb || tcb->m_segmentSize == 0) {
            return;
        }

        if (tcb->m_congState != TcpSocketState::CA_OPEN &&
            tcb->m_congState != TcpSocketState::CA_RECOVERY &&
            tcb->m_congState != TcpSocketState::CA_LOSS) {
            return;
        }

        m_state.curr_rtt = tcb->m_lastRtt.Get().GetSeconds();
        m_state.last_cwnd = m_state.curr_cwnd;
        m_state.curr_cwnd = tcb->m_cWnd.Get() / tcb->m_segmentSize; // Переводим в сегменты
        NS_LOG_DEBUG("IncreaseWindow: curr_cwnd = " << m_state.curr_cwnd << ", tcb->m_cWnd = " << tcb->m_cWnd.Get());

        if (tcb->m_congState == TcpSocketState::CA_OPEN ||
            tcb->m_congState == TcpSocketState::CA_RECOVERY ||
            tcb->m_congState == TcpSocketState::CA_LOSS) {
            SplineCCAlgo(tcb, m_state.curr_rtt, m_state.throughput, segmentsAcked);
        }
    }

    std::string SplineCcNew::GetName() const
    {
        return "SplineCcNew";
    }

    Ptr<TcpCongestionOps> SplineCcNew::Fork()
    {
        NS_LOG_FUNCTION(this);
        return CopyObject<SplineCcNew>(this);
    }

} // namespace ns3
