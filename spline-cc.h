#ifndef SPLINE_CC_NEW_H
#define SPLINE_CC_NEW_H

#include "ns3/tcp-congestion-ops.h"

namespace ns3 {

class SplineCcNew : public TcpCongestionOps {
public:
    static TypeId GetTypeId(void);
    SplineCcNew();
    SplineCcNew(const SplineCcNew& other);
    virtual ~SplineCcNew() override;

    virtual void IncreaseWindow(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;
    virtual uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    void OnPacketLoss(Ptr<TcpSocketState> tcb, const SequenceNumber32& seq);
    virtual Ptr<TcpCongestionOps> Fork() override;
    virtual std::string GetName() const override;
    void PktsAcked(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt) override;
    void CwndEvent(Ptr<TcpSocketState> tcb, const ns3::TcpSocketState::TcpCAEvent_t event) override;

private:
    enum {
        MODE_START_PROBE = 0,
        MODE_PROBE_BW = 1,
        MODE_PROBE_RTT = 2,
        MODE_DRAIN_PROBE = 3
    };

    struct {
        uint32_t drain_probe_count{0};
        uint32_t fairness_rat;
        uint32_t last_rtt;
        uint32_t last_min_rtt;
        uint32_t curr_rtt;
        uint32_t rtt_avg;
        uint64_t throughput;
        uint64_t bw;
        uint64_t last_bw;
        uint32_t curr_cwnd;
        uint32_t last_cwnd;
        uint32_t last_max_cwnd;
        uint32_t next_cwnd;
        uint32_t eps;
        uint32_t gamma;
        uint32_t curr_ack;
        uint32_t last_ack;
        uint64_t pacing_rate;
        uint32_t current_mode;
        uint32_t probe_mode;
        uint32_t epp;
        uint32_t epp_min_rtt;
    } m_state;

    void     SplineCCAlgo(Ptr<TcpSocketState> tcb, uint32_t curr_rtt, uint64_t throughput, uint32_t num_acks);
    void     __epsilone_rtt(Ptr<TcpSocketState> tcb);
    uint64_t __bw(Ptr<TcpSocketState> tcb);
    uint32_t __gamma_ack(Ptr<TcpSocketState> tcb);
    uint32_t stable_rtt_bw(Ptr<TcpSocketState> tcb);
    uint32_t fairness_rtt_bw(Ptr<TcpSocketState> tcb);
    uint32_t overload_rtt_bw(Ptr<TcpSocketState> tcb);
    uint32_t prob_bw(Ptr<TcpSocketState> tcb);
    uint32_t stable_rtt(Ptr<TcpSocketState> tcb);
    uint32_t overload_rtt(Ptr<TcpSocketState> tcb);
    uint32_t fairness_rtt(Ptr<TcpSocketState> tcb);
    uint32_t prob_rtt(Ptr<TcpSocketState> tcb);
    uint32_t drain_probe(Ptr<TcpSocketState> tcb);
    uint32_t start_probe(Ptr<TcpSocketState> tcb);
    uint64_t pacing_gain_rate(Ptr<TcpSocketState> tcb);
    uint32_t cwnd_next_gain(Ptr<TcpSocketState> tcb);
    uint32_t probs(Ptr<TcpSocketState> tcb);
};

} // namespace ns3

#endif // SPLINE_CC_NEW_H
