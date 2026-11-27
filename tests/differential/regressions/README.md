# Differential regressions

Every module the differential harness once found a divergence in, minimized, as `<seed>.wasm`: the
module and the argument seed it diverged with (`isoline-differential-replay <seed>.wasm <seed>`).
`CorpusReplay.everyRegressionAgrees` replays each on every engine, in both lanes, as part of the
unit run. The fix lands in the code of the task that owns it, with a unit test of its own; the module
stays here so that the harness keeps checking it.
