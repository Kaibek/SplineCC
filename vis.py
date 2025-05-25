import xml.etree.ElementTree as ET
import pandas as pd
import matplotlib.pyplot as plt
import os

# Путь к файлу FlowMonitor
flowmon_file = "bbr_spline.flowmon"

# Проверка существования файла
if not os.path.exists(flowmon_file):
    print(f"Ошибка: Файл {flowmon_file} не найден!")
    exit(1)

# Парсинг XML
try:
    tree = ET.parse(flowmon_file)
    root = tree.getroot()
except ET.ParseError as e:
    print(f"Ошибка парсинга XML: {e}")
    exit(1)

# Функция для извлечения метрик потока
def extract_flow_metrics(flow_stats, flow_id):
    try:
        tx_bytes = int(flow_stats.get("txBytes"))
        rx_bytes = int(flow_stats.get("rxBytes"))
        tx_packets = int(flow_stats.get("txPackets"))
        rx_packets = int(flow_stats.get("rxPackets"))
        lost_packets = int(flow_stats.get("lostPackets"))
        time_first_rx = float(flow_stats.get("timeFirstRxPacket").replace("+", "").replace("ns", "")) / 1e9
        time_last_rx = float(flow_stats.get("timeLastRxPacket").replace("+", "").replace("ns", "")) / 1e9
        duration = time_last_rx - time_first_rx
        throughput_mbps = (rx_bytes * 8) / duration / 1e6 if duration > 0 else 0
        loss_percentage = (lost_packets / tx_packets * 100) if tx_packets > 0 else 0
        return {
            "flow_id": flow_id,
            "tx_bytes": tx_bytes,
            "rx_bytes": rx_bytes,
            "tx_packets": tx_packets,
            "rx_packets": rx_packets,
            "lost_packets": lost_packets,
            "throughput_mbps": throughput_mbps,
            "loss_percentage": loss_percentage
        }
    except (AttributeError, ValueError) as e:
        print(f"Ошибка при извлечении метрик для Flow {flow_id}: {e}")
        return None

# Извлечение данных для обоих потоков
flows = []
for flow_id in ["1", "2"]:
    flow_stats = root.find(f".//Flow[@flowId='{flow_id}']")
    if flow_stats is None:
        print(f"Flow {flow_id} не найден в файле FlowMonitor!")
        continue
    metrics = extract_flow_metrics(flow_stats, flow_id)
    if metrics:
        flows.append(metrics)

flows_df = pd.DataFrame(flows)

# Гистограммы задержек и прерываний
def extract_histogram(flow_stats, hist_type):
    histogram = []
    hist_elem = flow_stats.find(hist_type)
    if hist_elem is not None:
        for bin_elem in hist_elem.findall("bin"):
            try:
                start = float(bin_elem.get("start"))
                count = int(bin_elem.get("count"))
                histogram.append({"start": start, "count": count})
            except (AttributeError, ValueError) as e:
                print(f"Предупреждение: Пропущен bin в {hist_type}: {e}")
    return pd.DataFrame(histogram) if histogram else pd.DataFrame({"start": [], "count": []})

delay_histograms = {}
interruptions_histograms = {}
for flow_id in ["1", "2"]:
    flow_stats = root.find(f".//Flow[@flowId='{flow_id}']")
    if flow_stats is not None:
        delay_histograms[flow_id] = extract_histogram(flow_stats, "delayHistogram")
        interruptions_histograms[flow_id] = extract_histogram(flow_stats, "flowInterruptionsHistogram")

# Парсинг трассировок
def parse_trace_file(trace_file, columns):
    if not os.path.exists(trace_file):
        print(f"Файл {trace_file} не найден!")
        return None
    try:
        df = pd.read_csv(trace_file, sep=" ", names=columns, header=None)
        return df
    except Exception as e:
        print(f"Ошибка парсинга {trace_file}: {e}")
        return None

# Загрузка трассировок
bbr_cwnd_df = parse_trace_file("bbr_cwnd_trace.txt", ["time", "cwnd"])
spline_cwnd_df = parse_trace_file("spline_cwnd_trace.txt", ["time", "cwnd"])
bbr_rtt_df = parse_trace_file("bbr_rtt_trace.txt", ["time", "rtt_ms"])
spline_rtt_df = parse_trace_file("spline_rtt_trace.txt", ["time", "rtt_ms"])
bbr_pacing_df = parse_trace_file("bbr_pacing_rate_trace.txt", ["time", "pacing_mbps"])
spline_pacing_df = parse_trace_file("spline_pacing_rate_trace.txt", ["time", "pacing_mbps"])
bbr_rx_df = parse_trace_file("bbr_rx_trace.txt", ["time", "size"])
spline_rx_df = parse_trace_file("spline_rx_trace.txt", ["time", "size"])

# Построение графиков
plt.figure(figsize=(15, 12))

# 1. Пропускная способность
plt.subplot(3, 2, 1)
if not flows_df.empty:
    labels = ["BBR" if row["flow_id"] == "1" else "SplineCcNew" for _, row in flows_df.iterrows()]
    plt.bar(labels, flows_df["throughput_mbps"], color=["#007bff", "#28a745"])
    for i, row in flows_df.iterrows():
        plt.text(i, row["throughput_mbps"] * 0.9, f"Loss: {row['loss_percentage']:.2f}%", color="red", ha="center")
    plt.title("Throughput Comparison")
    plt.ylabel("Throughput (Mbps)")
else:
    plt.text(0.5, 0.5, "No throughput data", ha="center", va="center")
    plt.title("Throughput Comparison")
    plt.ylabel("Throughput (Mbps)")

# 2. Гистограмма задержек
plt.subplot(3, 2, 3)
has_delay_data = False
if not delay_histograms["1"].empty:
    plt.bar(delay_histograms["1"]["start"], delay_histograms["1"]["count"], width=0.0005, color="#007bff", alpha=0.5, label="BBR")
    has_delay_data = True
if not delay_histograms["2"].empty:
    plt.bar(delay_histograms["2"]["start"], delay_histograms["2"]["count"], width=0.0005, color="#28a745", alpha=0.5, label="SplineCcNew")
    has_delay_data = True
if has_delay_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No delay histogram data", ha="center", va="center")
plt.title("Delay Histogram")
plt.xlabel("Delay (seconds)")
plt.ylabel("Packet Count")

# 3. Гистограмма прерываний
plt.subplot(3, 2, 5)
has_interruptions_data = False
if not interruptions_histograms["1"].empty:
    plt.bar(interruptions_histograms["1"]["start"], interruptions_histograms["1"]["count"], width=0.125, color="#007bff", alpha=0.5, label="BBR")
    has_interruptions_data = True
if not interruptions_histograms["2"].empty:
    plt.bar(interruptions_histograms["2"]["start"], interruptions_histograms["2"]["count"], width=0.125, color="#28a745", alpha=0.5, label="SplineCcNew")
    has_interruptions_data = True
if has_interruptions_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No interruptions histogram data", ha="center", va="center")
plt.title("Flow Interruptions Histogram")
plt.xlabel("Interruption Duration (seconds)")
plt.ylabel("Count")

# 4. Окно перегрузки
plt.subplot(3, 2, 2)
has_cwnd_data = False
if bbr_cwnd_df is not None and not bbr_cwnd_df.empty:
    plt.plot(bbr_cwnd_df["time"], bbr_cwnd_df["cwnd"], color="#007bff", label="BBR")
    has_cwnd_data = True
if spline_cwnd_df is not None and not spline_cwnd_df.empty:
    plt.plot(spline_cwnd_df["time"], spline_cwnd_df["cwnd"], color="#28a745", label="SplineCcNew")
    has_cwnd_data = True
if has_cwnd_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No cwnd data", ha="center", va="center")
plt.title("Congestion Window over Time")
plt.xlabel("Time (seconds)")
plt.ylabel("Cwnd (bytes)")

# 5. RTT
plt.subplot(3, 2, 4)
has_rtt_data = False
if bbr_rtt_df is not None and not bbr_rtt_df.empty:
    plt.plot(bbr_rtt_df["time"], bbr_rtt_df["rtt_ms"], color="#007bff", label="BBR")
    has_rtt_data = True
if spline_rtt_df is not None and not spline_rtt_df.empty:
    plt.plot(spline_rtt_df["time"], spline_rtt_df["rtt_ms"], color="#28a745", label="SplineCcNew")
    has_rtt_data = True
if has_rtt_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No RTT data", ha="center", va="center")
plt.title("RTT over Time")
plt.xlabel("Time (seconds)")
plt.ylabel("RTT (ms)")

# 6. Pacing Rate
plt.subplot(3, 2, 6)
has_pacing_data = False
if bbr_pacing_df is not None and not bbr_pacing_df.empty:
    plt.plot(bbr_pacing_df["time"], bbr_pacing_df["pacing_mbps"], color="#007bff", label="BBR")
    has_pacing_data = True
if spline_pacing_df is not None and not spline_pacing_df.empty:
    plt.plot(spline_pacing_df["time"], spline_pacing_df["pacing_mbps"], color="#28a745", label="SplineCcNew")
    has_pacing_data = True
if has_pacing_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No pacing rate data", ha="center", va="center")
plt.title("Pacing Rate over Time")
plt.xlabel("Time (seconds)")
plt.ylabel("Pacing Rate (Mbps)")

plt.tight_layout()
plt.savefig("bbr_spline_comparison.png")
print("Графики сохранены в bbr_spline_comparison.png")
plt.show()

# Throughput over time
plt.figure(figsize=(10, 4))
has_rx_data = False
if bbr_rx_df is not None and not bbr_rx_df.empty:
    bbr_rx_df["cumulative_bytes"] = bbr_rx_df["size"].cumsum()
    bbr_rx_df["throughput_mbps"] = (bbr_rx_df["cumulative_bytes"] * 8) / (bbr_rx_df["time"] - bbr_rx_df["time"].iloc[0]) / 1e6
    plt.plot(bbr_rx_df["time"], bbr_rx_df["throughput_mbps"], color="#007bff", label="BBR")
    has_rx_data = True
if spline_rx_df is not None and not spline_rx_df.empty:
    spline_rx_df["cumulative_bytes"] = spline_rx_df["size"].cumsum()
    spline_rx_df["throughput_mbps"] = (spline_rx_df["cumulative_bytes"] * 8) / (spline_rx_df["time"] - spline_rx_df["time"].iloc[0]) / 1e6
    plt.plot(spline_rx_df["time"], spline_rx_df["throughput_mbps"], color="#28a745", label="SplineCcNew")
    has_rx_data = True
if has_rx_data:
    plt.legend()
else:
    plt.text(0.5, 0.5, "No throughput data", ha="center", va="center")
plt.title("Throughput over Time")
plt.xlabel("Time (seconds)")
plt.ylabel("Throughput (Mbps)")
plt.savefig("throughput_time.png")
print("График пропускной способности сохранён в throughput_time.png")
plt.show()
