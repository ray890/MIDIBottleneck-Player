# Engineering opportunities

These are evidence-backed future directions, not promises for a particular release. The shorter public view is [ROADMAP.md](ROADMAP.md).

## Near term

Build 44 is published and closed. Gather experience with its released executables, then choose the next product phase deliberately. The deferred architectural work below is not an active build and must not begin incidentally.

## Completed foundations

- **Build 18 — archive/history foundation:** corrected the private diagnostics chronology after a provenance audit. The sanitized public history keeps exact/nearest-preserved qualifications and excludes private evidence.
- **Build 25 — corrected Per-note gate:** separates source-note occurrences from one constrained Up/Down output state per pitch. Repeated Note On strikes can retrigger through a clean release gap; simultaneous layers and dense overload are deterministic.
- **Build 26 — gate/Analysis hardening:** uses growable occurrence segments, preserves surviving same-pitch support after a channel disable, and publishes a generation-safe workload-only Analysis preview.
- **Build 27 — compact final event store:** uses immutable 40-byte record segments and a segmented long-payload store. Playback and Analysis read compact views without per-event object allocation.
- **Build 28 — direct compact parser:** parses bounded track streams into provisional segments, merges directly into the final store, and segments tempo/source histories. No runtime configuration sidecar remains.
- **Build 29 — large-file preflight:** scans only files large enough to justify a second pass, then estimates open-file and loading-peak memory from exact SMF counts.
- **Build 44 — bounded MIDI compatibility:** opens bare SMF and RIFF/RMID payloads through one bounded source view shared by preflight and production. One deterministic post-meta running-status repair is counted and disclosed; ambiguous malformed input stays strict.
- **Build 30 — human-facing refinement:** clarifies memory warnings, gate statistics, downloads, documentation, release history, and roadmap. Effective speed now uses a monotonic gate-progress frontier.
- **Build 31 — queue-state correctness:** distinguishes real blocked-output backlog from virtual rate-model pressure and applies forward-only Drop newest/complete-note decisions without delaying accepted MIDI.
- **Build 33 — direct event rate:** adds a third visible rate model with an integer rational clock shared by playback, virtual forward admission, and Analysis. Rejected work consumes no service phase, zero is unlimited/immediate, and active edits use a same-position generation restart.
- **Build 34 — live edits and Channels provenance:** ordinary rate edits now apply at the next service start without silencing or restarting. Channels can show indexed source values as gray historical readouts when no output observation exists; these never count as sent MIDI. Forward-only queue limiting now starts off.
- **Build 35 — oldest complete-note overflow:** the finite delayed queue can evict the oldest unsent note occurrence and its queued release in constant-time indexed operations. A later release is suppressed, in-service attacks remain protected, and static Analysis makes matching decisions. Protected releases and note-silencing controls can exceed the nominal soft capacity.
- **Build 36 — session view controls:** native system-menu choices independently collapse and restore Processing model and Statistics. They preserve settings, counters, queue and output state, and the user's extra standard-window height. Compact height is remeasured from visible rows.
- **Build 37 — interface refinements:** shorter gate-filter and compact slash readouts, measured Analysis-panel readout fallback, graph-hover Space transport, ordered observed-state Chase when first opening Channels, scrub-or-type main numeric values, a finer low-rate Events/sec slider, and exact 9,999,999/sec service-clock support.
- **Build 38 — main numeric refinement:** content-measured processing and queue fields, closer numeric/label alignment, and processing scrubbing that follows the selected slider scale without rounding an exact typed starting value. The Channel Monitor editor's semantics remain separate. A bounded UI-scaling feasibility note records why no partial scale menu was added.
- **Build 39 — numeric controls and Per-note Analysis:** centered measured numeric values preserve narrow white margins without clipping; Queue scrubbing uses a continuous gentle-low/fast-high scale with exact reversal. Static Analysis now runs the shared gate state machine over source events and graphs its selected messages at logical emission times. Its gate projection remains distinct from ordinary queue pressure and from native-provider behavior.
- **Build 40 — Rate model presentation:** None, three ordinary service-rate choices, and Per-note gate share one visible grouped selector. A contextual heading distinguishes forward-only virtual admission from delayed slowdown without changing scheduling mathematics.
- **Build 41 — contextual offline Help:** F1 and the title-bar Help command open one reusable first-party guide at the topic associated with the focused main-window control. It does not need a bundled file, native provider, or decorative question-mark style.
- **Build 42 — queue-age and presentation controls:** grouped Rate headings are static popup text and are absent from the ComboBox's selectable/accessibility item model; None and all four active models retain independent values. Zero Per-note intervals resolve at source timestamps. A default-off waiting-time queue limit shares strict age and overflow decisions with Analysis. Session-only 50%–200% scaling for the main, Analysis, and Channels windows is recalculated from canonical 100% measurements.
- **Build 43 — presentation and protected pressure:** the grouped popup rendered static headings while the collapsed Rate field used native themed rendering, but the headings did not create sufficiently perceptible grouping and several small-scale/layout defects remained. Tahoma/Lucida Console improved 50%/75% readability, standard height became content-locked, Queue now/max remained an event count, and purple identified required protected traffic above a soft limit. Playback and Analysis preserved the same protected-admission decisions.
- **Build 44 — shutdown observability and Channels acknowledgement:** the test process now uses a real `Application.Run` lifecycle, and full x86/x64 gates monitor native top-level windows plus descendant error-reporting processes through post-exit teardown. The former popup-triggered `0xC0020001` fault was test-harness-only; the packaged player remained clean. A forced-value acknowledgement race found by the frozen gate now preserves truthful historical provenance when Auto wins the boundary.
- **Build 44 — Bank-aware percussion presentation:** Channel 10 combines its effective displayed Bank and Program for recognized GM2, GS, and XG drum-kit names, keeps the 1-based Program visible, and uses a qualified numbered fallback for device-specific or unknown mappings. Other channels retain the shared melodic General MIDI catalog.

Build 37 compared the older finite policies with Build 34 on the same bounded 120,000-event synthetic fixture. The Build 35 linked-node path showed a local Analysis slowdown, especially for Drop oldest. A segmented FIFO now handles policies that do not need complete-note eviction. This recovered much, but not all, of the baseline time, with matching projected drop and occupancy decisions and zero measured Gen0 collections. Further throughput conclusions require representative repeated workloads; this is not a claim about native provider speed.

## Playback and MIDI state decisions

### Whole-state chase on Play/Seek

Completed in Build 32. The checked-by-default power-user option uses segmented controller/parameter histories and event-index lower bounds, restores ordered state before later source events, respects disabled channels and overrides, and excludes notes, SysEx, poly pressure, system/meta messages, and channel-mode commands.

### Channel-override extensions

Current overrides safely cover Bank MSB/LSB, Program, Volume, Expression, Pan, Sustain, pitch bend, and channel aftertouch. Source conflicts are filtered before scheduler admission; explicit controls remain ordered. The Channel Monitor also supports one-attribute source-value chase.

Build 44 completes the narrow default-off projection of current forced overrides, with frozen monitored x86/x64 acceptance. It belongs independently to each Analysis window, snapshots destination-channel forces after Track routing, gives the snapshot its own cache identity, reports generated forced messages separately from filtered competing source state, refreshes enabled windows after override edits, and never changes Playback. Presets, simultaneous requested/forced display, static transformations, and every broader override extension remain deferred. The existing automatic whole-state chase remains a distinct Play/Seek feature.

### Preserving playback on Pause or Seek

Build 44 implements the accepted default-off, session-only **Preserve playback queue on Pause and Seek** model. Pause freezes the same worker, transport clock, queue, structural model state, and counters without Reset/Panic. Seek stops new old-position admission, drains all accepted decisions, appends a protected logical All Notes Off marker, then performs target chase and target admission. Intended and output timelines remain distinct while the retained queue drains. Repeated Seek markers are retained, while only unadmitted work for superseded targets is replaced. Stop, file/output replacement, disposal, timeout, and provider failure remain hard cleanup boundaries. The tradeoff is explicit: seeking can take a long time and sustained or stuck sound is not guaranteed to stop immediately.

### Track routing for constrained hardware

Build 44 now implements this session-only stopped workflow:

`source event → optional track-to-output-channel route → queue/service model → runtime override filter → MIDI output`

Both parsers retain bounded track/instrument names, and the planner presents every MIDI track/channel combination rather than assuming a whole track has one channel. The complete preview exposes usable destinations, melodic capacity, percussion policy, representative patches, exact-safe/GM-family/aggressive tiers, conflicts, same-key ambiguity, retimbrance, handoffs, unresolved errors, manual destination corrections, and unambiguous routed/merged/retimbred/omitted/generated-state totals. Each destination now compiles a deterministic Bank/Program timeline: clean non-overlapping roles can hand instruments off, while incompatible overlap keeps the selected representative and suppresses competing changes. Aggressive representative selection is global rather than Exact-first: it balances musical importance, overlapping/state-independent roles, patch/family coverage, and merge cost. It may deterministically omit a lower-value entry only when every merge is more destructive; the prominent row and complete preview report its note count, and the otherwise valid lossy plan requires confirmation. Unresolved plans cannot Apply; family, aggressive, omitted, and manual plans require explicit warning confirmation. A successful Apply creates one immutable routed song view shared by Playback, Analysis, chase, Channels, filtering, overrides, occurrence ownership, and statistics while retaining the original source. Large-file preflight now includes bounded retained-name and optional routing workspace/index allowances, and the focused compact-store benchmark builds no duplicate MIDI payload graph. Plans are deliberately session-only until stable file identity/versioning is designed.

### Build 31 forward-only finite capacity

Completed for **Drop newest** and **Drop incoming complete notes**. Playback and Analysis share the bounded pressure model, accepted output is not delayed, and real native/scheduler backlog is displayed separately. Drop oldest and clear/catch-up still require a simulated delayed rate model; supporting them without delay would require an ordered producer/consumer contract and cannot honestly be emulated after output is sent.

## Presentation decisions

### Scaling and alternate main-window mode

- Hiding Statistics or Processing controls is complete in Build 36; the canonical shown-both layout and native DPI measurements remain authoritative.
- Build 42 applies 50%, 75%, 100%, 125%, 150%, and 200% non-cumulatively to all three application windows on top of the current Windows DPI. The main window preserves responsive transitions and hidden sections; Analysis preserves its split/report/graph geometry and scales graph painting; Channels preserves automatic fitting or the user's manual window/column geometry. Repeated cycling and exact 100% restoration are covered.
- Build 44 implements the accepted default-off, session-only borderless/chromeless main window with a compact title strip, an integrated File and output header, and a non-shifting auto-hide overlay. All three retain native moving/snapping, taskbar/Alt-Tab identity, keyboard-accessible window controls, and exact native-frame bounds/state restoration. A real child-input path forwards invisible edge/corner drags to native resizing without drawing a border. Automated tests cover that production mapping and lifecycle, while human pointer testing remains the final feel check. Analysis and Channels retain native frames.

Build 32's placement audit kept these controls in the system menu at the time. Build 40 moved Per-note gate into the grouped Rate model dropdown and removed the separate slowdown checkbox. Forward-only queue admission, state chase, and Always on top remain in the system menu.

### Rate-model presentation — completed in Build 40

The visible selector has **None**, ordinary models under **Simulated slowdown**, and musical gates/limits under **Bandwidth / note gating**. The first heading becomes **Forward-only queue admission** when an ordinary rate drives the finite virtual queue without delaying accepted output. The native collapsed selector remains unchanged; the popup uses strongly separated, nonselectable static headings, while the ComboBox item and accessibility choice model contains only real choices. None never runs a hidden rate; activating forward-only admission from None visibly selects the remembered ordinary model. Ordinary live edits, gate transitions, and queue rules retain their established boundaries.

### Delayed NoteOff treatment

Build 44's playback review now uses one bounded owner per MIDI pitch globally across tracks and channels. The selected value supplies minimum transmitted duration and earliest retrigger eligibility. A transmitted NoteOff begins a derived recovery of one quarter of that value, capped at 40 ms; during recovery, deterministic Per-note candidate priority retains at most one live replacement. Displaced, coalesced, losing, and ended candidates cannot release a newer owner. Other pitches remain independent, and Playback and Analysis share the same occurrence, sustain, timing, and candidate decisions.

### Exact rate gates — accepted Build 44 direction

The Build 44 **Note start rate gate** uses exact rational pacing, a fixed 50 ms batching tolerance, and an exact rolling one-second ceiling for admitted NoteOns. This preserves below-limit passages and practical high-limit chords without turning quiet time into growing saved credit. Simultaneous pressure uses the shared musical ranking; releases for admitted attacks stay protected and releases paired with rejected attacks stay suppressed.

The distinct **Output rate gate** applies the same paced/rolling contract to ordinary transmitted MIDI messages, not the Events/sec slowdown queue, and rejected work is never postponed for catch-up. Controllers and source state consume ordinary capacity. An admitted attack reserves room for its release, so routine protected traffic cannot make ordinary output exceed the target; immediate lifecycle cleanup bypass is counted separately. Complete unsent note occurrences use the shared deterministic musical ranking. Analysis projects the ideal nonblocking result; live decisions may differ after unpredictable provider blocking because they use actual completed dispatch time.

The moderate-limit Delayed NoteOff melody loss was reproduced and corrected with one bounded musically selected recovery candidate per pitch; zero interval is also verified to add no positive timing gate beyond intentional ownership and simultaneous-transition rules. Output-rate measurement proved that the brief load-transition graph peak is a short-bucket average rather than a half-open rolling-cap violation. Its roughly one-second rejected-count echo comes from exact expiry of the earlier tolerance-preserved group. Removing that tolerance caused severe live underuse under scheduler batching, so the bounded rhythm is retained. No measured example justified a separate musical-selection strategy or selector. **Drop background notes** remains unavailable with waiting-time limiting unless a separately validated policy can safely advance already-overdue work.

### Help presentation — completed in Build 41

Native `WS_EX_CONTEXTHELP` conflicts with the principal windows' minimize/maximize styles. The offline F1/system-menu Help window now provides topic-specific guidance without that style or a decorative question mark. Future improvements to wording can be ordinary documentation work.

## Public project maintenance

The public repository uses explicit first-party packaging and explicit `main`/tag pushes. Build 31 continues to publish versioned loose executables plus checksums. Private diagnostics, provider logs, machine paths, and recovery refs stay local.

A bounded GitHub Actions trial was retired because realized WinForms geometry tests depend on interactive desktop, font, and working-area metrics. Future CI should separate display-independent tests or provide a controlled interactive desktop instead of weakening layout assertions.

## Longer-term / high-risk research

- **Output-paced timeline:** completed as a default-off session mode. Logical source time stops at a due event or same-time batch until simulated/native acceptance completes; future timestamps are not admitted and no catch-up burst is built. Playback uses actual synchronous call duration, while Analysis models deterministic service without claiming to predict native-provider blocking.
- **Deferred pending later product decisions:** compositor synchronization, producer/consumer output separation, portability, and override extensions beyond the accepted narrow per-Analysis-window projection.
- **Rewrite/backend architecture only:** helper-process MIDI isolation may be reconsidered with a backend rewrite because it changes IPC, timing, SysEx ownership, and output lifetime. It was not Build 44 work and remains rewrite/backend-only.
- **Static Per-note Analysis:** completed in Build 39 using the shared gate decisions. The only implemented override projection is the narrow default-off per-window Build 44 item stated above.
