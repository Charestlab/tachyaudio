# External onset validation and dedicated hardware

## Evidence so far

The initial 600 silent native scheduling trials reported no deadline failures.
Predicted onset residuals stayed within one 48 kHz sample (20.833 microseconds),
as expected from the placement calculation. These are not independent DAC or
acoustic measurements. Neither equivalence to TDT nor superiority to
PsychPortAudio has been demonstrated. A 50 ms minimum scheduling head start is
advance notice, not measured output latency or closed-loop response time.

## Practical wiring

```text
computer -> wired interface line out -> acquisition analog channel A
hardware-timed reference pulse      -> acquisition channel B / digital input
                                      one shared acquisition clock
```

Use a multi-channel DAQ, scope, or recorder with appropriate inputs. Begin with
low-level signals and no participant. Check voltage limits, impedance, grounding,
attenuation and isolation against both equipment manuals. Disable phantom power.
TTL goes to a TTL-compatible input or properly conditioned analog input, never
directly into an arbitrary sound-card input. Use approved auxiliary inputs, not
electrode inputs; follow laboratory isolation requirements before connecting
anything to participant-connected electrophysiology equipment.

For a first troubleshooting test, a line-out-to-line-in cable suffices. Inspect
recorded waveforms for clipping, dropouts and repetition stability. Without
independent timing calibration this measures round-trip behavior, not isolated
output latency or absolute scheduled-onset accuracy.

For electrophysiology, record a stimulus monitor on an approved auxiliary input
alongside the experiment event marker. Both are then on the neural acquisition
clock. Their difference measures actual audio-to-marker offset, even without
mapping the computer clock. A Python-issued USB marker has its own jitter; it
does not become an accurate deadline reference simply because it is recorded.

## What makes a reference valid?

Common-clock acquisition timestamps two physical signals relative to one another.
It does not automatically relate either signal to Python's monotonic `when`.
For that comparison, use a documented host/device clock synchronization facility
and hardware-scheduled reference pulses, or independently timestamp repeated
synchronization events and fit acquisition time = offset + rate * host time.
Measure drift and mapping uncertainty throughout the run. USB command issue
time is not the physical pulse-edge time.

A marker on the second audio output channel can validate channel alignment, but
both channels share scheduling errors. It cannot independently prove absolute
onset accuracy. If no reliable clock mapping is available, report relative
audio-to-marker timing only, not error relative to the requested host deadline.

## Measurement protocol

1. Fix sample rate, routing, volume, buffers and cabling. Avoid Bluetooth and
   disable optional processing such as spatial effects and automatic gain.
2. Play a known short, low-level waveform, acquiring line output and the reference
   concurrently. Define onset detection in advance: threshold with a noise
   criterion or matched-template detection. Document ramp and estimator bias.
3. Measure audio edge minus reference edge. Compare to `when` only through an
   independently validated clock mapping; include its uncertainty.
4. Save every raw waveform, requested/predicted time, marker and onset estimate,
   device configuration, software commit and native error. Missing sounds count
   as failures, not excluded trials.
5. Estimate fixed bias on a separate calibration set. Report uncorrected bias,
   calibrated residual SD, p95/p99/p99.9 absolute error, maximum and failure rate.
6. Test idle and graphics/CPU load, long runs and head-start sweeps. At least
   10,000 trials per condition is our proposed reliability target, not a guarantee.
7. Compare PsychPortAudio using identical hardware, rate, waveform, levels,
   routing, head start and load; alternate blocks to reduce drift confounds.

At 48 kHz one acquisition sample spans 20.833 microseconds. Input filtering,
detection and clock mapping add uncertainty; a higher sampling rate alone does
not remove systematic delay. Finally measure acoustics with a calibrated
microphone at the transducer/ear position or appropriate coupler. Electrical
onset excludes transducer delay, propagation and sound-pressure calibration.

## How close to TDT?

Tachyaudio removes one avoidable source of jitter: requiring Python to wake at
onset. It is not a calibrated stimulation/acquisition instrument. It cannot
supply hardware TTL, synchronize unrelated hardware clocks, guarantee desktop
OS deadlines, calibrate sound pressure or change converter performance.

TDT's RZ6 combines DSP generation, acquisition and digital I/O with high-rate
operation and defined hardware timing. TDT manuals also document converter/I/O
delays: deterministic does not mean zero latency. Transducers, attenuation and
calibration remain part of the complete system. A validated software/interface
setup may satisfy a particular behavioral or ERP protocol, but we have no
physical measurements yet to quantify the gap. Internal sample-grid residuals
do not establish ABR-grade timing, closed-loop guarantees or TDT equivalence.

## Primary references

- [TDT RZ6 hardware](https://www.tdt.com/docs/hardware/rz6-multi-i-o-processor/)
- [TDT RPvdsEx manual, including I/O delays](https://www.tdt.com/files/manuals/RPvdsEx_Manual.pdf)
- [TDT ABR calibration](https://www.tdt.com/docs/abr-user-guide/calibration/)
- [PsychPortAudioTimingTest](https://psychtoolbox.org/docs/PsychPortAudioTimingTest)
- [AudioFeedbackLatencyTest and its loopback limitations](https://psychtoolbox.org/docs/AudioFeedbackLatencyTest)
