## Sampling
Based on the Performance Measurement Unit

Periodically taking snapshots of a programs execution state is referred to as sampling. Sampling can give deep insights into a programs behavior and performance, but comes with a considerable performance overhead, especially when sampling with high frequency and/or in virtualised environments. For further information regarding sampling overhead in miniOSv, consult the corresponding section of this documentation.

### Overview
MiniOSv leverages [PMU](./10-performance-monitoring.md) counter overflows as they provide more configurability and precision compared to timer interrupts. Our unikernel furthermore only implements the structure around sampling, so by design we do **not** provide a default implementation what should be done during a default routine. Instead, the application is responsible to provide the interrupt handler that will be run on every sample. 

### Interface
```c++
PerfSampler(uint64_t frequency, std::function handler, PMCEvent event)
```
- `frequency` takes the number of events (e.g. CPU cycles) between two samples
- `handler` takes a function that will be run on every sample
- `event` takes an event struct expressing which event should be counted (defaults to `PERF_COUNT_HW::CPU_CYCLES`)

> [!NOTE]
> The performance counter will be reset to the corresponding value (defined by the frequency) after each sample automatically, this does not have to be part of the handler function.
