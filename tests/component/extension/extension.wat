;; SPDX-License-Identifier: Apache-2.0
;;
;; The extension world's hand-written guest (plan P11 Task 21): extension.wit's world `extension` as a
;; component, written by hand. It behaves exactly as guest.c does, so one host test drives both
;; (src/isoline/component/api/Extension_test.cpp). `wasm-tools parse` 1.263.0 makes extension.wasm of it
;; (build-guests.sh); no build of Isoline does.
;;
;; Its imports and type sections are laid out as wit-component lays out guest.wasm's. Its code is two core
;; modules where wit-component makes three: $Libc defines the memory and the bump allocator
;; (`cabi_realloc`) every canonical option names, and $Main imports them with the lowered host functions,
;; which need that memory and allocator before $Main exists. One memory in all, as Contour's provision
;; allows.
;;
;; $Main's memory map, the same in every instance:
;;   16 ..  392  constant strings and name tables (the data segments below)
;;   1024        host.state's result, a state-snapshot (80 bytes, 8-aligned)
;;   1152        host.run-command's result (24 bytes)
;;   1184        surface.size's result (8 bytes)
;;   1200        the exports' results (16 bytes)
;;   4096 ..     the heap, which never frees
(component
  (type $ty-types (instance
    (type (;0;) (enum "insert" "normal" "visual" "visual-line" "visual-block" "hint"))
    (export (;1;) "input-mode" (type (eq 0)))
    (type (;2;) (record (field "title" string) (field "active" bool)))
    (export (;3;) "tab" (type (eq 2)))
    (type (;4;) (enum "hidden" "normal" "error" "indeterminate" "warning"))
    (export (;5;) "progress-state" (type (eq 4)))
    (type (;6;) (record (field "state" 5) (field "percent" u8)))
    (export (;7;) "progress" (type (eq 6)))
    (type (;8;) (record (field "size-pt" f32) (field "cell-width-px" u32) (field "cell-height-px" u32)))
    (export (;9;) "font-metrics" (type (eq 8)))
    (type (;10;) (record (field "cols" u32) (field "rows" u32)))
    (export (;11;) "size" (type (eq 10)))
    (type (;12;) (list 3))
    (type (;13;) (tuple string string))
    (type (;14;) (list u8))
    (type (;15;) (variant (case "title" string) (case "cwd" string) (case "foreground-process" string)
                          (case "input-mode" 1) (case "tabs" 12) (case "user-var" 13) (case "progress" 7)
                          (case "focus" bool) (case "font-changed" 9) (case "resized" 11) (case "timer")
                          (case "reply" 14)))
    (export (;16;) "event" (type (eq 15)))
    (type (;17;) (flags "title" "cwd" "foreground-process" "input-mode" "tabs" "user-var" "progress" "focus"
                        "font"))
    (export (;18;) "event-kinds" (type (eq 17)))
    (type (;19;) (enum "none" "render"))
    (export (;20;) "render-request" (type (eq 19)))
  ))
  (import "isoline-test:extension/types@0.1.0" (instance $types (type $ty-types)))
  (alias export $types "size" (type $size))
  (alias export $types "input-mode" (type $input-mode))
  (alias export $types "tab" (type $tab))
  (alias export $types "progress" (type $progress))
  (alias export $types "font-metrics" (type $font-metrics))

  (type $ty-host (instance
    (export (;0;) "surface" (type (sub resource)))
    (alias outer 1 $size (type (;1;)))
    (export (;2;) "size" (type (eq 1)))
    (type (;3;) (enum "trace" "debug" "info" "warn" "error"))
    (export (;4;) "log-level" (type (eq 3)))
    (alias outer 1 $input-mode (type (;5;)))
    (export (;6;) "input-mode" (type (eq 5)))
    (alias outer 1 $tab (type (;7;)))
    (export (;8;) "tab" (type (eq 7)))
    (alias outer 1 $progress (type (;9;)))
    (export (;10;) "progress" (type (eq 9)))
    (alias outer 1 $font-metrics (type (;11;)))
    (export (;12;) "font-metrics" (type (eq 11)))
    (type (;13;) (list 8))
    (type (;14;) (tuple string string))
    (type (;15;) (list 14))
    (type (;16;) (record (field "title" string) (field "cwd" string) (field "foreground-process" string)
                         (field "input-mode" 6) (field "tabs" 13) (field "user-vars" 15) (field "progress" 10)
                         (field "focused" bool) (field "size" 2) (field "unix-time-ms" u64)
                         (field "utc-offset-minutes" s32) (field "font" 12)))
    (export (;17;) "state-snapshot" (type (eq 16)))
    (type (;18;) (list u8))
    (type (;19;) (record (field "exit-code" s32) (field "stdout" 18) (field "stderr" 18)))
    (export (;20;) "command-output" (type (eq 19)))
    (type (;21;) (enum "not-declared" "pending" "denied" "failed" "timed-out"))
    (export (;22;) "command-error" (type (eq 21)))
    (type (;23;) (borrow 0))
    (type (;24;) (func (param "self" 23) (param "bytes" 18)))
    (export (;0;) "[method]surface.write" (func (type 24)))
    (type (;25;) (func (param "self" 23) (result 2)))
    (export (;1;) "[method]surface.size" (func (type 25)))
    (type (;26;) (func (param "after-ms" u32)))
    (export (;2;) "request-frame" (func (type 26)))
    (type (;27;) (func (param "level" 4) (param "message" string)))
    (export (;3;) "log" (func (type 27)))
    (type (;28;) (func (result 17)))
    (export (;4;) "state" (func (type 28)))
    (type (;29;) (result 20 (error 22)))
    (type (;30;) (func (param "name" string) (param "args" 15) (result 29)))
    (export (;5;) "run-command" (func (type 30)))
    (type (;31;) (own 0))
    (type (;32;) (func (result 31)))
    (export (;6;) "open-surface" (func (type 32)))
    (type (;33;) (func (param "surface" 31)))
    (export (;7;) "present" (func (type 33)))
  ))
  (import "isoline-test:extension/host@0.1.0" (instance $host (type $ty-host)))

  (type $ty-control (instance
    (type (;0;) (record (field "id" string) (field "title" string) (field "is-active" bool)))
    (export (;1;) "tab-info" (type (eq 0)))
    (type (;2;) (option string))
    (type (;3;) (list 1))
    (type (;4;) (record (field "id" string) (field "title" 2) (field "tabs" 3)))
    (export (;5;) "pane" (type (eq 4)))
    (type (;6;) (variant (case "bad-request" string) (case "not-found" string) (case "forbidden")
                         (case "failed" string)))
    (export (;7;) "control-error" (type (eq 6)))
    (type (;8;) (list 5))
    (type (;9;) (result 8 (error 7)))
    (type (;10;) (func (param "filter" 2) (result 9)))
    (export (;0;) "list-panes" (func (type 10)))
  ))
  (import "isoline-test:extension/control@0.1.0" (instance $control (type $ty-control)))

  ;; The world's own `use` of the interfaces' types, as wit-component imports them.
  (alias export $types "event" (type $event-of-types))
  (import "event" (type $event (eq $event-of-types)))
  (alias export $types "event-kinds" (type $event-kinds-of-types))
  (import "event-kinds" (type $event-kinds (eq $event-kinds-of-types)))
  (alias export $types "render-request" (type $render-request-of-types))
  (import "render-request" (type $render-request (eq $render-request-of-types)))
  (alias export $types "size" (type $size-of-types))
  (import "size" (type $world-size (eq $size-of-types)))
  (alias export $control "pane" (type $pane-of-control))
  (import "pane" (type $pane (eq $pane-of-control)))
  (alias export $control "control-error" (type $control-error-of-control))
  (import "control-error" (type $control-error (eq $control-error-of-control)))

  ;; The memory and the allocator: a bump allocator that never frees, growing the memory as it must.
  (core module $Libc
    (memory (export "memory") 1)
    (global $next (mut i32) (i32.const 4096))
    ;; Whether cabi_realloc misbehaves, handing out a block past the memory's end (mode `bad-realloc`).
    (global $misbehaving (mut i32) (i32.const 0))
    (func (export "misbehave") (global.set $misbehaving (i32.const 1)))
    (func (export "cabi_realloc") (param $old i32) (param $oldSize i32) (param $align i32) (param $newSize i32)
                                  (result i32)
      (local $start i32)
      (local $end i32)
      (local $have i32)
      (if (global.get $misbehaving)
        (then (return (i32.const 0xfffffff0))))
      (if (i32.eqz (local.get $newSize))
        (then (return (local.get $align))))
      (local.set $start
        (i32.and (i32.add (global.get $next) (i32.sub (local.get $align) (i32.const 1)))
                 (i32.sub (i32.const 0) (local.get $align))))
      (local.set $end (i32.add (local.get $start) (local.get $newSize)))
      (if (i32.lt_u (local.get $end) (local.get $start))
        (then unreachable))
      (local.set $have (i32.shl (memory.size) (i32.const 16)))
      (if (i32.gt_u (local.get $end) (local.get $have))
        (then
          (if (i32.eq (memory.grow (i32.shr_u (i32.add (i32.sub (local.get $end) (local.get $have))
                                                       (i32.const 65535))
                                              (i32.const 16)))
                      (i32.const -1))
            (then unreachable))))
      (global.set $next (local.get $end))
      (if (local.get $old)
        (then
          (memory.copy (local.get $start) (local.get $old)
                       (select (local.get $oldSize) (local.get $newSize)
                               (i32.lt_u (local.get $oldSize) (local.get $newSize))))))
      (local.get $start)))

  ;; The extension.
  (core module $Main
    (import "libc" "memory" (memory 1))
    (import "libc" "cabi_realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "libc" "misbehave" (func $misbehave))
    (import "host" "[resource-drop]surface" (func $surfaceDrop (param i32)))
    (import "host" "[method]surface.write" (func $surfaceWrite (param i32 i32 i32)))
    (import "host" "[method]surface.size" (func $surfaceSize (param i32 i32)))
    (import "host" "request-frame" (func $requestFrame (param i32)))
    (import "host" "log" (func $log (param i32 i32 i32)))
    (import "host" "state" (func $state (param i32)))
    (import "host" "run-command" (func $runCommand (param i32 i32 i32 i32 i32)))
    (import "host" "open-surface" (func $openSurface (result i32)))
    (import "host" "present" (func $present (param i32)))
    (import "control" "list-panes" (func $listPanes (param i32 i32 i32 i32)))

    (data (i32.const 16) "load")
    (data (i32.const 32) "active tab: ")
    (data (i32.const 48) "var: ")
    (data (i32.const 64) "status: ")
    (data (i32.const 80) "spinning")
    (data (i32.const 96) "status")
    (data (i32.const 112) "mode")
    (data (i32.const 152) "=")
    ;; command-error's case names, as (pointer, length) pairs at 160, in the WIT's order.
    (data (i32.const 160) "\d0\00\00\00\0c\00\00\00" "\e0\00\00\00\07\00\00\00" "\e8\00\00\00\06\00\00\00"
                          "\f0\00\00\00\06\00\00\00" "\f8\00\00\00\09\00\00\00")
    (data (i32.const 208) "not-declared")
    (data (i32.const 224) "pending")
    (data (i32.const 232) "denied")
    (data (i32.const 240) "failed")
    (data (i32.const 248) "timed-out")
    ;; The modes' names, as (pointer, length) pairs at 256, the mode's number its index.
    (data (i32.const 256) "\40\01\00\00\06\00\00\00" "\48\01\00\00\07\00\00\00" "\50\01\00\00\04\00\00\00"
                          "\58\01\00\00\05\00\00\00" "\60\01\00\00\04\00\00\00" "\68\01\00\00\05\00\00\00"
                          "\70\01\00\00\0b\00\00\00" "\80\01\00\00\09\00\00\00")
    (data (i32.const 320) "normal")
    (data (i32.const 328) "hostile")
    (data (i32.const 336) "spin")
    (data (i32.const 344) "large")
    (data (i32.const 352) "trap")
    (data (i32.const 360) "hoard")
    (data (i32.const 368) "bad-realloc")
    (data (i32.const 384) "spin-load")

    ;; How the guest behaves, as the configuration's `mode` entry chose at load (README.md): 0 normal,
    ;; 1 hostile, 2 spin, 3 large, 4 trap, 5 hoard, 6 bad-realloc, 7 spin-load.
    (global $mode (mut i32) (i32.const 0))
    (global $titlePointer (mut i32) (i32.const 0))
    (global $titleLength (mut i32) (i32.const 0))

    ;; A copy of $length bytes at $from, on the heap.
    (func $copy (param $from i32) (param $length i32) (result i32)
      (local $to i32)
      (local.set $to (call $realloc (i32.const 0) (i32.const 0) (i32.const 1) (local.get $length)))
      (memory.copy (local.get $to) (local.get $from) (local.get $length))
      (local.get $to))

    ;; The concatenation of two byte strings, on the heap; its length is the sum of theirs.
    (func $concat (param $first i32) (param $firstLength i32) (param $second i32) (param $secondLength i32)
                  (result i32)
      (local $to i32)
      (local.set $to (call $realloc (i32.const 0) (i32.const 0) (i32.const 1)
                                    (i32.add (local.get $firstLength) (local.get $secondLength))))
      (memory.copy (local.get $to) (local.get $first) (local.get $firstLength))
      (memory.copy (i32.add (local.get $to) (local.get $firstLength)) (local.get $second)
                   (local.get $secondLength))
      (local.get $to))

    ;; Logs the concatenation of two byte strings at $level.
    (func $logJoined (param $level i32) (param $first i32) (param $firstLength i32) (param $second i32)
                     (param $secondLength i32)
      (call $log (local.get $level)
                 (call $concat (local.get $first) (local.get $firstLength) (local.get $second)
                               (local.get $secondLength))
                 (i32.add (local.get $firstLength) (local.get $secondLength))))

    ;; Whether two byte strings are equal.
    (func $equals (param $first i32) (param $firstLength i32) (param $second i32) (param $secondLength i32)
                  (result i32)
      (local $index i32)
      (if (i32.ne (local.get $firstLength) (local.get $secondLength))
        (then (return (i32.const 0))))
      (block $equal
        (loop $next
          (br_if $equal (i32.ge_u (local.get $index) (local.get $firstLength)))
          (if (i32.ne (i32.load8_u (i32.add (local.get $first) (local.get $index)))
                      (i32.load8_u (i32.add (local.get $second) (local.get $index))))
            (then (return (i32.const 0))))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br $next)))
      (i32.const 1))

    (func (export "load") (param $config i32) (param $configLength i32) (result i32)
      (local $list i32)
      (local $count i32)
      (local $index i32)
      (local $entry i32)
      (local $message i32)
      (local $length i32)
      (local $mode i32)
      ;; The configuration, 16 bytes an entry: `mode` names how the guest behaves.
      (local.set $index (i32.const 0))
      (block $configDone
        (loop $entries
          (br_if $configDone (i32.ge_u (local.get $index) (local.get $configLength)))
          (local.set $entry (i32.add (local.get $config) (i32.shl (local.get $index) (i32.const 4))))
          (if (call $equals (i32.load (local.get $entry)) (i32.load offset=4 (local.get $entry))
                            (i32.const 112) (i32.const 4))
            (then
              (local.set $mode (i32.const 0))
              (block $modesDone
                (loop $modes
                  (br_if $modesDone (i32.ge_u (local.get $mode) (i32.const 8)))
                  (if (call $equals (i32.load offset=8 (local.get $entry)) (i32.load offset=12 (local.get $entry))
                                    (i32.load (i32.add (i32.const 256) (i32.shl (local.get $mode) (i32.const 3))))
                                    (i32.load offset=4 (i32.add (i32.const 256)
                                                                (i32.shl (local.get $mode) (i32.const 3)))))
                    (then (global.set $mode (local.get $mode))))
                  (local.set $mode (i32.add (local.get $mode) (i32.const 1)))
                  (br $modes)))))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br $entries)))
      (if (i32.eq (global.get $mode) (i32.const 7))
        (then
          (call $log (i32.const 0) (i32.const 80) (i32.const 8))
          (loop $forever (br $forever))))
      (if (i32.eq (global.get $mode) (i32.const 6))
        (then (call $misbehave)))

      (call $log (i32.const 2) (i32.const 16) (i32.const 4))

      (call $state (i32.const 1024))
      (global.set $titleLength (i32.load offset=4 (i32.const 1024)))
      (global.set $titlePointer (call $copy (i32.load (i32.const 1024)) (global.get $titleLength)))
      ;; The tabs, 12 bytes each: the active one's title.
      (local.set $list (i32.load offset=28 (i32.const 1024)))
      (local.set $count (i32.load offset=32 (i32.const 1024)))
      (local.set $index (i32.const 0))
      (block $tabsDone
        (loop $tabs
          (br_if $tabsDone (i32.ge_u (local.get $index) (local.get $count)))
          (local.set $entry (i32.add (local.get $list) (i32.mul (local.get $index) (i32.const 12))))
          (if (i32.load8_u offset=8 (local.get $entry))
            (then
              (call $logJoined (i32.const 1) (i32.const 32) (i32.const 12)
                               (i32.load (local.get $entry)) (i32.load offset=4 (local.get $entry)))))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br $tabs)))
      ;; The user variables, 16 bytes each: `var: name=value`.
      (local.set $list (i32.load offset=36 (i32.const 1024)))
      (local.set $count (i32.load offset=40 (i32.const 1024)))
      (local.set $index (i32.const 0))
      (block $varsDone
        (loop $vars
          (br_if $varsDone (i32.ge_u (local.get $index) (local.get $count)))
          (local.set $entry (i32.add (local.get $list) (i32.shl (local.get $index) (i32.const 4))))
          (local.set $length (i32.add (i32.const 5) (i32.load offset=4 (local.get $entry))))
          (local.set $message (call $concat (i32.const 48) (i32.const 5)
                                            (i32.load (local.get $entry)) (i32.load offset=4 (local.get $entry))))
          (local.set $message (call $concat (local.get $message) (local.get $length) (i32.const 152) (i32.const 1)))
          (local.set $length (i32.add (local.get $length) (i32.const 1)))
          (call $logJoined (i32.const 1) (local.get $message) (local.get $length)
                           (i32.load offset=8 (local.get $entry)) (i32.load offset=12 (local.get $entry)))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br $vars)))

      ;; run-command("status", config): its output, or the name of its error.
      (call $runCommand (i32.const 96) (i32.const 6) (local.get $config) (local.get $configLength) (i32.const 1152))
      (if (i32.eqz (i32.load8_u (i32.const 1152)))
        (then
          (call $logJoined (i32.const 2) (i32.const 64) (i32.const 8)
                           (i32.load offset=8 (i32.const 1152)) (i32.load offset=12 (i32.const 1152))))
        (else
          (local.set $entry (i32.add (i32.const 160) (i32.shl (i32.load8_u offset=4 (i32.const 1152)) (i32.const 3))))
          (call $logJoined (i32.const 3) (i32.const 64) (i32.const 8)
                           (i32.load (local.get $entry)) (i32.load offset=4 (local.get $entry)))))

      (call $requestFrame (i32.const 250))
      ;; event-kinds { title, cwd }
      (i32.const 3))

    ;; A title event replaces the title and asks for a render; every other event asks for none.
    (func (export "update") (param $case i32) (param $pointer i32) (param $length i32) (param i32) (param i32)
                            (result i32)
      (if (local.get $case)
        (then (return (i32.const 0))))
      (global.set $titlePointer (call $copy (local.get $pointer) (local.get $length)))
      (global.set $titleLength (local.get $length))
      (i32.const 1))

    ;; `[title]`, the title cut to the surface's width less the brackets.
    (func (export "render") (param $cols i32) (param $rows i32) (result i32)
      (local $room i32)
      (local $shown i32)
      (local $to i32)
      (if (i32.eq (global.get $mode) (i32.const 1))
        (then
          (i32.store (i32.const 1200) (i32.const 8))
          (i32.store offset=4 (i32.const 1200) (i32.const 0x0fffffff))
          (return (i32.const 1200))))
      (if (i32.eq (global.get $mode) (i32.const 2))
        (then
          (call $log (i32.const 0) (i32.const 80) (i32.const 8))
          (loop $forever (br $forever))))
      ;; 4 MiB its memory holds, which the host's caps may refuse.
      (if (i32.eq (global.get $mode) (i32.const 3))
        (then
          (i32.store (i32.const 1200) (call $realloc (i32.const 0) (i32.const 0) (i32.const 1) (i32.const 0x400000)))
          (i32.store offset=4 (i32.const 1200) (i32.const 0x400000))
          (return (i32.const 1200))))
      (if (i32.eq (global.get $mode) (i32.const 4))
        (then unreachable))
      (if (i32.ge_u (local.get $cols) (i32.const 2))
        (then (local.set $room (i32.sub (local.get $cols) (i32.const 2)))))
      (local.set $shown (select (global.get $titleLength) (local.get $room)
                                (i32.lt_u (global.get $titleLength) (local.get $room))))
      (local.set $to (call $realloc (i32.const 0) (i32.const 0) (i32.const 1)
                                    (i32.add (local.get $shown) (i32.const 2))))
      (i32.store8 (local.get $to) (i32.const 0x5b))
      (memory.copy (i32.add (local.get $to) (i32.const 1)) (global.get $titlePointer) (local.get $shown))
      (i32.store8 (i32.add (local.get $to) (i32.add (local.get $shown) (i32.const 1))) (i32.const 0x5d))
      (i32.store (i32.const 1200) (local.get $to))
      (i32.store offset=4 (i32.const 1200) (i32.add (local.get $shown) (i32.const 2)))
      (i32.const 1200))

    ;; The heap never frees, so a post-return has nothing to release.
    (func (export "cabi_post_render") (param i32))

    ;; control.list-panes' answer, handed back unchanged: its result area is the export's.
    (func (export "panes") (param $case i32) (param $pointer i32) (param $length i32) (result i32)
      (call $listPanes (local.get $case) (local.get $pointer) (local.get $length) (i32.const 1200))
      (i32.const 1200))

    (func (export "cabi_post_panes") (param i32))

    (func (export "paint") (param $bytes i32) (param $length i32) (result i32)
      (local $surface i32)
      (local.set $surface (call $openSurface))
      ;; A guest that keeps every surface it opens: one write, its size, and no drop.
      (if (i32.eq (global.get $mode) (i32.const 5))
        (then
          (call $surfaceWrite (local.get $surface) (local.get $bytes) (local.get $length))
          (call $surfaceSize (local.get $surface) (i32.const 1200))
          (return (i32.const 1200))))
      (call $surfaceWrite (local.get $surface) (local.get $bytes) (local.get $length))
      (call $surfaceWrite (local.get $surface) (local.get $bytes) (local.get $length))
      (call $surfaceWrite (local.get $surface) (local.get $bytes) (local.get $length))
      (call $surfaceSize (local.get $surface) (i32.const 1184))
      (call $surfaceDrop (local.get $surface))
      (call $present (call $openSurface))
      (i64.store (i32.const 1200) (i64.load (i32.const 1184)))
      (i32.const 1200)))

  (core instance $libc (instantiate $Libc))
  (alias core export $libc "memory" (core memory $memory))
  (alias core export $libc "cabi_realloc" (core func $realloc))

  (alias export $host "surface" (type $surface))
  (core func $surface-drop (canon resource.drop $surface))
  (alias export $host "[method]surface.write" (func $surface-write))
  (core func $surface-write (canon lower (func $surface-write) (memory $memory)))
  (alias export $host "[method]surface.size" (func $surface-size))
  (core func $surface-size (canon lower (func $surface-size) (memory $memory)))
  (alias export $host "request-frame" (func $request-frame))
  (core func $request-frame (canon lower (func $request-frame)))
  (alias export $host "log" (func $log))
  (core func $log (canon lower (func $log) (memory $memory) string-encoding=utf8))
  (alias export $host "state" (func $state))
  (core func $state (canon lower (func $state) (memory $memory) (realloc $realloc) string-encoding=utf8))
  (alias export $host "run-command" (func $run-command))
  (core func $run-command (canon lower (func $run-command) (memory $memory) (realloc $realloc) string-encoding=utf8))
  (alias export $host "open-surface" (func $open-surface))
  (core func $open-surface (canon lower (func $open-surface)))
  (alias export $host "present" (func $present))
  (core func $present (canon lower (func $present)))
  (alias export $control "list-panes" (func $list-panes))
  (core func $list-panes (canon lower (func $list-panes) (memory $memory) (realloc $realloc) string-encoding=utf8))

  (core instance $host-core
    (export "[resource-drop]surface" (func $surface-drop))
    (export "[method]surface.write" (func $surface-write))
    (export "[method]surface.size" (func $surface-size))
    (export "request-frame" (func $request-frame))
    (export "log" (func $log))
    (export "state" (func $state))
    (export "run-command" (func $run-command))
    (export "open-surface" (func $open-surface))
    (export "present" (func $present)))
  (core instance $control-core
    (export "list-panes" (func $list-panes)))
  (core instance $main (instantiate $Main
    (with "libc" (instance $libc))
    (with "host" (instance $host-core))
    (with "control" (instance $control-core))))

  (type $config (list (tuple string string)))
  (alias core export $main "load" (core func $load-core))
  (func $load (param "config" $config) (result $event-kinds)
    (canon lift (core func $load-core) (memory $memory) (realloc $realloc) string-encoding=utf8))
  (export "load" (func $load))

  (alias core export $main "update" (core func $update-core))
  (func $update (param "ev" $event) (result $render-request)
    (canon lift (core func $update-core) (memory $memory) (realloc $realloc) string-encoding=utf8))
  (export "update" (func $update))

  (alias core export $main "render" (core func $render-core))
  (alias core export $main "cabi_post_render" (core func $render-post-return))
  (func $render (param "cols" u32) (param "rows" u32) (result (list u8))
    (canon lift (core func $render-core) (memory $memory) (post-return $render-post-return)))
  (export "render" (func $render))

  (alias core export $main "panes" (core func $panes-core))
  (alias core export $main "cabi_post_panes" (core func $panes-post-return))
  (func $panes (param "filter" (option string)) (result (result (list $pane) (error $control-error)))
    (canon lift (core func $panes-core) (memory $memory) (realloc $realloc) string-encoding=utf8
                (post-return $panes-post-return)))
  (export "panes" (func $panes))

  (alias core export $main "paint" (core func $paint-core))
  (func $paint (param "bytes" (list u8)) (result $world-size)
    (canon lift (core func $paint-core) (memory $memory) (realloc $realloc)))
  (export "paint" (func $paint))
)
