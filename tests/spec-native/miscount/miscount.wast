;; The native runner's self-test (tests/CMakeLists.txt, spec-native-miscount): the oracle counts one
;; command more than the script holds, so the script fails as a whole before any command runs.
(module (func (export "f")))
(invoke "f")
