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
