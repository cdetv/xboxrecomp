# isr_gate

`xbox_IsrGateTryEnter` is `xbox_IsrGateEnter` without the wait. The APU frame
thread used to wait for the dispatch gate before every interrupt delivery; on
an overloaded host the raised guest thread holding it was descheduled, the
wait reached 0.5 s, and no audio was produced meanwhile (Conker, 16 busy host
processes: ~900 of 7504 frames per 5 s). It now tries, and if the gate is
busy keeps the line up and retries on its next frame.

`isr_gate_try`: a thread raised to DISPATCH_LEVEL holds the gate; the try
must return 0 at once, then 1 after the holder lowers. `isr_gate_off`
(`RECOMP_ISR_GATE=0`) is the control: the try must not report busy.

    cmake -S tests/isr_gate -B build/isr-gate -G Ninja
    cmake --build build/isr-gate
    ctest --test-dir build/isr-gate
