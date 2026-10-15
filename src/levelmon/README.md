# levelmon (Windows)

Port of the `levelmon` tool from [cxadc-linux3](https://github.com/happycube/cxadc-linux3) for the cxadc-win driver.
It reads samples continuously from a card and prints clipping counts, min/max amplitude, DC offset,
the measured sample rate and the driver's DMA over/underflow counter, several times per second.
Use it to set `level` / `center_offset` while watching a playing tape, and to check that the card
delivers the sample rate you expect.

```
PS> .\levelmon.exe 0
\\.\cxadc0  vmux=1 level=0 sixdb=off tenbit=off center_offset=0 tenxfsc=0 crystal=28636363  (9 MiB per line)
lo |0| [ 50.391%] (  0.000%) center +0.59% hi ( 50.588%) [ 50.781%] |0|  nsamp 9998336  rate 28.98  ouflow 0
```

```
Usage: levelmon.exe [device] [-i seconds] [-n count] [-b mib] [-h]

  device      \\.\cxadc0 (default), or just the index, e.g. 1
  -i seconds  data read per printed line, in seconds at 40 MS/s (default 0.25)
  -n count    stop after this many lines (default: run until Ctrl+C)
  -b mib      bytes read per line in MiB, overrides -i
```

Output columns, left to right: clipped-low count, minimum, average of the samples below centre,
DC offset relative to mid-scale, average of the samples above centre, maximum, clipped-high count,
samples in the line, measured rate in MS/s, driver over/underflow counter. `CLIPPING` is appended
when either clip counter is non-zero. In 10-bit mode (`EnableTenbit`) the 16-bit samples are
scaled to 10 bits before the statistics, as the Linux tool does.

## Building

Open a "x64 Native Tools" prompt, or point `cl.exe` at the SDK yourself, and run from this directory:

```
cl -nologo -W4 -O2 -MT -D_CRT_SECURE_NO_WARNINGS -I ..\shared -Fe:levelmon.exe levelmon.c -link -SUBSYSTEM:CONSOLE
```

Only the Windows SDK is needed (no WDK). `-MT` links the C runtime statically so the binary runs on
a machine without the Visual C++ redistributable.
