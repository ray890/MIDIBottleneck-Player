# Mutation check (Milestones 3-5): makes one deliberate decision change per v2 component,
# rebuilds the x64 dump tool, replays that model's v1 trace, and requires a
# difference. Every source file is restored afterwards and the build is rebuilt.
# Prerequisite: the x64 CMake build (v2\BUILDING.md).
# Usage: mutation-check.ps1 [-Family rate,queue,ordered,pernote,delayed,attack,outputrate,analysis,routing,engine,burst,direct,harness]
# Family 'burst' (Milestone 7): seven mutations of the immediate-dispatch burst and the due-time search, and three of the
# channel monitor's batched recording and the snapshot's eligible count (prompt 009); detected by midibn-engine-tests.
# Family 'direct' (DD-094): eight mutations of the direct WinMM output's call gate, its time-limited reset and close, the
# record of drivers that stopped answering, and the player's Play guard and status line; detected by
# midibn-direct-output-tests or midibn-app-tests.
# Families other than 'engine' replay a model trace with midibn-dump (Milestones 3-4).
# Family 'engine' (Milestone 5; 55 with the DD-006..DD-009 fixes): 50 mutations, one or more per engine component (transport Start/Seek/Pause,
# Stop/Reset/Panic order, Pause acknowledgement, ordered Seek preserve, worker wait/spin, Queue service start,
# Drop admission, age limit and each overflow policy, forward-only admission, Per-note gate, Delayed NoteOff,
# Note start rate gate, Output rate gate, service-time models, Output-paced timeline, chase, live overrides,
# disabled channels, ordered generations, statistics snapshot, routed playback, SysEx send path). Each one
# rebuilds midibn-engine-harness and runs the engine comparison (170 scenarios x 23 fixtures against
# v1's committed digests, --fail-fast) and requires it to FAIL; a run that hangs is killed after 15 minutes and
# counts as detected. The unmutated comparison must pass first. Raw logs: v2\out\mutation\engine-NN.log
# (untracked). The comparison runs with the difference rules and the v2 baseline, as the parity runner does.
# Family 'harness': eight cases that test the comparison machinery itself, on copies of the expected files
# (no source file is changed): (1) an unexplained new difference from v1 in a trace fails, (2) in an engine
# recording fails, (3) removing the rules that explain DD-001 fails, (4) a stale trace rule fails ("no longer
# occurs"), (5) a stale engine rule fails, (6) v2 baseline drift in a trace fails, (7) in an engine recording
# fails, (8) an explained difference from v1 still has to match the v2 baseline. Two controls must pass first:
# the unchanged files, and an explained difference. Raw logs: v2\out\mutation\harness-NN.log.
param([string[]]$Family)
if ($Family) { $Family = @($Family | ForEach-Object { $_ -split ',' }) }   # also accepts -File ... -Family a,b
$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
# .NET file calls resolve relative paths against the process directory.
[IO.Directory]::SetCurrentDirectory((Get-Location).Path)
$cm = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$m = @(
 @('Service duration (events/sec remainder)', 'v2/core/src/models/rate.cpp', 'return static_cast<int64_t>(n / d);', 'return static_cast<int64_t>((n + 1) / d);', 'rate'),
 @('Service duration (bitrate ceiling)', 'v2/core/src/models/rate.cpp', 'return numerator % bitrate != 0 ? quotient + 1 : quotient;', 'return quotient;', 'rate'),
 @('Progressive rate pacer (burst size)', 'v2/core/src/models/rate.cpp', 'max64(minimumBurstSlots, (rate + 19) / 20)', 'max64(minimumBurstSlots, (rate + 20) / 20)', 'rate'),
 @('Adaptive note interval (close test)', 'v2/core/src/models/rate.cpp', 'nextSourceMicroseconds <= open_boundary_) return false;', 'nextSourceMicroseconds < open_boundary_) return false;', 'rate'),
 @('Pending queue (head-only eviction)', 'v2/core/src/models/queue.cpp', 'if (eligible_head_ < 0 || eligible_head_ != head_) {', 'if (eligible_head_ < 0) {', 'queue'),
 @('Forward-drop queue (capacity test)', 'v2/core/src/models/queue.cpp', 'if (occupancy() >= capacity_ && !safetyAdmission) return false;', 'if (occupancy() > capacity_ && !safetyAdmission) return false;', 'queue'),
 @('Complete-note tracker (sustain safety)', 'v2/core/src/models/queue.cpp', 'return (controller == 64 && value < 64)', 'return (controller == 64 && value <= 64)', 'queue'),
 @('Musical priority (final tie-break)', 'v2/core/src/models/priority.cpp', 'return candidate.source_order > current.source_order;', 'return candidate.source_order < current.source_order;', 'queue'),
 @('Ordered finite admission (soft limit)', 'v2/core/src/models/ordered_admission.cpp', 'if (protection == OutputWorkProtection::Ordinary || protection == OutputWorkProtection::OrdinaryNoteAttack) {', 'if (protection == OutputWorkProtection::Ordinary || protection == OutputWorkProtection::OrdinaryNoteAttack || protection == OutputWorkProtection::SafetyTraffic) {', 'ordered'),
 @('Per-note interval gate (boundary rounding)', 'v2/core/src/models/per_note_gate.cpp', 'if (remainder == 0) return quotient * interval_microseconds_;', 'if (remainder == 0) return (quotient + 1) * interval_microseconds_;', 'pernote'),
 @('Delayed NoteOff (recovery length)', 'v2/core/src/models/delayed_note_off_gate.cpp', 'recovery_microseconds_ = min64(minimumActiveMicroseconds / 4, kMaximumRecoveryMicroseconds);', 'recovery_microseconds_ = min64(minimumActiveMicroseconds / 3, kMaximumRecoveryMicroseconds);', 'delayed'),
 @('Note start rate gate (release suppression)', 'v2/core/src/models/rate_gates.cpp', 'if (get(node).accepted) emissions.push_back(midiEvent);', 'if (true) emissions.push_back(midiEvent);', 'attack'),
 @('Output rate gate (two-slot note rule)', 'v2/core/src/models/rate_gates.cpp', 'newAvailable = 2;', 'newAvailable = 1;', 'outputrate'),
 @('Analysis raw workload (cluster size class)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (clusterSize >= 10) result.events_in_clusters_at_least_10 += clusterSize;', 'if (clusterSize >= 11) result.events_in_clusters_at_least_10 += clusterSize;', 'analysis'),
 @('Analysis bucket limit (2,000,000)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (bucketCount > kMaximumBucketCount) {', 'if (bucketCount >= kMaximumBucketCount) {', 'analysis'),
 @('Analysis service time without decoding (processing time, DD-096)', 'v2/core/include/midibn/models/rate.h', 'if (mode_ == ServiceDurationMode::ProcessingTime) return processing_microseconds_;', 'if (mode_ == ServiceDurationMode::ProcessingTime) return processing_microseconds_ + 1;', 'analysis'),
 @('Analysis processing-time demand', 'v2/core/src/analysis/workload_analysis.cpp', 'result.buckets[i].service_demand_us = checked_mul(result.buckets[i].event_count, c.processing_us);', 'result.buckets[i].service_demand_us = checked_add(result.buckets[i].event_count, c.processing_us);', 'analysis'),
 @('Analysis finite queue (admission limit)', 'v2/core/src/analysis/workload_analysis.cpp', 'if ((!configuration.queue_age_limit_enabled && occupancy < limit ||', 'if ((!configuration.queue_age_limit_enabled && occupancy <= limit ||', 'analysis'),
 @('Analysis queue age (limit test)', 'v2/core/src/analysis/workload_analysis.cpp', 'wrap_sub(logicalNow, checked_add(events.micros_at(pending.oldest_event_index()), arrivalOffset)) > limit;', 'wrap_sub(logicalNow, checked_add(events.micros_at(pending.oldest_event_index()), arrivalOffset)) >= limit;', 'analysis'),
 @('Analysis clear buffer (same-time catch-up)', 'v2/core/src/analysis/workload_analysis.cpp', 'while (i + 1 < count && events.micros_at(i + 1) <= arrival) { dropped++; i++; }', 'while (i + 1 < count && events.micros_at(i + 1) < arrival) { dropped++; i++; }', 'analysis'),
 @('Analysis Output-paced timeline (unlimited queue)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (outputPaced) arrival = pacedArrival;', 'if (false) arrival = pacedArrival;', 'analysis'),
 @('Analysis Output-paced timeline (age offset)', 'v2/core/src/analysis/workload_analysis.cpp', 'queueArrivalOffset = wrap_sub(pacedArrival, sourceArrival);', 'queueArrivalOffset = 0;', 'analysis'),
 @('Analysis forward-only queue (clock commit)', 'v2/core/src/analysis/workload_analysis.cpp', 'serviceClock = admittedClock;', '(void)admittedClock;', 'analysis'),
 @('Analysis Per-note projection (source vs boundary)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (next < count && source <= boundary) {', 'if (next < count && source < boundary) {', 'analysis'),
 @('Analysis Delayed NoteOff projection (tie rule)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (boundary < source || boundary == source && !sourcePrecedesRecovery) {', 'if (boundary <= source) {', 'analysis'),
 @('Analysis Note start projection (note classification)', 'v2/core/src/analysis/workload_analysis.cpp', 'record_gate_output(result, gateLocator, time, note);', 'record_gate_output(result, gateLocator, time, !note);', 'analysis'),
 @('Analysis Output rate projection (dispatch time)', 'v2/core/src/analysis/workload_analysis.cpp', 'if (gate.record_dispatch(time, false)) result.output_rate_safety_soft_excess++;', 'if (gate.record_dispatch(time + 1, false)) result.output_rate_safety_soft_excess++;', 'analysis'),
 @('Analysis gate bucket growth', 'v2/core/src/analysis/workload_analysis.cpp', 'std::max(required, previous > kMaximumBucketCount / 2 ? kMaximumBucketCount : previous * 2));', 'std::max(required, previous > kMaximumBucketCount / 2 ? kMaximumBucketCount : previous * 3));', 'analysis'),
 @('Analysis forced-override projection (suppression)', 'v2/core/src/overrides/override_snapshot.cpp', 'return forcedValue != kAutoValue && forcedValue != sourceValue;', 'return forcedValue != kAutoValue;', 'analysis'),
 @('Analysis base-workload cache (two results)', 'v2/core/src/analysis/workload_analysis.cpp', 'while (!order_.empty() && (results_.size() >= 2 || buckets_ + added > kMaximumBucketCount)) {', 'while (!order_.empty() && (results_.size() >= 3 || buckets_ + added > kMaximumBucketCount)) {', 'analysis'),
 @('Analysis progress (overall cap)', 'v2/core/src/analysis/workload_analysis.cpp', 'p.overall_permille = std::min<int32_t>(1000,', 'p.overall_permille = std::min<int32_t>(999,', 'analysis'),
 @('Pending queue (duplicate attack refused before changing, DD-004)', 'v2/core/src/models/queue.cpp', 'if (entry.is_note_on && pending_attack_nodes_.find(entry.event_index) != pending_attack_nodes_.end())', 'if (false && pending_attack_nodes_.find(entry.event_index) != pending_attack_nodes_.end())', 'queue'),
 @('Progressive rate pacer (clamped record, DD-004)', 'v2/core/src/models/rate.cpp', 'record(max64(0, timestamp));   // the same clamped time that expiry uses (v1 recorded the unclamped one; DD-004)<NL>    return true;', 'record(timestamp);<NL>    return true;', 'rate'),
 @('Routing lane profiles (sustain threshold)', 'v2/core/src/routing/track_routing.cpp', 'bool sustainOn = value >= 64;', 'bool sustainOn = value > 64;', 'routing'),
 @('Routing exact-safe tier (same-channel preference)', 'v2/core/src/routing/track_routing.cpp', 'if (group->destination == lane.lane.source_channel) return group;', 'if (false) return group;', 'routing'),
 @('Routing GM-family tier (unknown program)', 'v2/core/src/routing/track_routing.cpp', '(lane.representative_patch.gm_family() < 0 ||', '(false ||', 'routing'),
 @('Routing aggressive tier (representative score)', 'v2/core/src/routing/track_routing.cpp', 'int64_t score = static_cast<int64_t>(candidateCount - dominanceIndex) * 100;', 'int64_t score = static_cast<int64_t>(candidateCount - dominanceIndex) * 10;', 'routing'),
 @('Routing omission (destructive merge threshold)', 'v2/core/src/routing/track_routing.cpp', 'bool destructiveMerge = bestCost >= 15 &&', 'bool destructiveMerge = bestCost >= 16 &&', 'routing'),
 @('Routing compiler (handoff owner)', 'v2/core/src/routing/track_routing.cpp', 'if (multipleSourceChannels && representative != nullptr) return representative;', 'if (false && representative != nullptr) return representative;', 'routing'),
 @('Routing routed view (unassigned events)', 'v2/core/src/routing/track_routing.cpp', 'event_destinations_[static_cast<size_t>(source.source_index)] >= -1;', 'event_destinations_[static_cast<size_t>(source.source_index)] >= -2;', 'routing'),
 @('Routing statistics (merged lanes)', 'v2/core/src/routing/track_routing.cpp', 's.merged_lane_count += static_cast<int32_t>(destination.assignments.size()) - 1;', 's.merged_lane_count += static_cast<int32_t>(destination.assignments.size()) - 0;', 'routing'),
 @('Routing plan view (lossy confirmation)', 'v2/core/src/routing/route_plan_view.cpp', 'bool confirmed = !state.valid || !compiled->requires_explicit_apply || lossyConfirmationChecked;', 'bool confirmed = !state.valid || !compiled->requires_explicit_apply;', 'routing')
)
# Family 'engine' (Milestone 5): the engine comparison (midibn-engine-harness: 170 scenarios x 23
# fixtures, v1 digests) must FAIL for each change. '<NL>' in a pattern stands for a CRLF line break.
$e = @(
 @('Transport Start (position)', 'v2/core/src/engine/playback_engine.cpp', 'transport_base_ticks_ = micros_to_ticks(startMicroseconds);', 'transport_base_ticks_ = micros_to_ticks(startMicroseconds + 1);', 'engine'),
 @('Transport Start (state chase bound)', 'v2/core/src/engine/playback_engine.cpp', 'startMicroseconds);<NL>    pending_state_chase_event_index_.store(chase_midi_state_on_play_seek() && start_event_index_ > 0<NL>        ? start_event_index_ : -1);', 'startMicroseconds);<NL>    pending_state_chase_event_index_.store(chase_midi_state_on_play_seek() && start_event_index_ > 0<NL>        ? start_event_index_ - 1 : -1);', 'engine'),
 @('Transport Seek (position)', 'v2/core/src/engine/playback_engine.cpp', 'transport_base_ticks_ = micros_to_ticks(targetMicroseconds);', 'transport_base_ticks_ = micros_to_ticks(targetMicroseconds + 1);', 'engine'),
 @('Transport Pause (position)', 'v2/core/src/engine/playback_engine.cpp', 'transport_base_ticks_ = micros_to_ticks(pausedMicroseconds);', 'transport_base_ticks_ = micros_to_ticks(pausedMicroseconds + 1);', 'engine'),
 @('Stop/Reset/Panic order (reset before panic)', 'v2/core/src/output/midi_output.cpp', 'OutputStatus resetFailure = output->reset();<NL>    OutputStatus panicFailure = output->panic();', 'OutputStatus panicFailure = output->panic();<NL>    OutputStatus resetFailure = output->reset();', 'engine'),
 @('Stop (final silence at worker exit)', 'v2/core/src/engine/playback_engine.cpp', 'silence_when_worker_exits_ = thread != nullptr;', 'silence_when_worker_exits_ = false;', 'engine'),
 @('Pause acknowledgement (generation test)', 'v2/core/src/engine/playback_engine.cpp', 'pause_acknowledged_generation_ != pause_request_generation_) {', 'pause_acknowledged_generation_ == pause_request_generation_) {', 'engine'),
 @('Seek position-keeping (ordered preserve, state test)', 'v2/core/src/engine/playback_engine.cpp', '(previousState == PlaybackState::Playing || previousState == PlaybackState::Paused) &&', '(previousState == PlaybackState::Playing) &&', 'engine'),
 @('Worker wait (wake-generation check)', 'v2/core/src/engine/playback_engine.cpp', 'if (iterationWakeGeneration != wake_generation()) return;', 'if (false) return;', 'engine'),
 @('Worker spin (final-approach test)', 'v2/core/src/engine/playback_engine.cpp', 'while (targetTicks > current_transport_ticks() && is_playing()) {', 'while (targetTicks >= current_transport_ticks() && is_playing()) {', 'engine'),
 @('Worker spin (stopwatch final-approach test)', 'v2/core/src/engine/playback_engine.cpp', 'while (targetStamp > now_stamp() && is_playing()) {', 'while (targetStamp >= now_stamp() && is_playing()) {', 'engine'),
 @('Worker wait (wake event cleared before the check)', 'v2/core/src/engine/playback_engine.cpp', 'wake_->reset();<NL>        if (iterationWakeGeneration != wake_generation()) return;', 'if (iterationWakeGeneration != wake_generation()) return;', 'engine'),
 @('Queue service start (not before the previous completion)', 'v2/core/src/engine/engine_worker.cpp', 'int64_t startTicks = max64(event_ticks(inService), lastCompletionTicks);', 'int64_t startTicks = min64(event_ticks(inService), lastCompletionTicks);', 'engine'),
 @('Drop queue admission (capacity test)', 'v2/core/src/engine/engine_worker.cpp', 'if (((!ageLimited && outstanding < bufferCapacity) ||', 'if (((!ageLimited && outstanding <= bufferCapacity) ||', 'engine'),
 @('Drop newest (protected traffic admission)', 'v2/core/src/engine/engine_worker.cpp', '<NL>                    if (protectedTraffic) {<NL>                        pending.enqueue(incomingEntry);', '<NL>                    if (!protectedTraffic) {<NL>                        pending.enqueue(incomingEntry);', 'engine'),
 @('Drop oldest (protected head test)', 'v2/core/src/engine/engine_worker.cpp', 'bool oldestProtected = CompleteNoteTracker::classify(oldestEvent) == CompleteNoteEventKind::NoteOff ||', 'bool oldestProtected = CompleteNoteTracker::classify(oldestEvent) == CompleteNoteEventKind::NoteOn ||', 'engine'),
 @('Clear buffer and catch up (catch-up index)', 'v2/core/src/engine/engine_worker.cpp', 'int32_t catchUp = find_first_event_after_transport(nextArrival, now);', 'int32_t catchUp = find_first_event_after_transport(nextArrival, 0);', 'engine'),
 @('Drop incoming complete notes (safety admission)', 'v2/core/src/engine/engine_worker.cpp', 'bool safetyAdmission = overflowPolicy == OverflowPolicy::DropIncomingCompleteNotes &&<NL>                noteKind != CompleteNoteEventKind::NoteOn;', 'bool safetyAdmission = overflowPolicy == OverflowPolicy::DropIncomingCompleteNotes &&<NL>                noteKind == CompleteNoteEventKind::NoteOn;', 'engine'),
 @('Drop oldest complete note (own attack evicted)', 'v2/core/src/engine/engine_worker.cpp', 'if (ownAttackEvicted || (!evicted && !protectedTraffic)) {', 'if (ownAttackEvicted || (!evicted && protectedTraffic)) {', 'engine'),
 @('Drop background notes (nothing evicted)', 'v2/core/src/engine/engine_worker.cpp', 'if (ownAttackEvicted || (!evicted && noteKind == CompleteNoteEventKind::NoteOn)) {', 'if (ownAttackEvicted || (!evicted && noteKind != CompleteNoteEventKind::NoteOn)) {', 'engine'),
 @('Drop queue age limit (strict test)', 'v2/core/src/engine/playback_engine.cpp', 'return models::wrap_sub(logicalNowMicroseconds, event_micros(pending.oldest_event_index())) > limit;', 'return models::wrap_sub(logicalNowMicroseconds, event_micros(pending.oldest_event_index())) >= limit;', 'engine'),
 @('Forward-only admission (safety admission)', 'v2/core/src/engine/engine_worker.cpp', 'bool accepted = virtualQueue.try_admit(arrival, service, safetyAdmission);', 'bool accepted = virtualQueue.try_admit(arrival, service, false);', 'engine'),
 @('Per-note gate (source before boundary)', 'v2/core/src/engine/engine_worker.cpp', 'if (nextSource <= nextBoundary) {', 'if (nextSource < nextBoundary) {', 'engine'),
 @('Delayed NoteOff (boundary wins ties)', 'v2/core/src/engine/engine_worker.cpp', 'if (nextBoundary < nextSource || (nextBoundary == nextSource && !sourcePrecedesRecovery)) {', 'if (nextBoundary <= nextSource) {', 'engine'),
 @('Note start rate gate (release suppression)', 'v2/core/src/models/rate_gates.cpp', 'if (get(node).accepted) emissions.push_back(midiEvent);', 'if (true) emissions.push_back(midiEvent);', 'engine'),
 @('Note start rate gate (disabled channel retire)', 'v2/core/src/engine/engine_worker.cpp', 'limiter.retire_channel(channel);', '(void)channel;', 'engine'),
 @('Output rate gate (two-slot note rule)', 'v2/core/src/models/rate_gates.cpp', 'newAvailable = 2;', 'newAvailable = 1;', 'engine'),
 @('Output rate gate (dispatch clock is the real time, not the source time)', 'v2/core/src/engine/engine_worker.cpp', 'int32_t count = gate.end_source_tick(emissions, ticks_to_micros(now_stamp()));', 'int32_t count = gate.end_source_tick(emissions, sourceTime);', 'engine'),
 @('Output rate gate (disabled channel retire)', 'v2/core/src/engine/engine_worker.cpp', 'gate.retire_channel(channel);', '(void)channel;', 'engine'),
 @('Service time (events/sec interval)', 'v2/core/src/models/rate.cpp', 'int64_t numerator = remainder_ + 1000000;', 'int64_t numerator = remainder_ + 2000000;', 'engine'),
 @('Service time (bitrate ceiling)', 'v2/core/src/models/rate.cpp', 'return numerator % bitrate != 0 ? quotient + 1 : quotient;', 'return quotient;', 'engine'),
 @('Service time (processing time)', 'v2/core/src/models/rate.cpp', 'if (mode == ServiceDurationMode::ProcessingTime) return max64(0, processingMicroseconds);', 'if (mode == ServiceDurationMode::ProcessingTime) return max64(0, processingMicroseconds - 1);', 'engine'),
 @('Output-paced timeline (hold length)', 'v2/core/src/engine/playback_engine.cpp', 'max64(0, models::wrap_sub(now_stamp(), output_paced_hold_start_stamp_)));', '0);', 'engine'),
 @('Output-paced timeline (service completion stamp)', 'v2/core/src/engine/engine_worker.cpp', 'completionStamp = serviceHold ? saturating_add_ticks(now_stamp(), serviceTicks) : kInt64Max;', 'completionStamp = serviceHold ? saturating_add_ticks(now_stamp(), 0) : kInt64Max;', 'engine'),
 @('Chase (routing filter)', 'v2/core/src/engine/playback_engine.cpp', 'song.chase_index->messages(eventIndexExclusive, &routing, &overrides)', 'song.chase_index->messages(eventIndexExclusive, nullptr, &overrides)', 'engine'),
 @('Live overrides (suppression test)', 'v2/core/src/engine/channel_state.cpp', 'return forcedValue != kAutoValue && sourceValue != forcedValue;', 'return forcedValue != kAutoValue;', 'engine'),
 @('Live overrides (sustain wire value)', 'v2/core/src/engine/channel_state.cpp', 'static_cast<uint8_t>(value == 0 ? 0 : 127); break;', 'static_cast<uint8_t>(value == 0 ? 0 : 64); break;', 'engine'),
 @('Disabled channels (silence controller)', 'v2/core/src/engine/playback_engine.cpp', 'send_protected_event(ChannelOverrideState::create_controller_message(request.channel, 120, 0, 0));', 'send_protected_event(ChannelOverrideState::create_controller_message(request.channel, 121, 0, 0));', 'engine'),
 @('Ordered generations (Pause clock shift)', 'v2/core/src/engine/engine_ordered.cpp', 'adapter.shift_source_clock_anchors(pausedTicks);', 'adapter.shift_source_clock_anchors(0);', 'engine'),
 @('Ordered finite admission (soft limit)', 'v2/core/src/models/ordered_admission.cpp', 'if (protection == OutputWorkProtection::Ordinary || protection == OutputWorkProtection::OrdinaryNoteAttack) {', 'if (protection == OutputWorkProtection::Ordinary || protection == OutputWorkProtection::OrdinaryNoteAttack || protection == OutputWorkProtection::SafetyTraffic) {', 'engine'),
 @('Statistics snapshot (maximum queue length)', 'v2/core/src/engine/playback_engine.cpp', 's.maximum_queue_length = maximum_queue_length_;', 's.maximum_queue_length = outstanding_events_;', 'engine'),
 @('Statistics (Stop clears the speed frontier)', 'v2/core/src/engine/playback_engine.cpp', 'if (channelBoundary == ChannelTransitionBoundary::Stop) effective_speed_frontier_microseconds_ = 0;', '(void)channelBoundary;', 'engine'),
 @('Routing playback (routed channel in the status byte)', 'v2/core/src/engine/playback_engine.cpp', 'midiEvent.routed_channel_plus_one != 0 ? midiEvent.status() : midiEvent.bytes[0]', 'midiEvent.bytes[0]', 'engine'),
 @('SysEx send path (payload threshold)', 'v2/core/src/engine/playback_engine.cpp', 'if (midiEvent.data_length > 3 && store != nullptr) {', 'if (midiEvent.data_length > 4 && store != nullptr) {', 'engine'),
 @('Service time (events/sec remainder rounding up)', 'v2/core/src/models/rate.cpp', 'return static_cast<int64_t>(n / d);', 'return static_cast<int64_t>((n + 1) / d);', 'engine'),
 @('Service time (events/sec remainder not carried)', 'v2/core/src/models/rate.cpp', 'remainder_ = static_cast<int64_t>(n % d);', 'remainder_ = 0;', 'engine'),
 @('Output-paced timeline (ordered service ticks)', 'v2/core/src/engine/engine_ordered.cpp', 'adapter.shift_source_clock_anchors(saturating_add_ticks(pacedServiceTicks, sendTicks));', 'adapter.shift_source_clock_anchors(sendTicks);', 'engine'),
 @('Output-paced timeline (ordered finite service ticks)', 'v2/core/src/engine/engine_ordered.cpp', 'core.shift_source_clock_anchors(saturating_add_ticks(pacedServiceTicks, sendTicks));', 'core.shift_source_clock_anchors(sendTicks);', 'engine'),
 @('Ordered finite admission (Clear buffer ignores filtered arrivals)', 'v2/core/src/engine/engine_ordered.cpp', 'core.is_at_or_above_capacity(capacity) && !filtered) {', 'core.is_at_or_above_capacity(capacity)) {', 'engine'),
 @('Override cleared while paused cancels its pending message (DD-006)', 'v2/core/src/engine/channel_state.cpp', 'else pending_masks_[static_cast<size_t>(channel)] &= ~(1 << static_cast<int>(attribute));', '', 'engine'),
 @('Pause restarts after the paused time (DD-007)', 'v2/core/src/engine/playback_engine.cpp', 'start_event_index_ = find_first_event_after(*song, pausedMicroseconds);', 'start_event_index_ = find_first_event_at_or_after(*song, pausedMicroseconds);', 'engine'),
 @('Continuation start skips the events at the start time (DD-007)', 'v2/core/src/engine/playback_engine.cpp', 'start_event_index_ = continuation ? find_first_event_after(*song, startMicroseconds)', 'start_event_index_ = false ? find_first_event_after(*song, startMicroseconds)', 'engine'),
 @('Ordered Drop counts suppressed NoteOffs (DD-008)', 'v2/core/src/engine/engine_ordered.cpp', 'if (result.disposition == models::OrderedFiniteAdmissionDisposition::Rejected)<NL>            dropped++;', 'if (result.disposition == models::OrderedFiniteAdmissionDisposition::Rejected && !result.paired_release_suppressed)<NL>            dropped++;', 'engine'),
 @('Ordered gate pressure publishes the next source event (DD-009)', 'v2/core/src/engine/engine_ordered.cpp', 'published_structural_next_arrival_ = nextSource;', 'published_structural_next_arrival_ = 0;', 'engine'),
 @('Source filter shortcut (channels with an override or disabled pass unchecked, DD-093)', 'v2/core/src/engine/playback_engine.cpp', 'return (channel_routing_.disabled_mask() & (1 << channel)) == 0 && channel_overrides_.status_mask(channel) == 0;', 'return true;', 'engine'),
 @('Output rate gate (dispatch time + 1 us)', 'v2/core/src/engine/engine_worker.cpp', 'int32_t count = gate.end_source_tick(emissions, ticks_to_micros(now_stamp()));', 'int32_t count = gate.end_source_tick(emissions, ticks_to_micros(now_stamp()) + 1);', 'engine')
)
$m = $m + $e
# Family 'burst' (Milestone 7, DD-065 to DD-067): the immediate-dispatch burst, its direct send from the stored events,
# its lock-free in-service publication, the due-time threshold and the galloping search. The lock-step harness cannot see
# these paths (its output sets a send observer, which selects the decoded path, and every send takes virtual time, so every
# group is one event), so the detector is midibn-engine-tests (test_direct_send_equivalence, test_burst_lag_statistics,
# test_burst_snapshot_while_send_blocked, test_time_conversion): the run must fail.
$b = @(
 @('Burst direct send (two-byte messages lose their data byte)', 'v2/core/src/engine/engine_worker.cpp', 'static const uint32_t kInlineMask[4] = { 0u, 0xFFu, 0xFFFFu, 0xFFFFFFu };', 'static const uint32_t kInlineMask[4] = { 0u, 0xFFu, 0xFFu, 0xFFFFFFu };', 'burst'),
 @('Burst direct send (a SysEx sent as a short message)', 'v2/core/src/engine/engine_worker.cpp', 'if (!record.is_long() && record.kind() != EventKind::SystemExclusive) {', 'if (!record.is_long()) {', 'burst'),
 @('Burst lag (stale clock reading)', 'v2/core/src/engine/engine_worker.cpp', 'int64_t elapsedTicks = models::wrap_sub(stamp, stopwatchAtBatchStart);', 'int64_t elapsedTicks = models::wrap_sub(previousStamp, stopwatchAtBatchStart);', 'burst'),
 @('Burst publication (the event in service is not written)', 'v2/core/src/engine/engine_worker.cpp', 'burst_event_.store(eventIndex, std::memory_order_relaxed);', '(void)eventIndex;', 'burst'),
 @('Burst publication (next event off by one)', 'v2/core/src/engine/playback_engine.cpp', 'if (mode_ == ProcessingMode::Queue) published_next_process_ = event + 1;', 'if (mode_ == ProcessingMode::Queue) published_next_process_ = event;', 'burst'),
 @('Due threshold (off by one microsecond)', 'v2/core/include/midibn/engine/engine_types.h', 'microseconds = ((ticks + 1) * scale - 1) / frequency_;', 'microseconds = ((ticks + 1) * scale) / frequency_;', 'burst'),
 @('Due search (strict comparison)', 'v2/core/src/engine/playback_engine.cpp', 'if (event_micros(probe) > latestDue) { high = probe; break; }', 'if (event_micros(probe) >= latestDue) { high = probe; break; }', 'burst'),
 @('Channel monitor batch (a group of sends not recorded, DD-092)', 'v2/core/src/engine/engine_worker.cpp', 'channel_state_.record_successful_batch(sentRecords, count);', '(void)count;', 'burst'),
 @('Channel monitor record from the stored form (second data byte lost, DD-092)', 'v2/core/include/midibn/engine/channel_state.h', 'r.second = length > 2 ? static_cast<uint8_t>(record.packed >> 16) : 0;', 'r.second = 0;', 'burst'),
 @('Snapshot eligible count (passed blocks not subtracted, DD-093)', 'v2/core/include/midibn/engine/snapshot_eligible_counter.h', 'total_ -= blocks_.front();', '(void)0;', 'burst')
)
$m = $m + $b
# Family 'direct' (DD-094): the direct WinMM output's call gate and the player's Play guard (Stop during a flood on a driver
# whose MODM_DATA blocks while its queue is full). Detected by the test program named in the sixth field; the run must fail.
$d = @(
 @('Direct send gate (the one-exchange form ignores a close request)', 'v2/platform/win32/winmm_direct_output.cpp', '        if ((gate_.load(std::memory_order_acquire) & kGateClosed) == 0) return 1;', '        if (true) return 1;', 'direct', 'midibn-direct-output-tests'),
 @('Direct send gate (the leaving send does not finish a deferred close)', 'v2/platform/win32/winmm_direct_output.cpp', '    if ((flags & kGateDeferred) != 0 && (flags & kGateCount) == 0) claim_deferred_close();', '    if (false) claim_deferred_close();', 'direct', 'midibn-direct-output-tests'),
 @('Direct keep open (every open closes and reopens)', 'v2/platform/win32/winmm_direct_output.cpp', '!abandoned_.load(std::memory_order_acquire) && openedDeviceId_ == deviceId && enter()) {', '!abandoned_.load(std::memory_order_acquire) && openedDeviceId_ == deviceId && false) {', 'direct', 'midibn-direct-output-tests'),
 @('Player keeps a direct output open after a device change while stopped', 'v2/core/src/app/player_controller.cpp', '        if (direct_ && !engine_.has_live_worker()) close_output(direct_);', '        if (false) close_output(direct_);', 'direct', 'midibn-app-tests'),
 @('Direct close (resets and closes underneath a call in progress)', 'v2/platform/win32/winmm_direct_output.cpp', '    if (wait_for_calls_to_finish()) {', '    if (wait_for_calls_to_finish() || true) {', 'direct', 'midibn-direct-output-tests'),
 @('Direct busy retry (keeps retrying after a close request)', 'v2/platform/win32/winmm_direct_output.cpp', '        if ((gate_.load(std::memory_order_acquire) & kGateClosed) != 0) {', '        if (false) {', 'direct', 'midibn-direct-output-tests'),
 @('Direct open (opens again while a close is pending)', 'v2/platform/win32/winmm_direct_output.cpp', '    if (!previous.ok()) return previous;', '    (void)previous;', 'direct', 'midibn-direct-output-tests'),
 @('Player Play guard (reopens the output under a stuck call)', 'v2/core/src/app/player_controller.cpp', '    if (engine_.has_live_worker()) {', '    if (false) {', 'direct', 'midibn-app-tests'),
 @('Direct reset/close without a time limit', 'v2/platform/win32/winmm_direct_output.cpp', 'WaitForSingleObject(reinterpret_cast<HANDLE>(thread), static_cast<DWORD>(limitMilliseconds));', 'WaitForSingleObject(reinterpret_cast<HANDLE>(thread), INFINITE);', 'direct', 'midibn-direct-output-tests'),
 @('Direct driver that stopped answering is opened again', 'v2/platform/win32/winmm_direct_output.cpp', '    if (driver_unresponsive(utf8_to_wide(resolution.driver_file), &hung))', '    if (false)', 'direct', 'midibn-direct-output-tests'),
 @('Direct driver that stopped answering is reset and closed anyway', 'v2/platform/win32/winmm_direct_output.cpp', '    if (!abandoned_.load(std::memory_order_acquire) && reset_device()) {', '    if (reset_device()) {', 'direct', 'midibn-direct-output-tests'),
 @('Player status line (stopped answering not shown)', 'v2/core/src/app/player_controller.cpp', '    if (sameDevice && direct_outcome_ == DirectOutcome::InUse && direct_ && direct_->driver_stopped_answering()) {', '    if (false) {', 'direct', 'midibn-app-tests')
)
$m = $m + $d
# Coverage closed by scenarios (section I of engine-scenarios.txt): the events/sec remainder (rounding up, not
# carried: rate-eps13-*), the ordered Output-paced service ticks (ordered-output-paced-rate-1us-*), the ordered Clear
# buffer filtered-arrival test (ordered-drop-policy-clear-buffer-channels-off*) and the Output rate decision time
# (+1 us: gate-output-rate-limit-chase-*).
# Mutations found equivalent and replaced: the paused wait's wake-generation re-check closes a lost-wakeup race that
# lock-step delivery (commands only at scheduling points) cannot produce; the spin loop's wake poll and the wait's already-due test
# (remaining ticks equal to 0) and its not-playing test are likewise never reached; `limit < 1 ? 1 : limit` in the ordered finite admission cannot differ because the engine
# already clamps the queue limit to at least 1.
function Strip([string]$text) { ($text -split "`n" | Where-Object { -not $_.StartsWith('#') } | ForEach-Object { $_.TrimEnd("`r") }) }
$dump = 'v2\out\build-x64\Release\midibn-dump.exe'
$harness = 'v2\out\build-x64\Release\midibn-engine-harness.exe'
$engineArgs = @('--fixtures', 'v2\harness\fixtures', '--scenarios', 'v2\harness\engine-scenarios.txt', '--expected', 'v2\harness\expected\engine',
    '--rules', 'v2\harness\expected\differences-from-v1.txt', '--baseline', 'v2\harness\expected-v2\engine', '--fail-fast')
$engineTimeoutMs = 900000
New-Item -ItemType Directory -Force -Path 'v2\out\models' | Out-Null
New-Item -ItemType Directory -Force -Path 'v2\out\mutation' | Out-Null
$detected = 0
if ($Family) { $m = @($m | Where-Object { $Family -contains $_[4] }) }
# Runs the engine comparison; returns @{ Exit; Text; Hung }. A run that exceeds the timeout is killed (a mutation
# that makes the engine hang is a visible failure, not a pass).
function Invoke-EngineComparison([string]$log) {
    $p = Start-Process -FilePath $harness -ArgumentList $engineArgs -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle   # keeps the process handle so ExitCode is available
    $hung = -not $p.WaitForExit($engineTimeoutMs)
    if ($hung) { try { $p.Kill() } catch { } ; $p.WaitForExit() }
    $text = [IO.File]::ReadAllText($log)
    # PASS needs exit code 0 and the harness's own RESULT: PASS line.
    return @{ Exit = $(if ($hung) { -1 } elseif ($p.ExitCode -eq 0 -and $text -match "(?m)^RESULT: PASS") { 0 } else { 1 }); Text = $text; Hung = $hung }
}
if ($m | Where-Object { $_[4] -like 'engine*' }) {
    # The unmutated engine must pass first, or every mutation would "fail" trivially.
    $o = & $cm --build v2/out/build-x64 --config Release --target midibn-engine-harness 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAIL (unmutated engine harness)'; exit 2 }
    $base = Invoke-EngineComparison 'v2\out\mutation\engine-baseline.log'
    if ($base.Exit -ne 0) { Write-Host 'BASELINE FAIL: the unmutated engine comparison does not pass; fix that first'; exit 2 }
}
if ($m | Where-Object { $_[4] -eq 'burst' }) {
    # The unmutated engine tests must pass first.
    $o = & $cm --build v2/out/build-x64 --config Release --target midibn-engine-tests 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAIL (unmutated engine tests)'; exit 2 }
    & 'v2\out\build-x64\Release\midibn-engine-tests.exe' *> $null
    if ($LASTEXITCODE -ne 0) { Write-Host 'BASELINE FAIL: the unmutated engine tests do not pass; fix that first'; exit 2 }
}
if ($m | Where-Object { $_[4] -eq 'direct' }) {
    # The unmutated direct-output and app tests must pass first (app tests run from the repository root).
    foreach ($t in 'midibn-direct-output-tests', 'midibn-app-tests') {
        $o = & $cm --build v2/out/build-x64 --config Release --target $t 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAIL (unmutated $t)"; exit 2 }
        & "v2\out\build-x64\Release\$t.exe" *> $null
        if ($LASTEXITCODE -ne 0) { Write-Host "BASELINE FAIL: the unmutated $t does not pass; fix that first"; exit 2 }
    }
}
$n = 0
foreach ($x in $m) {
    $n++
    $path = $x[1]; $orig = [IO.File]::ReadAllText($path)
    $nl = if ($orig.Contains("`r`n")) { "`r`n" } else { "`n" }
    $find = $x[2].Replace('<NL>', $nl); $repl = $x[3].Replace('<NL>', $nl)
    $hits = ([regex]::Matches($orig, [regex]::Escape($find))).Count
    if ($hits -ne 1) { Write-Host "SKIP $($x[0]): pattern found $hits times"; continue }
    try {
        [IO.File]::WriteAllText($path, $orig.Replace($find, $repl), (New-Object Text.UTF8Encoding $false))
        $target = if ($x[4] -like 'engine*') { 'midibn-engine-harness' } elseif ($x[4] -eq 'burst') { 'midibn-engine-tests' } elseif ($x[4] -eq 'direct') { $x[5] } else { 'midibn-dump' }
        $o = & $cm --build v2/out/build-x64 --config Release --target $target 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAIL $($x[0])"; continue }
        if ($x[4] -eq 'burst' -or $x[4] -eq 'direct') {
            $log = "v2\out\mutation\$($x[4])-{0:00}.log" -f $n
            $p = Start-Process -FilePath "v2\out\build-x64\Release\$target.exe" -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
            $null = $p.Handle
            $hung = -not $p.WaitForExit(300000)
            if ($hung) { try { $p.Kill() } catch { }; $p.WaitForExit() }
            $firstFail = ([IO.File]::ReadAllText($log) -split "`n" | Where-Object { $_ -like 'FAIL*' } | Select-Object -First 1)
            if ($hung) { $detected++; Write-Host "DETECTED $($x[0]): the engine tests hung (killed after 300 s)" }
            elseif ($p.ExitCode -ne 0) { $detected++; Write-Host ("DETECTED {0}: {1}" -f $x[0], $(if ($firstFail) { $firstFail.Trim() } else { "exit $($p.ExitCode)" })) }
            else { Write-Host "NOT DETECTED $($x[0])" }
            continue
        }
        if ($x[4] -like 'engine*') {
            $r = Invoke-EngineComparison ("v2\out\mutation\engine-{0:00}.log" -f $n)
            $firstFail = ($r.Text -split "`n" | Where-Object { $_ -like 'FAIL engine*' } | Select-Object -First 1)
            if ($r.Hung) { $detected++; Write-Host "DETECTED $($x[0]): engine run hung (killed after $($engineTimeoutMs / 1000) s)" }
            elseif ($r.Exit -ne 0) { $detected++; Write-Host ("DETECTED {0}: {1}" -f $x[0], $(if ($firstFail) { $firstFail.Trim() } else { "exit $($r.Exit)" })) }
            else { Write-Host "NOT DETECTED $($x[0])" }
            continue
        }
        & $dump --models "v2\harness\expected\models\$($x[4]).trace" --fixtures v2\harness\fixtures --out "v2\out\models\mut.trace"
        $a = Strip ([IO.File]::ReadAllText("v2\harness\expected\models\$($x[4]).trace"))
        $b = Strip ([IO.File]::ReadAllText("v2\out\models\mut.trace"))
        $first = -1; $count = 0
        for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) { if ($a[$i] -ne $b[$i]) { $count++; if ($first -lt 0) { $first = $i } } }
        if ($first -ge 0) { $detected++; Write-Host ("DETECTED {0}: {1}.trace first difference at line {2} ({3} lines differ)" -f $x[0], $x[4], ($first + 1), $count) }
        else { Write-Host "NOT DETECTED $($x[0])" }
    }
    finally { [IO.File]::WriteAllText($path, $orig, (New-Object Text.UTF8Encoding $false)) }
}
& $cm --build v2/out/build-x64 --config Release 2>&1 | Out-Null
$buildExit = $LASTEXITCODE
$total = $m.Count

# ---- Family 'harness': tests of the comparison machinery (difference rules and v2 baseline) ----
if (-not $Family -or $Family -contains 'harness') {
    $parity = 'v2\out\build-x64\Release\midibn-parity.exe'
    $work = 'v2\out\mutation\harness'
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    Copy-Item -Recurse 'v2\harness\expected' "$work\expected"
    Copy-Item -Recurse 'v2\harness\expected-v2' "$work\expected-v2"
    $realRules = [IO.File]::ReadAllText('v2\harness\expected\differences-from-v1.txt')
    $quick = '--engine-fixtures', 'sysex-variants.mid,models-notes.mid'   # the engine on two fixtures: seconds, not a minute
    $caseNumber = 0
    function Invoke-Parity([string]$rulesText, [string]$log) {
        $rulesPath = "$work\rules.txt"
        [IO.File]::WriteAllText($rulesPath, $rulesText, (New-Object Text.UTF8Encoding $false))
        $a = @('--fixtures', 'v2\harness\fixtures', '--expected', "$work\expected", '--rules', $rulesPath, '--baseline', "$work\expected-v2",
            '--scenarios', 'v2\harness\engine-scenarios.txt') + $quick
        $out = & $parity @a 2>&1
        $code = $LASTEXITCODE
        [IO.File]::WriteAllLines((Join-Path (Get-Location).Path $log), [string[]]$out)
        return @{ Exit = $code; Text = ($out -join "`n") }
    }
    function Edit-File([string]$path, [scriptblock]$change) {
        $before = [IO.File]::ReadAllText($path)
        $after = & $change $before
        if ($after -eq $before) { throw "harness case: the edit of $path changed nothing" }
        [IO.File]::WriteAllText($path, $after, (New-Object Text.UTF8Encoding $false))
        return $before
    }
    # Runs one case: the mutation is applied to the copies, parity is run, the copies are restored. $expectText must
    # appear in the output of a run that fails (exit code not 0).
    function Test-Case([string]$name, [string]$rulesText, [string]$file, [scriptblock]$change, [string]$expectText) {
        $script:caseNumber++
        $log = "v2\out\mutation\harness-{0:00}.log" -f $script:caseNumber
        $original = $null
        try {
            if ($file) { $original = Edit-File $file $change }
            $r = Invoke-Parity $rulesText $log
        }
        finally { if ($file -and $null -ne $original) { [IO.File]::WriteAllText($file, $original, (New-Object Text.UTF8Encoding $false)) } }
        return @{ Name = $name; Result = $r; Expect = $expectText }
    }
    function Report-Detected($case) {
        $r = $case.Result
        $line = ($r.Text -split "`n" | Where-Object { $_ -like "*$($case.Expect)*" } | Select-Object -First 1)
        if ($r.Exit -ne 0 -and $line) { $script:detected++; Write-Host ("DETECTED {0}: {1}" -f $case.Name, $line.Trim()) }
        elseif ($r.Exit -eq 0) { Write-Host "NOT DETECTED $($case.Name): the run passed" }
        else { Write-Host "NOT DETECTED $($case.Name): the run failed, but not with the expected message '$($case.Expect)'" }
    }

    # Controls: the unchanged copies pass; an explained difference passes.
    $c0 = Test-Case 'control' $realRules $null $null ''
    if ($c0.Result.Exit -ne 0) { Write-Host 'HARNESS CONTROL FAIL: the unchanged expected files do not pass'; $c0.Result.Text -split "`n" | Where-Object { $_ -like 'FAIL*' } | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }; exit 2 }
    $v1digests = "$work\expected\engine\models-notes.digests"
    $v2digests = "$work\expected-v2\engine\models-notes.digests"
    $explainedRule = $realRules + "`nengine DD-001 channel-seek-playing@models-notes explained`n"
    $firstDigest = { param($t) [regex]::Replace($t, '(?m)^(channel-seek-playing )([0-9A-F])', { param($mm) $mm.Groups[1].Value + $(if ($mm.Groups[2].Value -eq '0') { '1' } else { '0' }) }) }
    $c1 = Test-Case 'control (an explained difference)' $explainedRule $v1digests $firstDigest ''
    if ($c1.Result.Exit -ne 0 -or $c1.Result.Text -notmatch 'explained by DD-001') { Write-Host 'HARNESS CONTROL FAIL: an engine difference explained by an engine rule did not pass'; $c1.Result.Text -split "`n" | Where-Object { $_ -like 'FAIL*' } | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }; exit 2 }

    $total += 8
    # (1) an unexplained new difference from v1 in a trace
    Report-Detected (Test-Case 'Unexplained difference from v1 (trace line)' $realRules "$work\expected\models-notes-mid.trace" { param($t) $t -replace '(?m)^e 1 ', 'e 7777 ' } 'FAIL models-notes.mid')
    # (2) ... in an engine recording
    Report-Detected (Test-Case 'Unexplained difference from v1 (engine recording)' $realRules $v1digests $firstDigest 'no rule explains it')
    # (3) the rules that explain DD-001 removed
    Report-Detected (Test-Case 'Rules for a real difference removed (DD-001)' "format 1`n" $null $null 'FAIL sysex-short.mid')
    # (4) a stale trace rule
    Report-Detected (Test-Case 'Stale trace rule (no longer occurs)' ($realRules + "`ntrace DD-001 models-notes-mid.trace /.*/ /.*/`n") $null $null 'no longer occurs')
    # (5) a stale engine rule
    Report-Detected (Test-Case 'Stale engine rule (no longer occurs)' ($realRules + "`nengine DD-001 *@models-notes explained`n") $null $null 'no longer occurs')
    # (6) v2 baseline drift in a trace
    Report-Detected (Test-Case 'v2 baseline drift (trace)' $realRules "$work\expected-v2\traces.sha256" { param($t) [regex]::Replace($t, '(?m)^(models-notes-mid\.trace )([0-9A-F])', { param($mm) $mm.Groups[1].Value + $(if ($mm.Groups[2].Value -eq '0') { '1' } else { '0' }) }) } 'FAIL baseline models-notes.mid')
    # (7) v2 baseline drift in an engine recording
    Report-Detected (Test-Case 'v2 baseline drift (engine recording)' $realRules $v2digests $firstDigest 'differs from the v2 baseline')
    # (8) an explained difference from v1 does not excuse a change against the v2 baseline: the v1 digest AND the
    #     v2 baseline are altered, the engine rule explains the v1 difference, the baseline still fails
    $before1 = [IO.File]::ReadAllText($v1digests)
    try {
        [IO.File]::WriteAllText($v1digests, (& $firstDigest $before1), (New-Object Text.UTF8Encoding $false))
        Report-Detected (Test-Case 'Explained difference still checked against the v2 baseline' $explainedRule $v2digests $firstDigest 'differs from the v2 baseline')
    }
    finally { [IO.File]::WriteAllText($v1digests, $before1, (New-Object Text.UTF8Encoding $false)) }
}

Write-Host "Detected $detected of $total mutations; rebuilt clean: exit $buildExit"
if ($detected -ne $total -or $buildExit -ne 0) { exit 1 }
exit 0   # (the last harness case leaves a failing parity exit code behind; the result is the line above)
