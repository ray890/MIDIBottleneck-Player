using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum DelayedNoteOffAdmission
    {
        NotNote,
        Accepted
    }

    // Shared live/offline bandwidth-reduction state machine. Ownership is
    // global per pitch: tracks and MIDI channels compete for one transmitted
    // voice, while the accepted owner's actual output channel is retained.
    internal sealed class DelayedNoteOffGate
    {
        private const int ChannelCount = 16;
        private const int PitchCount = 128;
        private const int MatchSlotCount = ChannelCount * PitchCount;
        private const int OccurrenceChunkSize = 4096;
        private const int MaximumLiveOccurrences = 1000000;
        private const long MaximumRecoveryMicroseconds = 40000;

        private const byte Idle = 0;
        private const byte AttackPending = 1;
        private const byte Held = 2;
        private const byte Recovery = 3;

        private const byte Owner = 1;
        private const byte InsideDuration = 2;
        private const byte RecoveryCandidate = 3;
        private const byte RecoveryLost = 4;
        private const byte RecoveryEnded = 5;
        private const byte DisplacedOwner = 6;

        private const byte TransitionAttack = 1;
        private const byte TransitionRelease = 2;
        private const byte TransitionRecovery = 3;

        private struct SourceOccurrence
        {
            internal bool InUse;
            internal bool KeyReleased;
            internal bool SustainHeld;
            internal byte Disposition;
            internal int Track;
            internal int Channel;
            internal int Pitch;
            internal int PreviousActive;
            internal int NextActive;
            internal int NextFree;
            internal long Order;
            internal MidiEventView NoteOn;
            internal MidiEventView NoteOff;
        }

        private struct DueTransition
        {
            internal int Pitch;
            internal byte Kind;
            internal long Order;
        }

        private readonly long _minimumActiveMicroseconds;
        private readonly long _recoveryMicroseconds;
        private readonly bool _intervalPolyphonyLimitEnabled;
        private readonly int _intervalPolyphonyLimit;
        private readonly List<SourceOccurrence[]> _occurrenceChunks = new List<SourceOccurrence[]>();
        private readonly int[] _activeHead = new int[MatchSlotCount];
        private readonly int[] _activeTail = new int[MatchSlotCount];
        private readonly bool[] _sustainDown = new bool[ChannelCount];
        private readonly byte[] _state = new byte[PitchCount];
        private readonly int[] _ownerOccurrence = new int[PitchCount];
        private readonly int[] _candidateOccurrence = new int[PitchCount];
        private readonly MidiEventView[] _attack = new MidiEventView[PitchCount];
        private readonly long[] _attackDue = new long[PitchCount];
        private readonly long[] _attackEmission = new long[PitchCount];
        private readonly long[] _attackOrder = new long[PitchCount];
        private readonly bool[] _releasePending = new bool[PitchCount];
        private readonly bool[] _releaseForRetrigger = new bool[PitchCount];
        private readonly MidiEventView[] _release = new MidiEventView[PitchCount];
        private readonly long[] _releaseDue = new long[PitchCount];
        private readonly long[] _releaseOrder = new long[PitchCount];
        private readonly long[] _recoveryDue = new long[PitchCount];
        private readonly long[] _recoveryOrder = new long[PitchCount];
        private readonly List<DueTransition> _dueTransitions = new List<DueTransition>(PitchCount);
        private readonly DueTransition[] _attackTransitions = new DueTransition[PitchCount];
        private readonly MusicalNotePriorityCandidate[] _musicalCandidates =
            new MusicalNotePriorityCandidate[PitchCount];
        private readonly MusicalNotePrioritySelector _musicalPriority =
            new MusicalNotePrioritySelector();
        private readonly bool[] _selectedAttackPitch = new bool[PitchCount];
        private int _musicalFairnessCursor;
        private int _freeOccurrence = -1;
        private int _nextUnusedOccurrence;
        private int _liveOccurrences;
        private long _nextBoundary = Int64.MaxValue;
        private long _sourceOrder;
        private long _filteredEvents;
        private long _insideDurationFiltered;
        private long _recoveryLostFiltered;
        private long _recoveryEndedFiltered;
        private bool _sourceComplete;

        internal DelayedNoteOffGate(long minimumActiveMicroseconds,
            bool intervalPolyphonyLimitEnabled = false,
            int intervalPolyphonyLimit = PitchCount)
        {
            if (minimumActiveMicroseconds < 0)
                throw new ArgumentOutOfRangeException("minimumActiveMicroseconds");
            if (intervalPolyphonyLimit < 1 || intervalPolyphonyLimit > PitchCount)
                throw new ArgumentOutOfRangeException("intervalPolyphonyLimit");
            _minimumActiveMicroseconds = minimumActiveMicroseconds;
            _intervalPolyphonyLimitEnabled = intervalPolyphonyLimitEnabled;
            _intervalPolyphonyLimit = intervalPolyphonyLimit;
            _recoveryMicroseconds = Math.Min(minimumActiveMicroseconds / 4,
                MaximumRecoveryMicroseconds);
            for (int slot = 0; slot < MatchSlotCount; slot++)
                _activeHead[slot] = _activeTail[slot] = -1;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                _ownerOccurrence[pitch] = -1;
                _candidateOccurrence[pitch] = -1;
                _attackDue[pitch] = _releaseDue[pitch] = _recoveryDue[pitch] = Int64.MaxValue;
            }
        }

        internal long MinimumActiveMicroseconds { get { return _minimumActiveMicroseconds; } }
        internal long RecoveryMicroseconds { get { return _recoveryMicroseconds; } }
        internal long NextBoundaryMicroseconds { get { return _nextBoundary; } }
        internal bool HasPendingTransitions { get { return _nextBoundary != Int64.MaxValue; } }
        internal int NextBoundaryOutputCount
        {
            get
            {
                if (_nextBoundary == Int64.MaxValue) return 0;
                int count = 0;
                for (int pitch = 0; pitch < PitchCount; pitch++)
                {
                    if (_state[pitch] == AttackPending && _attackDue[pitch] == _nextBoundary)
                        count++;
                    else if (_state[pitch] == Held && _releasePending[pitch] &&
                        _releaseDue[pitch] == _nextBoundary)
                        count++;
                    else if (_state[pitch] == Recovery && _recoveryDue[pitch] == _nextBoundary &&
                        _candidateOccurrence[pitch] >= 0)
                        count++;
                }
                return _intervalPolyphonyLimitEnabled
                    ? Math.Min(count, _intervalPolyphonyLimit) : count;
            }
        }
        internal int LiveOccurrenceCountForTesting { get { return _liveOccurrences; } }
        internal int OccurrenceSegmentCountForTesting { get { return _occurrenceChunks.Count; } }
        internal long InsideDurationFilteredForTesting { get { return _insideDurationFiltered; } }
        internal long RecoveryLostFilteredForTesting { get { return _recoveryLostFiltered; } }
        internal long RecoveryEndedFilteredForTesting { get { return _recoveryEndedFiltered; } }

        internal long TakeFilteredEventCount()
        {
            long result = _filteredEvents;
            _filteredEvents = 0;
            return result;
        }

        // Recovery candidates whose source transition occurs exactly at the
        // recovery boundary must participate before the winner is emitted.
        // Releases win all other exact-time ties, including the D boundary.
        internal bool SourcePrecedesBoundary(MidiEventView midiEvent)
        {
            return SourcePrecedesBoundary(midiEvent, _nextBoundary);
        }

        internal bool SourcePrecedesBoundary(MidiEventView midiEvent,
            long effectiveBoundaryMicroseconds)
        {
            if (_nextBoundary == Int64.MaxValue ||
                midiEvent.IntendedMicroseconds != effectiveBoundaryMicroseconds) return false;
            int pitch;
            bool noteOn;
            if (PerNoteIntervalGate.TryClassify(midiEvent, out pitch, out noteOn))
                return _intervalPolyphonyLimitEnabled ||
                    _state[pitch] == Recovery &&
                    _recoveryDue[pitch] <= effectiveBoundaryMicroseconds;
            if (!IsSustainController(midiEvent)) return false;
            int channel = midiEvent.Channel;
            for (pitch = 0; pitch < PitchCount; pitch++)
            {
                if (_state[pitch] != Recovery ||
                    _recoveryDue[pitch] > effectiveBoundaryMicroseconds) continue;
                int candidate = _candidateOccurrence[pitch];
                if (candidate >= 0 && GetOccurrence(candidate).Channel == channel) return true;
            }
            return false;
        }

        internal DelayedNoteOffAdmission Admit(MidiEventView midiEvent)
        {
            int pitch;
            bool noteOn;
            if (!PerNoteIntervalGate.TryClassify(midiEvent, out pitch, out noteOn))
            {
                ObserveSustain(midiEvent);
                RecalculateNextBoundary();
                return DelayedNoteOffAdmission.NotNote;
            }

            long order = _sourceOrder++;
            if (noteOn) AdmitAttack(pitch, midiEvent, order);
            else AdmitRelease(pitch, midiEvent, order);
            RecalculateNextBoundary();
            return DelayedNoteOffAdmission.Accepted;
        }

        internal int EmitBoundary(long boundaryMicroseconds, List<MidiEventView> emissions)
        {
            if (emissions == null) throw new ArgumentNullException("emissions");
            emissions.Clear();
            CollectDueTransitions(boundaryMicroseconds);
            if (!_intervalPolyphonyLimitEnabled)
            {
                _dueTransitions.Sort(CompareLegacyTransitions);
                for (int index = 0; index < _dueTransitions.Count; index++)
                {
                    DueTransition transition = _dueTransitions[index];
                    if (transition.Kind == TransitionAttack)
                        EmitAttack(transition.Pitch, boundaryMicroseconds, emissions);
                    else if (transition.Kind == TransitionRelease)
                        EmitRelease(transition.Pitch, boundaryMicroseconds, emissions);
                    else
                        EmitRecoveredAttack(transition.Pitch, boundaryMicroseconds, emissions);
                }
                RecalculateNextBoundary();
                return emissions.Count;
            }
            _dueTransitions.Sort(CompareTransitions);
            int emittedReleases = 0;
            int attackCount = 0;
            for (int index = 0; index < _dueTransitions.Count; index++)
            {
                DueTransition transition = _dueTransitions[index];
                _selectedAttackPitch[transition.Pitch] = false;
                if (transition.Kind == TransitionRelease)
                {
                    if (emittedReleases < _intervalPolyphonyLimit)
                    {
                        EmitRelease(transition.Pitch, boundaryMicroseconds, emissions);
                        emittedReleases++;
                    }
                    else
                        _releaseDue[transition.Pitch] = AddSaturated(
                            boundaryMicroseconds, _minimumActiveMicroseconds);
                }
                else if (transition.Kind == TransitionRecovery &&
                    _candidateOccurrence[transition.Pitch] < 0)
                    EmitRecoveredAttack(transition.Pitch, boundaryMicroseconds, emissions);
                else
                    _attackTransitions[attackCount++] = transition;
            }

            int attackCapacity = _intervalPolyphonyLimit - emissions.Count;
            while (attackCount > attackCapacity)
            {
                for (int index = 0; index < attackCount; index++)
                {
                    DueTransition transition = _attackTransitions[index];
                    MidiEventView attack = AttackForTransition(transition);
                    _musicalCandidates[index] = new MusicalNotePriorityCandidate
                    {
                        Pitch = transition.Pitch,
                        Velocity = attack.GetDataByte(2) & 0x7F,
                        Track = attack.Track,
                        Channel = attack.Channel,
                        SourceOrder = attack.EventIndex,
                        IntendedMicroseconds = boundaryMicroseconds,
                        DurationMicroseconds = DurationForTransition(transition)
                    };
                }
                int victimIndex = _musicalPriority.SelectLeastImportant(
                    _musicalCandidates, attackCount, _musicalFairnessCursor);
                _musicalFairnessCursor = _musicalPriority.NextFairnessCursor(
                    _musicalCandidates[victimIndex]);
                RejectDueAttack(_attackTransitions[victimIndex]);
                attackCount--;
                _attackTransitions[victimIndex] = _attackTransitions[attackCount];
            }
            for (int index = 0; index < attackCount; index++)
                _selectedAttackPitch[_attackTransitions[index].Pitch] = true;
            for (int index = 0; index < _dueTransitions.Count; index++)
            {
                DueTransition transition = _dueTransitions[index];
                if (!_selectedAttackPitch[transition.Pitch]) continue;
                if (transition.Kind == TransitionAttack)
                    EmitAttack(transition.Pitch, boundaryMicroseconds, emissions);
                else if (transition.Kind == TransitionRecovery)
                    EmitRecoveredAttack(transition.Pitch, boundaryMicroseconds, emissions);
            }
            RecalculateNextBoundary();
            return emissions.Count;
        }

        private MidiEventView AttackForTransition(DueTransition transition)
        {
            if (transition.Kind == TransitionAttack) return _attack[transition.Pitch];
            int candidate = _candidateOccurrence[transition.Pitch];
            return candidate < 0 ? default(MidiEventView) : GetOccurrence(candidate).NoteOn;
        }

        private long DurationForTransition(DueTransition transition)
        {
            int node = transition.Kind == TransitionAttack
                ? _ownerOccurrence[transition.Pitch] : _candidateOccurrence[transition.Pitch];
            if (node < 0) return Int64.MaxValue;
            SourceOccurrence occurrence = GetOccurrence(node);
            return occurrence.NoteOff.IsValid
                ? Math.Max(0, occurrence.NoteOff.IntendedMicroseconds -
                    occurrence.NoteOn.IntendedMicroseconds) : Int64.MaxValue;
        }

        private void RejectDueAttack(DueTransition transition)
        {
            int pitch = transition.Pitch;
            if (transition.Kind == TransitionRecovery)
            {
                int candidate = _candidateOccurrence[pitch];
                _candidateOccurrence[pitch] = -1;
                _recoveryDue[pitch] = Int64.MaxValue;
                if (candidate >= 0) RejectRecoveryCandidate(candidate, RecoveryLost);
                _state[pitch] = Idle;
                return;
            }

            int owner = _ownerOccurrence[pitch];
            _ownerOccurrence[pitch] = -1;
            if (owner >= 0)
            {
                SourceOccurrence occurrence = GetOccurrence(owner);
                occurrence.Disposition = RecoveryLost;
                CountFiltered(RecoveryLost, 1);
                if (occurrence.KeyReleased)
                {
                    CountFiltered(RecoveryLost, 1);
                    RemoveAndFree(owner);
                }
                else SetOccurrence(owner, occurrence);
            }
            _attack[pitch] = default(MidiEventView);
            _attackDue[pitch] = Int64.MaxValue;
            _releasePending[pitch] = false;
            _releaseForRetrigger[pitch] = false;
            _release[pitch] = default(MidiEventView);
            _releaseDue[pitch] = Int64.MaxValue;
            _state[pitch] = Idle;
        }

        internal void CompleteSource(long sourceEndMicroseconds)
        {
            if (_sourceComplete) return;
            _sourceComplete = true;
            sourceEndMicroseconds = Math.Max(0, sourceEndMicroseconds);
            for (int slot = 0; slot < MatchSlotCount; slot++)
            {
                int node = _activeHead[slot];
                while (node >= 0)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    int next = occurrence.NextActive;
                    if (occurrence.Disposition == Owner)
                    {
                        occurrence.KeyReleased = true;
                        occurrence.SustainHeld = false;
                        SetOccurrence(node, occurrence);
                        ScheduleOwnerRelease(occurrence.Pitch, default(MidiEventView),
                            sourceEndMicroseconds, false);
                    }
                    else if (occurrence.Disposition == RecoveryCandidate)
                    {
                        if (_candidateOccurrence[occurrence.Pitch] == node)
                            _candidateOccurrence[occurrence.Pitch] = -1;
                        CountFiltered(RecoveryEnded, 1);
                        RemoveAndFree(node);
                    }
                    else
                        RemoveAndFree(node);
                    node = next;
                }
            }
            RecalculateNextBoundary();
        }

        // The ordered caller already emitted the controller/channel safety
        // message. Retire only state owned by that effective output channel.
        internal int RetireChannel(int channel, long nowMicroseconds)
        {
            if (channel < 0 || channel >= ChannelCount) return 0;
            _sustainDown[channel] = false;
            int routed = 0;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                int node = _activeHead[MatchSlot(channel, pitch)];
                while (node >= 0)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    int next = occurrence.NextActive;
                    if (occurrence.Disposition == RecoveryCandidate &&
                        _candidateOccurrence[pitch] == node)
                    {
                        _candidateOccurrence[pitch] = -1;
                        routed++;
                    }
                    if (occurrence.Disposition == Owner && _ownerOccurrence[pitch] == node)
                    {
                        _ownerOccurrence[pitch] = -1;
                        _state[pitch] = Idle;
                        _releasePending[pitch] = false;
                        _attackDue[pitch] = _releaseDue[pitch] = _recoveryDue[pitch] = Int64.MaxValue;
                    }
                    RemoveAndFree(node);
                    node = next;
                }
                if (_state[pitch] == Recovery && _candidateOccurrence[pitch] < 0)
                {
                    // Recovery itself is global pitch timing; leave its bounded
                    // deadline intact even when this channel's candidate left.
                }
            }
            RecalculateNextBoundary();
            return routed;
        }

        internal static bool IsNoteTerminatingControl(MidiEventView midiEvent)
        {
            if (!midiEvent.IsValid || midiEvent.Channel < 0 || midiEvent.Channel >= ChannelCount ||
                midiEvent.DataLength < 2 || (midiEvent.Status & 0xF0) != 0xB0) return false;
            int controller = midiEvent.GetDataByte(1) & 0x7F;
            return controller == 120 || controller >= 123 && controller <= 127;
        }

        private void AdmitAttack(int pitch, MidiEventView midiEvent, long order)
        {
            if (_state[pitch] == Idle)
            {
                int owner = AllocateOccurrence(midiEvent, pitch, Owner, order);
                _ownerOccurrence[pitch] = owner;
                _attack[pitch] = midiEvent;
                _attackDue[pitch] = midiEvent.IntendedMicroseconds;
                _attackOrder[pitch] = order;
                _state[pitch] = AttackPending;
                return;
            }

            // Adaptive scheduling can keep an otherwise-due first attack
            // pending while later source attacks arrive. D starts only at an
            // actual transmitted NoteOn, so these occurrences compete for the
            // one boundary owner rather than being classified inside D.
            if (_state[pitch] == AttackPending)
            {
                SelectPendingAttackOwner(pitch, midiEvent, order);
                return;
            }

            long lastAttack = _attackEmission[pitch];
            if (_state[pitch] == Held && midiEvent.IntendedMicroseconds <
                AddSaturated(lastAttack, _minimumActiveMicroseconds))
            {
                AllocateOccurrence(midiEvent, pitch, InsideDuration, order);
                CountFiltered(InsideDuration, 1);
                return;
            }

            // Once D has matured, a same-pitch attack is the current source
            // batch's eligible retrigger. Keep at most one musically selected
            // occurrence for this pitch while its required old-owner release
            // and bounded R protection complete. This is not a general gate
            // queue: later contenders replace or lose to this single slot.
            if (_state[pitch] == Held)
            {
                int retrigger = AllocateOccurrence(midiEvent, pitch,
                    RecoveryCandidate, order);
                SelectRecoveryCandidate(pitch, retrigger);
                ScheduleOwnerRelease(pitch, default(MidiEventView),
                    midiEvent.IntendedMicroseconds, true);
                return;
            }

            // Recovery owns one bounded candidate slot for the pitch. Later
            // contenders may replace or lose to that slot, but they cannot
            // accumulate a second interval batch or a general attack queue.
            int candidate = AllocateOccurrence(midiEvent, pitch, RecoveryCandidate, order);
            SelectRecoveryCandidate(pitch, candidate);
        }

        private void SelectPendingAttackOwner(int pitch, MidiEventView midiEvent, long order)
        {
            int current = _ownerOccurrence[pitch];
            if (current < 0)
                throw new InvalidOperationException("A pending Delayed NoteOff attack has no owner occurrence.");
            SourceOccurrence existing = GetOccurrence(current);
            if (!PerNoteIntervalGate.IsBetterCandidate(midiEvent, existing.NoteOn))
            {
                int losing = AllocateOccurrence(midiEvent, pitch, RecoveryLost, order);
                CountFiltered(RecoveryLost, 1);
                SourceOccurrence loser = GetOccurrence(losing);
                SetOccurrence(losing, loser);
                return;
            }

            existing.Disposition = RecoveryLost;
            CountFiltered(RecoveryLost, 1);
            if (existing.KeyReleased)
            {
                CountFiltered(RecoveryLost, 1);
                RemoveAndFree(current);
            }
            else
                SetOccurrence(current, existing);

            int replacement = AllocateOccurrence(midiEvent, pitch, Owner, order);
            _ownerOccurrence[pitch] = replacement;
            _attack[pitch] = midiEvent;
            _attackOrder[pitch] = order;
            _releasePending[pitch] = false;
            _releaseForRetrigger[pitch] = false;
            _release[pitch] = default(MidiEventView);
            _releaseDue[pitch] = Int64.MaxValue;
        }

        private void AdmitRelease(int pitch, MidiEventView midiEvent, long order)
        {
            int node = MatchOccurrence(midiEvent.Track, midiEvent.Channel, pitch);
            if (node < 0) return;
            SourceOccurrence occurrence = GetOccurrence(node);
            occurrence.KeyReleased = true;
            occurrence.NoteOff = midiEvent;
            SetOccurrence(node, occurrence);

            if (occurrence.Disposition == InsideDuration ||
                occurrence.Disposition == RecoveryLost ||
                occurrence.Disposition == RecoveryEnded)
            {
                CountFiltered(occurrence.Disposition, 1);
                RemoveAndFree(node);
                return;
            }
            if (occurrence.Disposition == DisplacedOwner)
            {
                RemoveAndFree(node);
                return;
            }

            if (_sustainDown[occurrence.Channel])
            {
                occurrence.SustainHeld = true;
                SetOccurrence(node, occurrence);
                return;
            }
            ResolveSupportedRelease(node, occurrence, midiEvent.IntendedMicroseconds);
        }

        private void ResolveSupportedRelease(int node, SourceOccurrence occurrence, long releaseTime)
        {
            occurrence.SustainHeld = false;
            SetOccurrence(node, occurrence);
            if (occurrence.Disposition == RecoveryCandidate)
            {
                if (_candidateOccurrence[occurrence.Pitch] == node)
                    _candidateOccurrence[occurrence.Pitch] = -1;
                CountFiltered(RecoveryEnded, 2);
                RemoveAndFree(node);
                return;
            }
            if (occurrence.Disposition == Owner &&
                _ownerOccurrence[occurrence.Pitch] == node)
            {
                ScheduleOwnerRelease(occurrence.Pitch, occurrence.NoteOff,
                    releaseTime, false);
                return;
            }
            RemoveAndFree(node);
        }

        private void SelectRecoveryCandidate(int pitch, int candidate)
        {
            int current = _candidateOccurrence[pitch];
            if (current < 0)
            {
                _candidateOccurrence[pitch] = candidate;
                return;
            }
            SourceOccurrence proposed = GetOccurrence(candidate);
            SourceOccurrence existing = GetOccurrence(current);
            if (PerNoteIntervalGate.IsBetterCandidate(proposed.NoteOn, existing.NoteOn))
            {
                RejectRecoveryCandidate(current, RecoveryLost);
                _candidateOccurrence[pitch] = candidate;
            }
            else
                RejectRecoveryCandidate(candidate, RecoveryLost);
        }

        private void RejectRecoveryCandidate(int node, byte reason)
        {
            SourceOccurrence occurrence = GetOccurrence(node);
            occurrence.Disposition = reason;
            CountFiltered(reason, 1);
            if (occurrence.KeyReleased)
            {
                CountFiltered(reason, 1);
                RemoveAndFree(node);
            }
            else
                SetOccurrence(node, occurrence);
        }

        private void ScheduleOwnerRelease(int pitch, MidiEventView source, long requested,
            bool retrigger)
        {
            if (_state[pitch] != Held && _state[pitch] != AttackPending) return;
            long due = Math.Max(Math.Max(0, requested),
                AddSaturated(_state[pitch] == AttackPending ? _attackDue[pitch] : _attackEmission[pitch],
                    _minimumActiveMicroseconds));
            if (!_releasePending[pitch] || due < _releaseDue[pitch] || retrigger)
            {
                _releasePending[pitch] = true;
                _releaseForRetrigger[pitch] = retrigger;
                _release[pitch] = source;
                _releaseDue[pitch] = retrigger ? Math.Max(0, requested) : due;
                _releaseOrder[pitch] = _sourceOrder++;
            }
        }

        private void EmitAttack(int pitch, long boundary, List<MidiEventView> emissions)
        {
            if (_state[pitch] != AttackPending || _attackDue[pitch] > boundary) return;
            emissions.Add(_attack[pitch]);
            _attackEmission[pitch] = boundary;
            _attackDue[pitch] = Int64.MaxValue;
            _state[pitch] = Held;
            if (_releasePending[pitch] && !_releaseForRetrigger[pitch])
                _releaseDue[pitch] = Math.Max(_releaseDue[pitch],
                    AddSaturated(boundary, _minimumActiveMicroseconds));
        }

        private void EmitRelease(int pitch, long boundary, List<MidiEventView> emissions)
        {
            if (_state[pitch] != Held || !_releasePending[pitch] || _releaseDue[pitch] > boundary)
                return;
            int owner = _ownerOccurrence[pitch];
            int channel = owner >= 0 ? GetOccurrence(owner).Channel : _attack[pitch].Channel;
            emissions.Add(CreateRelease(_release[pitch], pitch, channel, boundary));
            bool retrigger = _releaseForRetrigger[pitch];
            _releasePending[pitch] = false;
            _releaseForRetrigger[pitch] = false;
            _releaseDue[pitch] = Int64.MaxValue;
            if (owner >= 0)
            {
                SourceOccurrence occurrence = GetOccurrence(owner);
                _ownerOccurrence[pitch] = -1;
                if (retrigger && !occurrence.KeyReleased)
                {
                    occurrence.Disposition = DisplacedOwner;
                    SetOccurrence(owner, occurrence);
                }
                else
                    RemoveAndFree(owner);
            }
            _state[pitch] = Recovery;
            _recoveryDue[pitch] = AddSaturated(boundary, _recoveryMicroseconds);
            _recoveryOrder[pitch] = _releaseOrder[pitch];
        }

        private void EmitRecoveredAttack(int pitch, long boundary, List<MidiEventView> emissions)
        {
            if (_state[pitch] != Recovery || _recoveryDue[pitch] > boundary) return;
            int candidate = _candidateOccurrence[pitch];
            _candidateOccurrence[pitch] = -1;
            _recoveryDue[pitch] = Int64.MaxValue;
            if (candidate < 0)
            {
                _state[pitch] = Idle;
                return;
            }
            SourceOccurrence occurrence = GetOccurrence(candidate);
            if (occurrence.KeyReleased && !occurrence.SustainHeld)
            {
                CountFiltered(RecoveryEnded, 2);
                RemoveAndFree(candidate);
                _state[pitch] = Idle;
                return;
            }
            occurrence.Disposition = Owner;
            SetOccurrence(candidate, occurrence);
            _ownerOccurrence[pitch] = candidate;
            _attack[pitch] = occurrence.NoteOn;
            _attackEmission[pitch] = boundary;
            emissions.Add(occurrence.NoteOn);
            _state[pitch] = Held;
            if (occurrence.KeyReleased)
                ScheduleOwnerRelease(pitch, occurrence.NoteOff, boundary, false);
        }

        private void CollectDueTransitions(long boundary)
        {
            _dueTransitions.Clear();
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                if (_state[pitch] == AttackPending && _attackDue[pitch] <= boundary)
                    _dueTransitions.Add(new DueTransition { Pitch = pitch,
                        Kind = TransitionAttack, Order = _attackOrder[pitch] });
                else if (_state[pitch] == Held && _releasePending[pitch] &&
                    _releaseDue[pitch] <= boundary)
                    _dueTransitions.Add(new DueTransition { Pitch = pitch,
                        Kind = TransitionRelease, Order = _releaseOrder[pitch] });
                else if (_state[pitch] == Recovery && _recoveryDue[pitch] <= boundary)
                    _dueTransitions.Add(new DueTransition { Pitch = pitch,
                        Kind = TransitionRecovery, Order = CandidateOrder(pitch) });
            }
        }

        private long CandidateOrder(int pitch)
        {
            int candidate = _candidateOccurrence[pitch];
            return candidate >= 0 ? GetOccurrence(candidate).Order : _recoveryOrder[pitch];
        }

        private void ObserveSustain(MidiEventView midiEvent)
        {
            if (!IsSustainController(midiEvent)) return;
            int channel = midiEvent.Channel;
            bool down = (midiEvent.GetDataByte(2) & 0x7F) >= 64;
            if (_sustainDown[channel] == down) return;
            _sustainDown[channel] = down;
            if (down) return;
            long time = midiEvent.IntendedMicroseconds;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                int node = _activeHead[MatchSlot(channel, pitch)];
                while (node >= 0)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    int next = occurrence.NextActive;
                    if (occurrence.KeyReleased && occurrence.SustainHeld)
                        ResolveSupportedRelease(node, occurrence, time);
                    node = next;
                }
            }
        }

        private static bool IsSustainController(MidiEventView midiEvent)
        {
            return midiEvent.IsValid && midiEvent.Channel >= 0 && midiEvent.Channel < ChannelCount &&
                midiEvent.DataLength >= 3 && (midiEvent.Status & 0xF0) == 0xB0 &&
                (midiEvent.GetDataByte(1) & 0x7F) == 64;
        }

        private void RecalculateNextBoundary()
        {
            long next = Int64.MaxValue;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                if (_state[pitch] == AttackPending) next = Math.Min(next, _attackDue[pitch]);
                else if (_state[pitch] == Held && _releasePending[pitch])
                    next = Math.Min(next, _releaseDue[pitch]);
                else if (_state[pitch] == Recovery) next = Math.Min(next, _recoveryDue[pitch]);
            }
            _nextBoundary = next;
        }

        private int AllocateOccurrence(MidiEventView noteOn, int pitch, byte disposition, long order)
        {
            if (_liveOccurrences >= MaximumLiveOccurrences)
                throw new InvalidOperationException(
                    "Delayed NoteOff has more than 1,000,000 simultaneous live note occurrences.");
            int node;
            if (_freeOccurrence >= 0)
            {
                node = _freeOccurrence;
                _freeOccurrence = GetOccurrence(node).NextFree;
            }
            else
            {
                node = _nextUnusedOccurrence++;
                int chunk = node / OccurrenceChunkSize;
                if (chunk == _occurrenceChunks.Count)
                    _occurrenceChunks.Add(new SourceOccurrence[OccurrenceChunkSize]);
            }
            SourceOccurrence occurrence = new SourceOccurrence
            {
                InUse = true,
                Disposition = disposition,
                Track = noteOn.Track,
                Channel = noteOn.Channel,
                Pitch = pitch,
                PreviousActive = -1,
                NextActive = -1,
                NextFree = -1,
                Order = order,
                NoteOn = noteOn
            };
            SetOccurrence(node, occurrence);
            AppendActive(node, occurrence);
            _liveOccurrences++;
            return node;
        }

        private int MatchOccurrence(int track, int channel, int pitch)
        {
            if (channel < 0 || channel >= ChannelCount) return -1;
            int first = -1;
            for (int node = _activeHead[MatchSlot(channel, pitch)]; node >= 0;
                node = GetOccurrence(node).NextActive)
            {
                SourceOccurrence occurrence = GetOccurrence(node);
                if (occurrence.KeyReleased) continue;
                if (first < 0) first = node;
                if (occurrence.Track == track) return node;
            }
            return first;
        }

        private void AppendActive(int node, SourceOccurrence occurrence)
        {
            int slot = MatchSlot(occurrence.Channel, occurrence.Pitch);
            occurrence.PreviousActive = _activeTail[slot];
            if (_activeTail[slot] >= 0)
            {
                SourceOccurrence tail = GetOccurrence(_activeTail[slot]);
                tail.NextActive = node;
                SetOccurrence(_activeTail[slot], tail);
            }
            else _activeHead[slot] = node;
            _activeTail[slot] = node;
            SetOccurrence(node, occurrence);
        }

        private void RemoveAndFree(int node)
        {
            SourceOccurrence occurrence = GetOccurrence(node);
            if (!occurrence.InUse) return;
            int slot = MatchSlot(occurrence.Channel, occurrence.Pitch);
            if (occurrence.PreviousActive >= 0)
            {
                SourceOccurrence previous = GetOccurrence(occurrence.PreviousActive);
                previous.NextActive = occurrence.NextActive;
                SetOccurrence(occurrence.PreviousActive, previous);
            }
            else _activeHead[slot] = occurrence.NextActive;
            if (occurrence.NextActive >= 0)
            {
                SourceOccurrence next = GetOccurrence(occurrence.NextActive);
                next.PreviousActive = occurrence.PreviousActive;
                SetOccurrence(occurrence.NextActive, next);
            }
            else _activeTail[slot] = occurrence.PreviousActive;
            SetOccurrence(node, new SourceOccurrence { NextFree = _freeOccurrence,
                PreviousActive = -1, NextActive = -1 });
            _freeOccurrence = node;
            _liveOccurrences--;
        }

        private void CountFiltered(byte reason, long count)
        {
            if (count <= 0) return;
            _filteredEvents += count;
            if (reason == InsideDuration) _insideDurationFiltered += count;
            else if (reason == RecoveryLost) _recoveryLostFiltered += count;
            else if (reason == RecoveryEnded) _recoveryEndedFiltered += count;
        }

        private SourceOccurrence GetOccurrence(int node)
        {
            return _occurrenceChunks[node / OccurrenceChunkSize][node % OccurrenceChunkSize];
        }

        private void SetOccurrence(int node, SourceOccurrence occurrence)
        {
            _occurrenceChunks[node / OccurrenceChunkSize][node % OccurrenceChunkSize] = occurrence;
        }

        private static int CompareTransitions(DueTransition left, DueTransition right)
        {
            if (left.Kind != right.Kind)
            {
                if (left.Kind == TransitionRelease) return -1;
                if (right.Kind == TransitionRelease) return 1;
            }
            if (left.Order != right.Order) return left.Order < right.Order ? -1 : 1;
            return left.Pitch.CompareTo(right.Pitch);
        }

        private static int CompareLegacyTransitions(DueTransition left,
            DueTransition right)
        {
            if (left.Order != right.Order) return left.Order < right.Order ? -1 : 1;
            if (left.Kind != right.Kind)
            {
                if (left.Kind == TransitionRelease) return -1;
                if (right.Kind == TransitionRelease) return 1;
            }
            return left.Pitch.CompareTo(right.Pitch);
        }

        private static int MatchSlot(int channel, int pitch) { return channel * PitchCount + pitch; }

        private static long AddSaturated(long value, long increment)
        {
            return increment > 0 && value > Int64.MaxValue - increment
                ? Int64.MaxValue : value + increment;
        }

        private static MidiEventView CreateRelease(MidiEventView source, int pitch, int channel,
            long boundaryMicroseconds)
        {
            if (channel < 0 || channel >= ChannelCount) channel = 0;
            byte command = source.IsValid && (source.Status & 0xF0) == 0x90
                ? (byte)0x90 : (byte)0x80;
            byte status = (byte)(command | channel);
            byte velocity = source.IsValid && source.DataLength > 2
                ? source.GetDataByte(2) : (byte)0;
            return MidiEventView.Create(source.IsValid ? source.AbsoluteTick : 0,
                source.IsValid ? source.IntendedMicroseconds : boundaryMicroseconds,
                source.IsValid ? source.Track : -1, MidiEventKind.NoteOff, channel,
                status, source.IsValid ? source.EventIndex : -1,
                MidiEventData.FromShort(status, (byte)pitch, velocity, 3));
        }
    }
}
