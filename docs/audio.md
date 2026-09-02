# Audio (M8) — the JAS audio pipeline on PC

Status: **M8 core implemented** — the JAS driver boots, mixes frames on the
CPU (DSP emulation), and pushes them to `Platform::Audio` (SDL3). Everything
compiles, links and `ctest` is green (154 passed / 0 failed).

Stub / still open (M8.5+):

- BNK + wave loading from the VFS and actual BGM/SE playback (needs
  `JASChannel`/`JASTrack`/bank loading — next step).
- Microphone / speaker → documented stub, `TODO(PC_PORT)`.

## 1. The Wii audio chain and what M8 replaces

```
GameAudio/AudioLib → JAISe/JAISeq (sequencers, not ported yet)
                        │
                  JASDriver (JAudio2, vendored, compiled natively)
                        │  JASAiCtrl: AI DMA buffers + per-frame cadence
                        │  JASDsp::syncFrame → DSP "microcode" (jdsp)
                        ▼
                 DSP (mixer, hardware on Wii) ──► AI (DMA to DAC)
```

On the Wii the final mix is done by the DSP running the `jdsp` microcode; the
AI unit DMAes the result out at 32028.5 Hz. M8 replaces the DSP with
`compat::dsp::Mixer` (pure C++) and the AI with `compat::ai` + the software
clock, ending at `Platform::Audio` (SDL3 pull streams).

The game barely touches AX directly; everything goes through JAudio2, so no
`compat/ax` is needed yet.

## 2. Vendored JAudio2 driver (compiled natively)

Compiled into `pc_compat` from `third_party/petari` (with the two pointer-ABI
patches, see `src/compat/patches/JSystem/JAudio2/` and
`src/compat/patches/README.md`):

| File | Role |
|---|---|
| `JASAiCtrl.cpp` (patched) | AI registers + DMA buffers + `updateDac`/`updateDSP`/`finishDSPFrame` |
| `JASDSPInterface.cpp` (patched) | `JASDsp::CH_BUF/FX_BUF` descriptors, `initBuffer`, `syncFrame` |
| `JASDriverIF.cpp` | driver-level callbacks, mixer level, output mode |
| `JASDSPChannel.cpp` | the 64 channel slots the sequencer allocates |
| `JASCalc.cpp` (patched) | `imixcopy`/`bcopy` (pointer→`uintptr_t`) |
| `JASLfo.cpp`, `JASProbe.cpp`, `JASCallback.cpp`, `JASHeapCtrl.cpp` (patched), `JASCmdStack.cpp`, `JASReport.cpp` | support |

NOT compiled: `dsptask.cpp`/`dspproc.cpp`/`osdsp_task.cpp` (they busy-wait on
the hardware mailboxes — `compat/dsp` replaces them), `JASAudioThread.cpp` /
`JKRThread.cpp` (need the OSThread layer; the host glue in
`src/compat/jsystem/JasAudioHost.cpp` reproduces `JASAudioThread::run()` on a
`Platform::Threading` thread — same message loop, `AUDIOMSG_DMA/DSP/STOP`),
and `JASChannel.cpp`/`JASTrack.cpp` (M8.5).

### 32-bit pointer ABI

The console passes DSP-visible 32-bit addresses everywhere (buffer starts,
wave data). Host pointers are 64-bit, so `compat::audio` keeps a token
registry: callers `storePtr(p)` → token, the emulation `loadPtr(token)` →
pointer. The two patched vendored files are the only places that needed the
change; everything below the driver is token-based, exactly like the Wii's
address space.

## 3. `compat/dsp` — the jdsp microcode in C++

`src/compat/dsp/`:

- `Mixer.h/.cpp` — `compat::dsp::Mixer`: 64 voice descriptors, 11 buses
  (main L/R, 4 FX-in, 5 aux), 4.12 pitch, linear interpolation, ADPCM + PCM8/16
  wave decode, FIR8/IIR channel filters, per-bus volumes (1.15) with ramps,
  auto-mixer envelope, one-shot/loop handling. FX/aux buses are summed dry
  into the main bus (`TODO(PC_PORT)` until the FX network lands).
- `Adpcm.h/.cpp` — DSP ADPCM decoder, 16 samples per 9-byte JAS block
  (not the 7-byte file format), coefficient ROM as in `JASDsp::DSPADPCM_FILTER`.
- `DSP.cpp` — the console-shaped surface (`DspBoot`, `DsetupTable`,
  `DsetMixerLevel`, `DSPSendCommands2`, `DsyncFrame2/4ch`, `DspFinishWork`,
  mailboxes) + the dsp-worker thread: one queued `FrameCmd` per sync, one
  `0xF355 xxFF` subframe mail per subframe (what `JASAudioThread::DSPCallback`
  consumes).
- `DSPCompat.h` — only the PC glue + `Mixer` live in `namespace compat::dsp`;
  the console-shaped functions are global (the vendored headers declare them
  global with C++ linkage; the mailboxes are `extern "C"` from
  `<revolution/dsp.h>`).

`compat::dsp::syncFrameHost(subFrames, outL, outR)` is what the patched
`JASDsp::syncFrame` ultimately calls.

## 4. `compat/ai` — AI registers, DMA and the soft clock

`src/compat/ai/`: `AIInit/AIInitDMA/AIStartDMA/AIStopDMA/AIGet* /AISetDSPSampleRate/AIRegisterDMACallback`
with the console contract (one "buffer done" callback per DMA buffer from a
non-game thread). The `ai-clock` thread fires the registered callback every
`getFrameSamples()/getDacRate()` seconds (≈17.5 ms). `pushAudioFrame` resamples
32028.47 Hz → 32000 Hz (linear, phase accumulator) and hands the frame to
`Platform::Audio::push`.

## 5. `Platform::Audio` — SDL3 backend

`src/platform/Audio/`:

- `RingBuffer.h` — SPSC lock-free FIFO of interleaved S16 (audio thread →
  device thread).
- `Audio.h/.cpp` — `SDL_OpenAudioDeviceStream` pull stream; the get-callback
  drains the ring (master gain applied there). If SDL audio is unavailable (or
  `config.enable = false`), the module runs in **virtual mode**: `push` keeps
  counting and `pull` still drains, so the whole pipeline works headless (CI,
  tests) with `isVirtual() == true`.

## 6. Host glue — `compat::jsystem::jas`

`src/compat/jsystem/JasAudioHost.{h,cpp}`:

- Owns the JAS heap fallback (`JASKernel::setupRootHeap` on a 16 MiB solid heap
  carved from the root heap) until the game boot (M9) does it via
  `JAUInitializer`.
- Starts the audio thread with the `JASAudioThread::run()` boot sequence:
  `JASDriver::initAI` → `JASDsp::boot` → `JASDsp::initBuffer` →
  `JASDSPChannel::initAll` → `registDSPBufCallback` (output → `compat::ai`) →
  `JASDriver::startDMA` → `ai::startClock`.
- `shutdownAudioSystem()` joins everything (STOP → thread, DSP worker, ai
  clock, SDL audio).

`ensureJasHeap()` in both the glue and the driver test: the solid heap must be
**strictly larger** than the subsystem heap carved out of it
(`setupRootHeap(solid, 0x800000 - 0x1000)` from a 16 MiB solid).

## 7. OS shims the driver needs

- `os/OSCache.cpp` — `DCInvalidateRange/DCFlushRange/DCStoreRange/DCZeroRange
  /DCEnable` no-ops (shared DMA, no cache to maintain).
- `os/OSTime.cpp` — `OSGetTime/OSGetTick/__OSGetSystemTime` over
  `Platform::Timing` (`OS_TIMER_CLOCK` = 60.75 MHz); defines `OS_BUS_CLOCK_SPEED`
  and `__OSBusClock`.
- `os/OSInterrupt.cpp` — `OSDisable/Enable/RestoreInterrupts` return 0
  (callers are already serialized; revisit if the game needs real exclusion).
- `os/OSThread.cpp` — `OSYieldThread` → `std::this_thread::yield()`.
- `os/OSMutex.cpp` (fix) — the mutex registry is now a function-local
  (Meyers) singleton: vendored global objects (`JASHeap audioAramHeap`) call
  `OSInitMutex` during static initialization, before file-scope statics in
  other TUs are guaranteed to be constructed.

## 8. Header overrides (`src/compat/include`)

- `JSystem/JAudio2/JASDSPInterface.hpp` — case shim (sources include
  `JASDSPInterface.hpp`, the vendored file is `JASDspInterface.hpp`); includes
  the vendored header by repo-relative path (safe on case-insensitive FS).
- `JSystem/JAudio2/JASGadget.hpp` — `JASPtrArray` base-initializer
  (`JASPtrTable<T>`, MWC accepted the unqualified spelling).
- `JSystem/JAudio2/JASHeapCtrl.hpp` — `operator new/delete` take `size_t`.
- `JSystem/JUtility/JUTAssert.hpp` — provides `__va_list`
  (`__builtin_va_list`), then includes the vendored header.
- `mem.h` — `<cstring>` shim.
- `revolution/os.h` — pointer-preserving address macros (`uintptr_t`).

## 9. Tests (`src/tests`)

| File | Covers |
|---|---|
| `audio_ring_test.cpp` | SPSC ring: round-trip, wrap-around, overflow drop |
| `audio_dsp_mixer_test.cpp` | Mixer: PCM routing/pan, voice end, ADPCM, bus summing |
| `audio_ai_test.cpp` | AI registers, DMA callback cadence, resampler → virtual sink |
| `jas_audio_driver_test.cpp` | Full boot → frames flow → clean shutdown (virtual mode) |

Run: `ctest --test-dir build --output-on-failure`.

## 10. Fidelity notes / approximations

- Interpolation is linear (`MixerConfig::interp`); the DSPRES_FILTER ROM
  kernel is a `TODO(PC_PORT)` (`Sinc4`).
- FX network (delay/reverb over `FX_BUF`) is stubbed as dry summing.
- GADPCM (5-byte blocks) not implemented (silence).
- The resampler is linear; the Wii AI did not need one.
