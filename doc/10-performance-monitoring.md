## Performance Monitoring

MiniOSv leverages hardware performance counters directly. The corresponding architecture-specific implementation can be found in 'arch/<arch>/arch-perf.hh'.

> [!NOTE]
> When running virtualized, these counters are gated. A VM-exit is therefore required to query or change their contents.


### Performance Monitoring Unit (PMU)
CPU packages contain multiple units in charge or performance monitoring:
- Oncore PMUs: Each CPU core contains a unit capable of measuring oncore events (cycles, l1 cache misses, branch mispredictions, ...)
- Uncore PMU(s): Each CPU package contains one or more units capable of measuring traffic on the package (l3 cache misses, Local upstream DMA read data bytes, ...)


#### Basic Functionality
Most oncore PMUs allow counting 6 events concurrently (IDs 0-5). Counting one event requires a pair of 2 registers:
- Performance Event Selector [0-5] (PerfEvtSel[0-5])
    - Write to this register to configure which event you want to count
    - This register allows for further configuration (enable counting, interrupt on overflow, ...)
- Performance Monitoring Counter [0-5] (PMC[0-5])
    - Read from this register to get the number of events counted

This conceptually maps the [PerfEvent header](https://github.com/viktorleis/perfevent) syntax to the following 4 x86 assembly instructions

<table>
<tr><th>PerfEvent</th><th>x86 assembly</th></tr>
<tr><td>

```c++
PerfEvent e;
e.startCounters();
yourBenchmark();
e.stopCounters();
```

</td><td>

```asm
wrmsr $PerfEvtSel0, $conf
rdmsr $PMC0
call yourBenchmark
rdmsr $PMC0
```

</td></tr>
</table>


> [!NOTE]
> This is substantially simplified
