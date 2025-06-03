# SplineCC: Адаптивный алгоритм управления перегрузкой TCP

## Обзор
SplineCC — это передовой алгоритм управления перегрузкой TCP, реализованный на языке C (`SplineCcNew.cc`) в среде симулятора NS-3. Алгоритм разработан для оптимизации производительности сетей с переменными условиями, такими как высокие потери пакетов, задержки и изменяющаяся пропускная способность. SplineCC сочетает элементы bandwidth-delay-based (подобно BBR) и loss-based (подобно CUBIC) подходов, используя четыре адаптивных режима работы: `START_PROBE`, `PROBE_BW`, `PROBE_RTT` и `DRAIN_PROBE`. Он подходит для высокоскоростных WAN, беспроводных сетей и сценариев с интенсивным трафиком, а также представляет интерес для исследователей и разработчиков сетевых протоколов.

SplineCC демонстрирует выдающуюся производительность, превосходя BBR в условиях высоких потерь (до 10%) и стабильных задержек, как показано в тестах NS-3. Алгоритм оптимизирован для работы как в одиночных потоках, так и в конкурентных сценариях.

## Особенности
- **Многорежимная адаптация**:
  - `START_PROBE`: Быстро увеличивает окно перегрузки (`cwnd`) для захвата доступной полосы на начальном этапе.
  - `PROBE_BW`: Настраивает `cwnd` на основе оценённой пропускной способности (`bw`) с использованием экспоненциального скользящего среднего (EMA).
  - `PROBE_RTT`: Минимизирует задержки, корректируя `cwnd` для измерения минимального RTT.
  - `DRAIN_PROBE`: Уменьшает избыточное окно при обнаружении перегрузки (рост RTT или снижение ACK).
- **Оценка состояния сети**: Использует метрики `eps` (на основе RTT, обновляет минимальный RTT за период эпохи) и `fairness_rat` (на основе ACK и `cwnd`) для динамической адаптации.
- **Управление перегрузкой**: Переходит в режим избегания перегрузки при достижении потерь или переполнение буффер inflight.
- **Pacing & fairness_rat**: Регулирует скорость отправки пакетов для сглаживания трафика и предотвращения перегрузки.
- **Оптимизация**: Минимизирует вычислительные затраты за счёт целочисленных операций и сдвигов.

## Технические детали
### Вычисления
Функция `SplineCCAlgo` вычисляет следующее окно перегрузки (`next_cwnd`) на основе параметров: `curr_rtt`, `throughput`, `num_acks` и структуры состояния `m_state`. Пример вычислений:
- Входные данные: `curr_rtt = 12` мс, `throughput = 125000000` байт/с, `num_acks = 10`.
- Начальное состояние: `last_min_rtt = 100` мс, `last_cwnd = 10`, `curr_cwnd = 64`, `bw = 0`.

#### Пошаговый процесс:
1. **Обновление RTT (`__epsilone_rtt`)**:
   - `last_min_rtt = min(last_min_rtt, curr_rtt) = 12` мс.
   - Увеличивает `epp_min_rtt`, если обнаружен новый минимум.
2. **Оценка пропускной способности (`__bw`)**:
   - `bw = (curr_ack * segmentSize) / minRtt = (10 * 1460) / 0.012 ≈ 1,216,667` байт/с.
   - Применяется EMA: `last_bw = (last_bw * 3 + bw) >> 2 ≈ 304,167` байт/с.
   - Фильтрация: `bw` ограничено 75–150% от `last_bw`.
3. **Режимы работы (`probs`)**:
   - `START_PROBE`: `curr_cwnd += num_acks = 64 + 10 = 74`.
   - `PROBE_BW`: `curr_cwnd = max(bw / segmentSize, curr_cwnd) ≈ 208`.
   - `PROBE_RTT`: `curr_cwnd = bw / segmentSize + (num_acks >> 2) ≈ 210`.
   - `DRAIN_PROBE`: `curr_cwnd = curr_cwnd * 0.75 ≈ 156`.
4. **Pacing (`pacing_gain_rate`)**:
   - `pacing_rate = bw * fairness_rat * minRtt ≈ 1,216,667 * 2 * 0.012 ≈ 29,200,008` байт/с.

#### Результат:
- `next_cwnd = 208` сегментов, `pacing_rate ≈ 29.2 Мбит/с`.

### Режимы переключения
- Переключение между режимами происходит каждые 9 эпох (`epp == 9`) или при перегрузке (RTT > 2 * `last_min_rtt`или `fairness_rat < 2`).

## Установка
Для использования SplineCC требуется NS-3 (версия 3.36 или выше) с установленными зависимостями.

1. Клонируйте репозиторий:
   ```bash
   git clone https://github.com/Kaibek/SplineCC.git
   cd SplineCC
   ```
2. Скомпилируйте NS-3 с поддержкой SplineCC:
   ```bash
   ./waf configure --enable-examples --enable-tests
   ./waf build
   ```
3. Интегрируйте `SplineCcNew.cc` в проект NS-3, добавив его в `src/internet/model/` и обновив `wscript`.

## Использование
### Интеграция в NS-3
1. `SplineCcNew` уже находится в конфигурации TCP

2. Инициализируйте симуляцию с вашими параметрами (например, `errorRate = 0.1`, `routerRate = 500Mbps`). Имеется уже тестовый файл:
   ```bash
   nano scratch/dumbbell-my.cc
   ```

### Пример кода
```cpp
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/spline-cc.h"

int main() {
    NodeContainer nodes;
    nodes.Create(2);
    InternetStackHelper stack;
    stack.Install(nodes);
    Config::SetDefault("ns3::TcpSocket::CongestionAlgo", StringValue("ns3::SplineCcNew"));
    // Настройка сети и приложений...
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
```

## Тестирование
### Рекомендации
1. **Симуляция в NS-3**:
   - Тестируйте с разными `errorRate` (0.01%, 5%, 10%) и `routerDelay` (10 мс, 80 мс, 500 мс).
   - Используйте `FlowMonitor` для анализа пропускной способности, RTT и потерь.
2. **Реальные тесты**:
   - Интегрируйте в Linux TCP stack (например, через `tc` или модули ядра) и протестируйте в реальной сети.
3. **Краевые случаи**:
   - Проверяйте поведение при `curr_rtt > 1s`, `num_acks = 0` или высоких потерях (>20%).
4. **Запуск симуляции**
   - достаточно перейти в дирректорию `SplineCC/build` и запуск теста `./scratch/ns3.40-dumbbell-my-default`

### Пример сценария NS-3
```cpp
uint32_t nLeaf = 2;
double errorRate = 0.1;
std::string routerRate = "500Mbps";
std::string routerDelay = "80ms";
PointToPointHelper pointToPoint;
pointToPoint.SetDeviceAttribute("DataRate", StringValue(routerRate));
pointToPoint.SetChannelAttribute("Delay", StringValue(routerDelay));
NetDeviceContainer devices = pointToPoint.Install(nodes);
Ipv4AddressHelper address;
address.SetBase("10.1.1.0", "255.255.255.0");
Ipv4InterfaceContainer interfaces = address.Assign(devices);
```

## Сравнение с другими алгоритмами
SplineCCNew протестирован в NS-3 в двух сценариях:
1. **Один канал**: Один поток на 500 Мбит/с, 80 мс задержка, 10% потерь.
2. **Разные каналы**: Сравнение с BBR при 10,000 Мбит/с, 50 мс RTT, 0.01% потерь.

### Результаты
#### Один канал
- **Пропускная способность**: `SplineCcNew` — (0.25 - 0.26) Мбит/с, BBR — (0.02 - 0.01) Мбит/с (12.5x лучше).
- **RTT**: `SplineCcNew` — 200-300 мс (стабильный), BBR — до 3000 - 450 мс (начало).
- **Задержка**: `SplineCcNew` — 10 мс, BBR — 10.5 мс.
- **Потери**: `SplineCcNew` — 9.83%, BBR — 8.21%.
- **Вывод**: `SplineCcNew` превосходит BBR в условиях высоких потерь благодаря стабильному `cwnd` и pacing.

### Графики
- **[Один канал]**: [Throughput, CWND, RTT, Delay, Interruptions, Pacing]
![image](https://github.com/user-attachments/assets/7285f39b-2851-468a-95d2-77fddf18d75e)


## Автор
**Калимоллаев Бекжан**

## Контакты
Для вопросов и сотрудничества: [bekzhankalimollaev777@gmail.com](mailto:bekzhankalimollaev777@gmail.com).

----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
# SplineCC: Adaptive TCP Congestion Control Algorithm

## Overview

SplineCC is an advanced TCP congestion control algorithm implemented in C (`SplineCcNew.cc`) within the NS-3 simulator environment. The algorithm is designed to optimize performance in networks with variable conditions such as high packet loss, latency, and fluctuating bandwidth. SplineCC combines elements of bandwidth-delay-based (similar to BBR) and loss-based (similar to CUBIC) approaches, using four adaptive operating modes: `START_PROBE`, `PROBE_BW`, `PROBE_RTT`, and `DRAIN_PROBE`. It is suitable for high-speed WANs, wireless networks, and high-traffic scenarios, and is also of interest to network protocol researchers and developers.

SplineCC demonstrates excellent performance, outperforming BBR under high-loss (up to 10%) and stable-latency conditions, as shown in NS-3 tests. The algorithm is optimized for both single-flow and competitive scenarios.

## Features

* **Multi-mode adaptation**:

  * `START_PROBE`: Quickly increases the congestion window (`cwnd`) to capture available bandwidth at the start.
  * `PROBE_BW`: Adjusts `cwnd` based on estimated bandwidth (`bw`) using exponential moving average (EMA).
  * `PROBE_RTT`: Minimizes latency by adjusting `cwnd` to measure minimum RTT.
  * `DRAIN_PROBE`: Reduces the excessive window when congestion is detected (increased RTT or decreased ACK).
* **Network state assessment**: Uses metrics `eps` (RTT-based, updates minimum RTT per epoch) and `fairness_rat` (ACK and `cwnd` based) for dynamic adaptation.
* **Congestion control**: Switches to congestion avoidance mode upon detecting loss or inflight buffer overflow.
* **Pacing & fairness\_rat**: Regulates packet sending rate to smooth traffic and prevent congestion.
* **Optimization**: Minimizes computational costs using integer operations and shifts.

## Technical Details

### Calculations

The `SplineCCAlgo` function computes the next congestion window (`next_cwnd`) based on parameters: `curr_rtt`, `throughput`, `num_acks`, and the state structure `m_state`. Example calculations:

* Input: `curr_rtt = 12` ms, `throughput = 125000000` bytes/s, `num_acks = 10`.
* Initial state: `last_min_rtt = 100` ms, `last_cwnd = 10`, `curr_cwnd = 64`, `bw = 0`.

#### Step-by-step process:

1. **RTT Update (`__epsilone_rtt`)**:

   * `last_min_rtt = min(last_min_rtt, curr_rtt) = 12` ms.
   * Increases `epp_min_rtt` if a new minimum is found.
2. **Bandwidth estimation (`__bw`)**:

   * `bw = (curr_ack * segmentSize) / minRtt = (10 * 1460) / 0.012 ≈ 1,216,667` bytes/s.
   * EMA applied: `last_bw = (last_bw * 3 + bw) >> 2 ≈ 304,167` bytes/s.
   * Filtering: `bw` limited to 75–150% of `last_bw`.
3. **Operating modes (`probs`)**:

   * `START_PROBE`: `curr_cwnd += num_acks = 64 + 10 = 74`.
   * `PROBE_BW`: `curr_cwnd = max(bw / segmentSize, curr_cwnd) ≈ 208`.
   * `PROBE_RTT`: `curr_cwnd = bw / segmentSize + (num_acks >> 2) ≈ 210`.
   * `DRAIN_PROBE`: `curr_cwnd = curr_cwnd * 0.75 ≈ 156`.
4. **Pacing (`pacing_gain_rate`)**:

   * `pacing_rate = bw * fairness_rat * minRtt ≈ 1,216,667 * 2 * 0.012 ≈ 29,200,008` bytes/s.

#### Result:

* `next_cwnd = 208` segments, `pacing_rate ≈ 29.2 Mbps`.

### Mode Switching

* Switching occurs every 9 epochs (`epp == 9`) or upon congestion detection (RTT > 2 \* `last_min_rtt` or `fairness_rat < 2`).

## Installation

SplineCC requires NS-3 (version 3.36 or later) with dependencies installed.

1. Clone the repository:

   ```bash
   git clone https://github.com/Kaibek/SplineCC.git
   cd SplineCC
   ```
2. Build NS-3 with SplineCC support:

   ```bash
   ./waf configure --enable-examples --enable-tests
   ./waf build
   ```
3. Integrate `SplineCcNew.cc` into your NS-3 project by adding it to `src/internet/model/` and updating the `wscript`.

## Usage

### Integration in NS-3

1. `SplineCcNew` is already configured for TCP.

2. Initialize the simulation with your parameters (e.g., `errorRate = 0.1`, `routerRate = 500Mbps`). A test file is available:

   ```bash
   nano scratch/dumbbell-my.cc
   ```

### Example Code

```cpp
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/spline-cc.h"

int main() {
    NodeContainer nodes;
    nodes.Create(2);
    InternetStackHelper stack;
    stack.Install(nodes);
    Config::SetDefault("ns3::TcpSocket::CongestionAlgo", StringValue("ns3::SplineCcNew"));
    // Configure network and applications...
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
```

## Testing

### Recommendations

1. **NS-3 simulation**:

   * Test with various `errorRate` (0.01%, 5%, 10%) and `routerDelay` (10 ms, 80 ms, 500 ms).
   * Use `FlowMonitor` to analyze throughput, RTT, and losses.
2. **Real-world tests**:

   * Integrate into the Linux TCP stack (e.g., via `tc` or kernel modules) and test in a live network.
3. **Edge cases**:

   * Test behavior with `curr_rtt > 1s`, `num_acks = 0`, or high loss (>20%).
4. **Simulation execution**:

   * Navigate to `SplineCC/build` and run the test: `./scratch/ns3.40-dumbbell-my-default`

### NS-3 Scenario Example

```cpp
uint32_t nLeaf = 2;
double errorRate = 0.1;
std::string routerRate = "500Mbps";
std::string routerDelay = "80ms";
PointToPointHelper pointToPoint;
pointToPoint.SetDeviceAttribute("DataRate", StringValue(routerRate));
pointToPoint.SetChannelAttribute("Delay", StringValue(routerDelay));
NetDeviceContainer devices = pointToPoint.Install(nodes);
Ipv4AddressHelper address;
address.SetBase("10.1.1.0", "255.255.255.0");
Ipv4InterfaceContainer interfaces = address.Assign(devices);
```

## Comparison with Other Algorithms

SplineCCNew was tested in NS-3 in two scenarios:

1. **Single link**: One flow at 500 Mbps, 80 ms delay, 10% loss.
2. **Various links**: Compared with BBR at 10,000 Mbps, 50 ms RTT, 0.01% loss.

### Results

#### Single Link

* **Throughput**: `SplineCcNew` — (0.25 - 0.26) Mbps, BBR — (0.02 - 0.01) Mbps (12.5x better).
* **RTT**: `SplineCcNew` — 200-300 ms (stable), BBR — up to 3000 - 450 ms (initial).
* **Delay**: `SplineCcNew` — 10 ms, BBR — 10.5 ms.
* **Loss**: `SplineCcNew` — 9.83%, BBR — 8.21%.
* **Conclusion**: `SplineCcNew` outperforms BBR in high-loss environments due to stable `cwnd` and pacing.

### Graphs

* **\[Single link]**: \[Throughput, CWND, RTT, Delay, Interruptions, Pacing]
  ![image](https://github.com/user-attachments/assets/7285f39b-2851-468a-95d2-77fddf18d75e)

## Author

**Kalimollaev Bekzhan**

## Contact

For questions and collaboration: [bekzhankalimollaev777@gmail.com](mailto:bekzhankalimollaev777@gmail.com).

