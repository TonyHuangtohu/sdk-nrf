.. _peripheral_adc_continuous:

Peripheral ADC Continuous NUS
#############################

This sample targets ``nrf54l15dk/nrf54l15/cpuapp``. It samples four SAADC
inputs and streams the results over the Nordic UART Service (NUS) TX
characteristic when a central is connected and notifications are enabled.

ADC inputs
**********

The board overlay configures 14-bit single-ended SAADC channels:

* source 0: P1.04, SAADC AIN0, sampled at 500 Hz
* source 1: P1.05, SAADC AIN1, sampled at 500 Hz
* source 2: P1.06, SAADC AIN2, sampled at 1 kHz
* source 3: P1.07, SAADC AIN3, sampled at 1 kHz

The aggregate NUS output rate is 3 kHz (500 + 500 + 1000 + 1000 Hz), or about
6 KB/s at two bytes per sample record.

P1.04 and P1.05 are used by the default UART20 console on the DK, so this
sample disables the UART console and uses RTT logging.

ADC sampling modes
******************

The sample supports two ADC sampling modes selected by ``ADC_USE_SCAN_MODE`` near
the top of ``src/main.c``:

* ``ADC_USE_SCAN_MODE=1`` (default): all four SAADC channels are sampled in one
  scan that returns four raw samples in channel ID order (AIN0 to AIN3).
  ``ADC_SCAN_USE_REGISTERS`` selects how the scan is done, see below.
* ``ADC_USE_SCAN_MODE=0``: legacy mode. The application keeps the previous
  behavior and calls ``adc_read_dt()`` separately for each channel that is due.

Scan implementation
===================

In scan mode, ``ADC_SCAN_USE_REGISTERS`` near the top of ``src/main.c`` selects
the implementation of ``read_adc_scan()``:

* ``ADC_SCAN_USE_REGISTERS=1`` (default): direct register access. At boot the
  application programs the SAADC registers once and leaves the SAADC disabled.
  Each scan enables the SAADC, sets the result buffer, triggers ``START`` and
  ``SAMPLE`` and polls for the ``STARTED`` and ``END`` events, then triggers
  ``STOP`` and disables the SAADC again. This is the same enable and disable
  sequence the Zephyr driver uses, so the SAADC is off between scans in both
  variants. The SAADC interrupts are disabled so the Zephyr driver ISR cannot
  consume the polled events.
* ``ADC_SCAN_USE_REGISTERS=0``: the Zephyr ADC driver. One ``adc_read()`` call
  with a sequence whose ``channels`` bitmask selects channel IDs 0 through 3.

To build the driver variant without editing the source:

.. code-block:: console

   west build -b nrf54l15dk/nrf54l15/cpuapp -d build_54l15 -p -- \
     -DEXTRA_CFLAGS=-DADC_SCAN_USE_REGISTERS=0

The register values match what the Zephyr driver programs for the board
overlay (verified by reading the registers back on hardware):

* ``CH[n].PSELP``: AIN0 to AIN3 on P1.04 to P1.07, ``CONNECT=AnalogInput``.
* ``CH[n].CONFIG``: gain 1/4 (``GAIN=Gain2_8``), internal 0.9 V reference,
  single ended, acquisition time 10 us (``TACQ=79``, ``(TACQ + 1) x 125 ns``).
* ``RESOLUTION``: 14 bit. ``OVERSAMPLE``: bypass. ``SAMPLERATE``: task mode.
* ``RESULT.MAXCNT``: in bytes on nRF54L15 (8 for four channels).

These registers are encoded differently on nRF54L15 than on nRF52, so nRF52
register examples cannot be reused as is. Because the values are hardcoded,
``adc_init()`` checks that the devicetree channel configuration still matches
and fails with ``-EINVAL`` if it does not. Update both if you change the overlay.

Like the Zephyr driver, the register variant clamps negative single-ended
results to 0. Near 0 V the SAADC offset can produce small negative values,
which the 14-bit NUS record cannot carry.

Each wait for an event is bounded. If the SAADC does not respond, the scan stops
the SAADC and returns ``-ETIMEDOUT`` instead of blocking forever.

Measured on ``nrf54l15dk/nrf54l15/cpuapp`` at 128 MHz over 5000 scans:

=====================  ===========  ===========  ===========
``read_adc_scan()``    Average      Minimum      Maximum
=====================  ===========  ===========  ===========
Zephyr driver          59.8 us      45.8 us      134 us
Register access        57.8 us      57.6 us      122 us
=====================  ===========  ===========  ===========

On average the two variants are close. The register variant mainly gives a
more consistent scan time. Most of the time is spent in the SAADC itself:
about 11 us from ``START`` to ``STARTED`` after the SAADC is enabled, plus four
channels with a 10 us acquisition time each. A shorter ``SAADC_REG_TACQ_10US``
makes a scan faster. Only shorten it if the source impedance of your inputs
allows it.

The register variant busy-waits during the scan (about 6 % CPU at 1 kHz),
while the driver variant lets other threads run during the conversion.

Sampling schedule
=================

Both modes run from the same 1 kHz schedule. In scan mode the firmware
scans all four channels every 1 ms. AIN2/AIN3 samples are queued and sent every
scan, giving 1 kHz output per channel. AIN0/AIN1 samples are only queued on
every second scan, giving 500 Hz output per channel. All samples produced by one
scan share the same timestamp.

LED indication
**************

The sample uses the board devicetree LED aliases on ``nrf54l15dk``:

* ``led0`` blinks while connectable Bluetooth LE advertising is active.
* ``led1`` is on while a central is connected.
* ``led3`` pulses briefly when an ADC sample is dropped because a per-source
  queue is full. The pulse is suppressed while ``led0`` is blinking (advertising).

When a connection is established, the advertising LED stops blinking and turns
off. When the connection is disconnected, ``led0`` resumes blinking immediately
and connectable advertising is restarted once the connection object is recycled.

NUS frame format
****************

Each notification contains one or more packed little-endian binary records,
batched up to the negotiated NUS payload length:

* ``uint16_t packed_sample_le``

Each ``packed_sample_le`` record uses the unused high bits of the 14-bit
single-ended ADC result:

* bits 15..14: source id (``0`` = AIN0, ``1`` = AIN1, ``2`` = AIN2,
  ``3`` = AIN3)
* bits 13..0: 14-bit raw ADC sample value

Decode each record as:

.. code-block:: c

   uint16_t packed = sys_get_le16(record);
   uint8_t source = packed >> 14;
   uint16_t raw = packed & 0x3fff;

This format assumes the inputs remain single-ended 14-bit channels. In this
mode the SAADC result is non-negative, so bits 15 and 14 are not part of the
valid ADC value. Timestamps and per-channel sequence numbers are kept locally
for diagnostics, but they are not sent over BLE.

Each source has its own message queue. If BLE cannot keep up, the oldest
sample in that source queue is dropped so the stream keeps the latest data.
The application requests automatic ATT MTU exchange; peers that keep the
default 23-byte ATT MTU will receive fewer records per notification and may not
keep up with the full 3 kHz aggregate sample rate.

RTT logging
***********

The sample logs over Segger RTT. On each successful NUS TX, ``LOG_DBG`` prints
every sample (channel and raw value). ``LOG_INF`` prints the same fields at a
reduced rate: once every 100 samples per channel (see
``NUS_LOG_INF_INTERVAL`` in ``src/main.c``). Set
``CONFIG_LOG_DEFAULT_LEVEL=4`` to enable per-sample debug output.

When ADC samples are dropped because BLE cannot keep up, the sample logs the
channel (``AIN0``–``AIN3`` / source id), the reason, and the per-channel queue
depth. The first drop on a channel is logged at ``LOG_WRN``; further drops are
summarized at ``LOG_INF`` every 100 drops per channel (see
``ADC_DROP_LOG_INF_INTERVAL``). Drop reasons include:

* ``queue full``: the per-source message queue is full and the oldest sample is
  evicted (typical when not connected, notifications are disabled, or the link
  is too slow).
* ``TX batch full``: a sample was dequeued but did not fit in the current NUS
  notification payload.
* ``NUS send failed``: a batched notification could not be sent and its samples
  were lost.

Bluetooth LE link tuning
************************

The sample actively requests a high-throughput link when a central connects:

* **2M PHY** — ``CONFIG_BT_AUTO_PHY_PERIPHERAL_2M`` makes the peripheral
  initiate a PHY update to LE 2M after connection. ``CONFIG_BT_USER_PHY_UPDATE``
  enables the ``le_phy_updated`` callback, which logs the negotiated TX/RX PHY
  over RTT.
* **~150 ms connection interval** — BLE connection intervals are specified in
  1.25 ms units, so 150 ms corresponds to 120 units
  (``CONN_INTERVAL_UNITS`` in ``src/main.c``).
  The peripheral also calls ``bt_conn_le_param_update()`` from ``connected()``
  and sets ``CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT=0`` so the parameter update is
  requested immediately. ``le_param_updated`` logs the interval actually in use.

The central may accept or reject these requests. If it keeps 1M PHY or a longer
connection interval, check RTT logs for the negotiated values and adjust the
central-side policy if needed.

Build
*****

.. code-block:: console

   west build -b nrf54l15dk/nrf54l15/cpuapp nrf/samples/bluetooth/peripheral_adc_continuous

Speed optimizations and SAADC blocking
****************************************

This sample was validated on NCS v3.3.0 with the default size optimization
(``CONFIG_SIZE_OPTIMIZATIONS``, ``-Os``). If you build with speed optimization
instead, for example:

.. code-block:: console

   west build -b nrf54l15dk/nrf54l15/cpuapp -d build_54l15 -p -- \
     -DCONFIG_SPEED_OPTIMIZATIONS=y

you may observe an intermittent hang in ``adc_thread`` inside
``read_adc_scan()`` / ``adc_read()``. The thread blocks forever in
``adc_context_wait_for_completion()`` while waiting for the SAADC completion
semaphore.

Root cause
==========

With ``-O2``, the nrfx SAADC driver in NCS v3.3.0 and v3.4.1 can reorder the
write to ``m_cb.saadc_state`` so that it happens **after** the SAADC
``TASK_START`` MMIO write in ``nrfx_saadc_mode_trigger()``. If the STARTED
interrupt runs in that window, the ISR may still see the old driver state and
fail to trigger ``TASK_SAMPLE``. No END/DONE event is produced, so the ADC
driver never gives the completion semaphore and ``adc_read()`` never returns.

This is a compiler reordering issue in the Nordic HAL, not an application bug
in this sample. Size optimization (``-Os``) keeps the safe instruction order and
does not expose the race as readily.

Status in NCS v3.4.1
====================

The issue is still present in NCS v3.4.1 (nrfx 4.3.1). The simple non-blocking
path of ``nrfx_saadc_mode_trigger()`` is unchanged from v3.3.0 and has no
barrier between the state update and the ``TASK_START`` write.

This was verified on ``nrf54l15dk/nrf54l15/cpuapp``:

* Disassembly with the v3.4.1 toolchain: with ``-Os`` the ``STRB`` that stores
  ``m_cb.saadc_state`` comes before the ``RESULT.PTR``, ``RESULT.MAXCNT`` and
  ``TASK_START`` writes. With ``-O2`` it comes after ``TASK_START``. The advanced
  mode path is reordered the same way.
* Hardware: with ``CONFIG_SPEED_OPTIMIZATIONS=y`` the hang occurs within the
  first second of streaming. A central that enables NUS notifications receives a
  few notifications with the samples already queued, and then no ADC data at all.
  After the hang, ``m_cb.saadc_state`` is ``NRF_SAADC_STATE_SIMPLE_MODE_SAMPLE``,
  SAADC is enabled with the result buffer latched, ``EVENTS_STARTED`` has been
  cleared by the ISR, and ``EVENTS_END`` never fires.
* With the default ``CONFIG_SIZE_OPTIMIZATIONS`` the sample streams without a
  hang.

Keep the default size optimization on v3.4.1, or apply the workaround below
before you enable speed optimization.

Workaround
==========

Apply a one-line ``compiler_barrier()`` in the Nordic HAL before rebuilding
with speed optimization. Edit:

``modules/hal/nordic/nrfx/drivers/src/nrfx_saadc.c``

In ``nrfx_saadc_mode_trigger()``, in the simple non-blocking path, place the
barrier between the state update and the buffer/start writes:

.. code-block:: c

   m_cb.saadc_state = NRF_SAADC_STATE_SIMPLE_MODE_SAMPLE;
   compiler_barrier();
   nrfy_saadc_buffer_set(NRF_SAADC, &m_cb.buffer_primary, true, false);

Rebuild with ``CONFIG_SPEED_OPTIMIZATIONS=y``. You can confirm the fix in
``zephyr.lst``: the ``STRB`` that stores ``m_cb.saadc_state`` must appear
**before** the ``STR`` instructions that write ``RESULT.PTR``,
``RESULT.MAXCNT``, and ``TASK_START``.

Note that this change lives in the NCS HAL module, not in this sample tree.
It may be overwritten when you upgrade NCS. Consider reporting the issue to
Nordic/Zephyr upstream and keeping a local patch or west manifest override.

Alternatives
============

* Use the default size optimization (``CONFIG_SIZE_OPTIMIZATIONS``) if maximum
  CPU performance is not required.
* Do not rely only on a timeout in ``adc_context_wait_for_completion()``: a
  timeout returns without resetting SAADC, and subsequent ``adc_read()`` calls
  may fail with ``-EBUSY``.
