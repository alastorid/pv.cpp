# pv.cpp

Buffer your pipes.

Unbound reader and writer speed.

## Without pv
```
Time T1 (10 Mbps Network):
tar (10 Mbps)  -->  ncat (10 Mbps)
[===>          ]    [===>          ]
Disk throttled down to match network speed
Time T2 (Network jumps to 500 Mbps):
tar (200 Mbps)  -->  ncat (200 Mbps)
[======>       ]    [======>       ]
Network throttled down to match disk speed
```
## With pv
```
Time T1 (10 Mbps Network):
tar (200 Mbps)     pv buffer         ncat (10 Mbps)
[>>>>>>>>>>>>] --> [||||||||||] --→  [>         ]
                   ↑ Buffer filling   ↑ Slow output
                   due to slow net    bottleneck

Time T2 (Network jumps to 500 Mbps):
tar (200 Mbps)     pv buffer         ncat (500 Mbps)
[>>>>>>>>>>>>] --> [|||       ] -->> [>>>>>>>>>>]
                   ↑ Buffer draining  ↑ Fast output
                   at higher speed    no bottleneck
```

## Limitation
The behavior is undetermined if 16 EiB of data is processed without ever clearing the buffer.
