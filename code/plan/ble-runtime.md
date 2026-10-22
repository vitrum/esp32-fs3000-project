# BLE Runtime Architecture

## Ownership

`BleManager` owns command admission, the serial control worker, diagnostic radio
policy and stack suspend/resume. UI code only submits commands and consumes
outcomes. The worker runs at priority 2 on core 1; this does not move the SDK's
Bluetooth host/controller or change its RF coexistence schedule.

Cancellation increments a generation counter. Running scans and queued commands
observe the same cancellation token, including cancellation before a worker starts.
An outcome remains pending until consumed; stack teardown refuses pending work.
The worker acknowledges its stop command before queues and the BLE stack are freed.

GATT connect/discovery still use NimBLE's blocking APIs in the control worker.
They wait for stack events, not a fixed application delay. Cancellation of an
in-flight GATT operation is finalized after that API returns; it does not kill
the worker or touch its client from the UI thread. Connection attempts are bounded
by an 8-second timeout (a power-address fallback can make a second attempt).

## Scanning

`BleScanner` starts asynchronously and waits on a completion semaphore. States:
Idle, Starting, Scanning, Stopping, Complete, Cancelled, Failed.
Start rejection, abnormal end reason, cancellation and missing completion events
are distinguished from a successful scan with zero devices. A 2-second completion
grace follows the requested duration; a failed stop prevents another scan while
the controller still reports scanning. Diagnostics include elapsed time and count.

| Mode | Interval | Window | Active |
| --- | --- | --- | --- |
| Manual, no BLE links | 100 ms | 60 ms | yes |
| Manual, existing link | 100 ms | 30 ms | yes |
| Background, no BLE links | 100 ms | 20 ms | no |
| Background, existing link | 100 ms | 30 ms | no |

Background discovery failures back off from 2 to 32 seconds. The existing main
loop reconnect cadence also applies. Manual requests do not inherit this backoff.
These are conservative initial policies, not measured RF performance guarantees.

## Data and Logs

Notification callbacks copy into bounded inboxes: power has 12 slots, posture 8,
and each packet is at most 244 bytes. Invalid, oversize and full-queue packets are
counted and discarded without blocking. Main-loop draining has a per-tick bound.
Disconnect invalidates buffered packets and resets consumer snapshots. CPS, FTMS,
CSC and posture parsing, plus raw HEX formatting, run outside BLE callbacks.

Phone writes enter an 8-slot, 32-byte command inbox. Disconnect discards queued
commands. Diagnostics retain their 8 KB replay ring and pause advertising/pushes
during pending sensor operations without stopping sensor notification reception.
When idle, push budget is 1500 bytes/s, reduced to 500 bytes/s with sensor links;
burst allowance is 180 bytes. Notification failures back off from 20 ms to 1 s.

Serial producers append to a separate 2 KB ring. A priority-1 output task drains
at most 128 writable bytes every 10 ms; slow USB cannot block a BLE producer.
Overflow retains the newest bytes and increments loss statistics. Queue-loss
telemetry is rate-limited to once per 5 seconds when counters change.

## Verification

Run `sh test/host/run.sh` and `pio run -e esp32s3`.
The host suite runs the real manager, scanner, parsers and diagnostics with mocked
NimBLE/FreeRTOS boundaries. It covers failure/timeout/cancel, timer wrap, queue
overflow, concurrent producers, parsing, stale commands, byte budgets, congestion
backoff, stack restart and removal of the fixed 1.5-second operation gap.

Host tests do not prove RF coexistence, remote device timing or heap sufficiency
on hardware. Validate repeated discovery and link stability with Wi-Fi off, idle,
transferring data, and phone diagnostics connected. Measure runtime free internal
heap and task stack high-water marks before changing budgets or scan windows.