using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum PerNoteGateAdmission
    {
        NotNote,
        Accepted,
        Filtered
    }

    // Live, bounded-state pitch transition gate. Source occurrences remain
    // distinct by track/channel/pitch, while constrained output has one
    // Up/Down state per pitch. The bounded pool represents only simultaneously
    // unmatched notes and never scales with file size.
    internal sealed class PerNoteIntervalGate
    {
        private const int PitchCount = 128;
        private const int ChannelCount = 16;
        private const int OccurrenceChunkSize = 4096;

        private struct SourceOccurrence
        {
            internal bool InUse;
            internal bool Ended;
            internal bool RemovedByRouting;
            internal byte Disposition; // 0 undecided, 1 selected, 2 rejected
            internal int Track;
            internal int Channel;
            internal int Pitch;
            internal int NextActive;
            internal int NextGroup;
            internal int NextFree;
            internal MidiEvent NoteOn;
            internal MidiEvent NoteOff;
        }

        private readonly long _intervalMicroseconds;
        private readonly bool[] _outputDown = new bool[PitchCount];
        private readonly int[] _outputChannel = new int[PitchCount];
        private readonly int[] _selectedSupportCount = new int[PitchCount];
        private readonly long[] _minimumReleaseBoundary = new long[PitchCount];
        private readonly long[] _nextAttackBoundary = new long[PitchCount];

        private readonly MidiEvent[] _pendingAttack = new MidiEvent[PitchCount];
        private readonly long[] _attackBoundary = new long[PitchCount];
        private readonly long[] _preparatoryReleaseBoundary = new long[PitchCount];
        private readonly MidiEvent[] _naturalRelease = new MidiEvent[PitchCount];
        private readonly long[] _naturalReleaseBoundary = new long[PitchCount];

        // At most one candidate group per pitch is retained for an eligible
        // boundary. Distinct ticks in the same interval compete by velocity,
        // then stable source order.
        private readonly int[] _candidateHead = new int[PitchCount];
        private readonly int[] _candidateRepresentative = new int[PitchCount];
        private readonly int[] _candidateMembers = new int[PitchCount];
        private readonly long[] _candidateBoundary = new long[PitchCount];
        private readonly int[] _delayedHead = new int[PitchCount];
        private readonly int[] _delayedRepresentative = new int[PitchCount];
        private readonly int[] _delayedMembers = new int[PitchCount];
        private readonly long[] _delayedBoundary = new long[PitchCount];

        private readonly int[] _tickHead = new int[PitchCount];
        private readonly int[] _tickTail = new int[PitchCount];
        private readonly int[] _tickRepresentative = new int[PitchCount];
        private readonly int[] _tickMembers = new int[PitchCount];
        private long _sourceTick = Int64.MinValue;

        private readonly List<SourceOccurrence[]> _occurrenceChunks = new List<SourceOccurrence[]>();
        private readonly int[] _activeHead = new int[ChannelCount * PitchCount];
        private readonly int[] _activeTail = new int[ChannelCount * PitchCount];
        private int _freeOccurrence = -1;
        private int _nextUnusedOccurrence;
        private long _nextBoundary = Int64.MaxValue;
        private long _filteredEvents;
        private bool _sourceComplete;

        internal PerNoteIntervalGate(long intervalMicroseconds)
        {
            if (intervalMicroseconds <= 0) throw new ArgumentOutOfRangeException("intervalMicroseconds");
            _intervalMicroseconds = intervalMicroseconds;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                _outputChannel[pitch] = -1;
                _candidateHead[pitch] = _candidateRepresentative[pitch] = -1;
                _delayedHead[pitch] = _delayedRepresentative[pitch] = -1;
                _tickHead[pitch] = _tickTail[pitch] = _tickRepresentative[pitch] = -1;
                _attackBoundary[pitch] = Int64.MaxValue;
                _preparatoryReleaseBoundary[pitch] = Int64.MaxValue;
                _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                _delayedBoundary[pitch] = Int64.MaxValue;
            }
            for (int index = 0; index < _activeHead.Length; index++)
                _activeHead[index] = _activeTail[index] = -1;
        }

        internal long IntervalMicroseconds { get { return _intervalMicroseconds; } }
        internal long NextBoundaryMicroseconds { get { return _nextBoundary; } }
        internal bool HasPendingTransitions { get { return _nextBoundary != Int64.MaxValue; } }

        internal void BeginSourceTick(long absoluteTick)
        {
            if (_sourceTick != Int64.MinValue) EndSourceTick();
            _sourceTick = absoluteTick;
        }

        internal PerNoteGateAdmission Admit(MidiEvent midiEvent)
        {
            int pitch;
            bool noteOn;
            if (!TryClassify(midiEvent, out pitch, out noteOn)) return PerNoteGateAdmission.NotNote;
            if (_sourceTick == Int64.MinValue) BeginSourceTick(midiEvent.AbsoluteTick);

            if (noteOn)
            {
                int node = AllocateOccurrence(midiEvent, pitch);
                AddToTickGroup(pitch, node);
                return PerNoteGateAdmission.Accepted;
            }

            int occurrence = MatchOccurrence(midiEvent.Track, midiEvent.Channel, pitch);
            if (occurrence < 0)
            {
                _filteredEvents++;
                return PerNoteGateAdmission.Filtered;
            }

            SourceOccurrence matched = GetOccurrence(occurrence);
            matched.Ended = true;
            matched.NoteOff = midiEvent;
            SetOccurrence(occurrence, matched);
            if (matched.Disposition == 0) return PerNoteGateAdmission.Accepted;
            if (matched.Disposition == 2)
            {
                _filteredEvents++;
                FreeOccurrence(occurrence);
                return PerNoteGateAdmission.Filtered;
            }

            if (_selectedSupportCount[pitch] > 0) _selectedSupportCount[pitch]--;
            if (_selectedSupportCount[pitch] == 0)
            {
                ScheduleNaturalRelease(pitch, midiEvent, midiEvent.IntendedMicroseconds);
                FreeOccurrence(occurrence);
                return PerNoteGateAdmission.Accepted;
            }

            _filteredEvents++;
            FreeOccurrence(occurrence);
            return PerNoteGateAdmission.Filtered;
        }

        internal void EndSourceTick()
        {
            if (_sourceTick == Int64.MinValue) return;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                int head = _tickHead[pitch];
                if (head < 0) continue;
                int representative = _tickRepresentative[pitch];
                long boundary = BoundaryAtOrAfter(GetOccurrence(representative).NoteOn.IntendedMicroseconds);
                if (_candidateHead[pitch] < 0)
                    StoreCandidate(pitch, head, representative, _tickMembers[pitch], boundary);
                else if (_candidateBoundary[pitch] == boundary &&
                    IsBetterRepresentative(representative, _candidateRepresentative[pitch]))
                {
                    RejectGroup(_candidateHead[pitch]);
                    StoreCandidate(pitch, head, representative, _tickMembers[pitch], boundary);
                }
                else
                    RejectGroup(head);

                _tickHead[pitch] = _tickTail[pitch] = _tickRepresentative[pitch] = -1;
                _tickMembers[pitch] = 0;
            }
            _sourceTick = Int64.MinValue;
            RecalculateNextBoundary();
        }

        // Source events at this timestamp must already have been admitted.
        // Emits no more than one transition per pitch in ascending-pitch order.
        internal int EmitBoundary(long boundaryMicroseconds, MidiEvent[] output)
        {
            if (output == null || output.Length < PitchCount)
                throw new ArgumentException("A 128-entry output buffer is required.", "output");
            EndSourceTick();
            int count = 0;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                ResolveDelayedCandidate(pitch, boundaryMicroseconds);
                if (_candidateHead[pitch] >= 0 && _candidateBoundary[pitch] <= boundaryMicroseconds)
                    FinalizeCandidate(pitch);

                if (_outputDown[pitch] &&
                    (_preparatoryReleaseBoundary[pitch] <= boundaryMicroseconds ||
                     _naturalReleaseBoundary[pitch] <= boundaryMicroseconds))
                {
                    bool natural = _naturalReleaseBoundary[pitch] <= boundaryMicroseconds &&
                        _selectedSupportCount[pitch] == 0;
                    MidiEvent releaseSource = natural ? _naturalRelease[pitch] : null;
                    int velocity = releaseSource == null ? 0 : releaseSource.GetDataByte(2) & 0x7F;
                    output[count++] = CreateNoteOff(releaseSource, pitch, _outputChannel[pitch], velocity,
                        boundaryMicroseconds);
                    _outputDown[pitch] = false;
                    _outputChannel[pitch] = -1;
                    _preparatoryReleaseBoundary[pitch] = Int64.MaxValue;
                    if (natural)
                    {
                        _naturalRelease[pitch] = null;
                        _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                    }
                    if (_pendingAttack[pitch] != null && _attackBoundary[pitch] <= boundaryMicroseconds)
                        _attackBoundary[pitch] = AddInterval(boundaryMicroseconds);
                    continue;
                }

                MidiEvent attack = _pendingAttack[pitch];
                if (attack != null && _attackBoundary[pitch] <= boundaryMicroseconds)
                {
                    if (_outputDown[pitch])
                    {
                        output[count++] = CreateNoteOff(null, pitch, _outputChannel[pitch], 0, boundaryMicroseconds);
                        _outputDown[pitch] = false;
                        _outputChannel[pitch] = -1;
                        _attackBoundary[pitch] = AddInterval(boundaryMicroseconds);
                        continue;
                    }
                    output[count++] = attack;
                    _outputDown[pitch] = true;
                    _outputChannel[pitch] = attack.Channel;
                    _minimumReleaseBoundary[pitch] = AddInterval(boundaryMicroseconds);
                    _pendingAttack[pitch] = null;
                    _attackBoundary[pitch] = Int64.MaxValue;
                    _preparatoryReleaseBoundary[pitch] = Int64.MaxValue;
                    if (_selectedSupportCount[pitch] == 0)
                        ScheduleNaturalRelease(pitch, _naturalRelease[pitch], _minimumReleaseBoundary[pitch]);
                    continue;
                }

                if (_outputDown[pitch] && _selectedSupportCount[pitch] == 0 &&
                    _naturalReleaseBoundary[pitch] <= boundaryMicroseconds)
                {
                    MidiEvent release = _naturalRelease[pitch];
                    int velocity = release == null ? 0 : release.GetDataByte(2) & 0x7F;
                    output[count++] = CreateNoteOff(release, pitch, _outputChannel[pitch], velocity,
                        boundaryMicroseconds);
                    _outputDown[pitch] = false;
                    _outputChannel[pitch] = -1;
                    _naturalRelease[pitch] = null;
                    _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                }
            }
            RecalculateNextBoundary();
            return count;
        }

        internal void CompleteSource(long sourceEndMicroseconds)
        {
            if (_sourceComplete) return;
            EndSourceTick();
            _sourceComplete = true;
            sourceEndMicroseconds = Math.Max(0, sourceEndMicroseconds);
            for (int key = 0; key < _activeHead.Length; key++)
            {
                int node = _activeHead[key];
                _activeHead[key] = _activeTail[key] = -1;
                while (node >= 0)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    int next = occurrence.NextActive;
                    occurrence.Ended = true;
                    occurrence.NextActive = -1;
                    SetOccurrence(node, occurrence);
                    if (occurrence.Disposition == 1 && _selectedSupportCount[occurrence.Pitch] > 0)
                        _selectedSupportCount[occurrence.Pitch]--;
                    if (occurrence.Disposition != 0) FreeOccurrence(node);
                    node = next;
                }
            }
            for (int pitch = 0; pitch < PitchCount; pitch++)
                if (_selectedSupportCount[pitch] == 0 &&
                    (_outputDown[pitch] || _pendingAttack[pitch] != null))
                    ScheduleNaturalRelease(pitch, null, sourceEndMicroseconds);
            RecalculateNextBoundary();
        }

        // The ordered channel-disable control already sent channel safety
        // messages. Remove unmatched source support and pending transitions
        // without reclassifying them as gate overload.
        internal int RetireChannel(int channel, long nowMicroseconds)
        {
            if (channel < 0 || channel >= ChannelCount) return 0;
            int routedFiltered = 0;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                int key = Key(channel, pitch);
                int node = _activeHead[key];
                _activeHead[key] = _activeTail[key] = -1;
                while (node >= 0)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    int next = occurrence.NextActive;
                    occurrence.Ended = true;
                    occurrence.RemovedByRouting = true;
                    occurrence.NextActive = -1;
                    SetOccurrence(node, occurrence);
                    if (occurrence.Disposition == 0) routedFiltered++;
                    else if (occurrence.Disposition == 1 && _selectedSupportCount[pitch] > 0)
                        _selectedSupportCount[pitch]--;
                    if (occurrence.Disposition != 0) FreeOccurrence(node);
                    node = next;
                }

                if (_pendingAttack[pitch] != null && _pendingAttack[pitch].Channel == channel)
                {
                    _pendingAttack[pitch] = null;
                    _attackBoundary[pitch] = Int64.MaxValue;
                    _preparatoryReleaseBoundary[pitch] = Int64.MaxValue;
                    routedFiltered++;
                }
                if (_outputDown[pitch] && _outputChannel[pitch] == channel)
                {
                    _outputDown[pitch] = false;
                    _outputChannel[pitch] = -1;
                    _naturalRelease[pitch] = null;
                    _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                    _preparatoryReleaseBoundary[pitch] = Int64.MaxValue;
                }
                if (!_outputDown[pitch] && _pendingAttack[pitch] == null &&
                    _selectedSupportCount[pitch] > 0)
                {
                    int replacement = FindBestSelectedSupport(pitch);
                    if (replacement >= 0)
                    {
                        long boundary = BoundaryAtOrAfter(Math.Max(0, nowMicroseconds));
                        if (boundary <= nowMicroseconds) boundary = AddInterval(boundary);
                        _pendingAttack[pitch] = GetOccurrence(replacement).NoteOn;
                        _attackBoundary[pitch] = boundary;
                        _naturalRelease[pitch] = null;
                        _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                    }
                }
                if (_selectedSupportCount[pitch] == 0 && _outputDown[pitch])
                    ScheduleNaturalRelease(pitch, null, nowMicroseconds);
                if (!_outputDown[pitch] && _pendingAttack[pitch] == null)
                {
                    _naturalRelease[pitch] = null;
                    _naturalReleaseBoundary[pitch] = Int64.MaxValue;
                }
            }
            RecalculateNextBoundary();
            return routedFiltered;
        }

        private int FindBestSelectedSupport(int pitch)
        {
            int best = -1;
            for (int channel = 0; channel < ChannelCount; channel++)
            {
                for (int node = _activeHead[Key(channel, pitch)]; node >= 0;
                    node = GetOccurrence(node).NextActive)
                {
                    SourceOccurrence occurrence = GetOccurrence(node);
                    if (occurrence.Ended || occurrence.RemovedByRouting || occurrence.Disposition != 1) continue;
                    if (best < 0 || IsBetterRepresentative(node, best)) best = node;
                }
            }
            return best;
        }

        internal long TakeFilteredEventCount()
        {
            long count = _filteredEvents;
            _filteredEvents = 0;
            return count;
        }

        internal bool IsActive(int pitch) { return pitch >= 0 && pitch < PitchCount && _outputDown[pitch]; }
        internal int OwnerChannel(int pitch) { return pitch < 0 || pitch >= PitchCount ? -1 : _outputChannel[pitch]; }
        internal int OccurrenceSegmentCountForTesting { get { return _occurrenceChunks.Count; } }
        internal bool HasActiveOccurrenceForTesting(int track, int channel, int pitch)
        {
            if (channel < 0 || channel >= ChannelCount || pitch < 0 || pitch >= PitchCount) return false;
            for (int node = _activeHead[Key(channel, pitch)]; node >= 0;
                node = GetOccurrence(node).NextActive)
                if (GetOccurrence(node).Track == track) return true;
            return false;
        }

        private int AllocateOccurrence(MidiEvent noteOn, int pitch)
        {
            int node;
            if (_freeOccurrence >= 0)
            {
                node = _freeOccurrence;
                _freeOccurrence = GetOccurrence(node).NextFree;
            }
            else
            {
                node = _nextUnusedOccurrence;
                EnsureOccurrenceCapacity(node);
                _nextUnusedOccurrence++;
            }
            SourceOccurrence occurrence = new SourceOccurrence();
            occurrence.InUse = true;
            occurrence.Track = noteOn.Track;
            occurrence.Channel = noteOn.Channel;
            occurrence.Pitch = pitch;
            occurrence.NextActive = -1;
            occurrence.NextGroup = -1;
            occurrence.NextFree = -1;
            occurrence.NoteOn = noteOn;
            SetOccurrence(node, occurrence);

            int key = Key(noteOn.Channel, pitch);
            int tail = _activeTail[key];
            if (tail < 0) _activeHead[key] = node;
            else
            {
                SourceOccurrence previous = GetOccurrence(tail);
                previous.NextActive = node;
                SetOccurrence(tail, previous);
            }
            _activeTail[key] = node;
            return node;
        }

        private void EnsureOccurrenceCapacity(int node)
        {
            if (node < _occurrenceChunks.Count * OccurrenceChunkSize) return;
            try { _occurrenceChunks.Add(new SourceOccurrence[OccurrenceChunkSize]); }
            catch (OutOfMemoryException exception)
            {
                throw new InvalidOperationException(
                    "Per-note interval gate could not allocate another source-note occurrence segment.", exception);
            }
        }

        private SourceOccurrence GetOccurrence(int node)
        {
            return _occurrenceChunks[node / OccurrenceChunkSize][node % OccurrenceChunkSize];
        }

        private void SetOccurrence(int node, SourceOccurrence occurrence)
        {
            _occurrenceChunks[node / OccurrenceChunkSize][node % OccurrenceChunkSize] = occurrence;
        }

        private int MatchOccurrence(int track, int channel, int pitch)
        {
            if (channel < 0 || channel >= ChannelCount) return -1;
            int key = Key(channel, pitch);
            int previous = -1;
            int node = _activeHead[key];
            int fallback = node;
            while (node >= 0)
            {
                if (GetOccurrence(node).Track == track) break;
                previous = node;
                node = GetOccurrence(node).NextActive;
            }
            if (node < 0) { node = fallback; previous = -1; }
            if (node < 0) return -1;
            int next = GetOccurrence(node).NextActive;
            if (previous < 0) _activeHead[key] = next;
            else
            {
                SourceOccurrence prior = GetOccurrence(previous);
                prior.NextActive = next;
                SetOccurrence(previous, prior);
            }
            if (_activeTail[key] == node) _activeTail[key] = previous;
            SourceOccurrence matched = GetOccurrence(node);
            matched.NextActive = -1;
            SetOccurrence(node, matched);
            return node;
        }

        private void AddToTickGroup(int pitch, int node)
        {
            int tail = _tickTail[pitch];
            if (tail < 0) _tickHead[pitch] = node;
            else
            {
                SourceOccurrence previous = GetOccurrence(tail);
                previous.NextGroup = node;
                SetOccurrence(tail, previous);
            }
            _tickTail[pitch] = node;
            _tickMembers[pitch]++;
            if (_tickRepresentative[pitch] < 0 || IsBetterRepresentative(node, _tickRepresentative[pitch]))
                _tickRepresentative[pitch] = node;
        }

        private void StoreCandidate(int pitch, int head, int representative, int members, long boundary)
        {
            _candidateHead[pitch] = head;
            _candidateRepresentative[pitch] = representative;
            _candidateMembers[pitch] = members;
            _candidateBoundary[pitch] = boundary;
        }

        private void FinalizeCandidate(int pitch)
        {
            int head = _candidateHead[pitch];
            if (head < 0) return;
            long firstBoundary = _candidateBoundary[pitch];
            long assigned = Math.Max(firstBoundary, _nextAttackBoundary[pitch]);
            if (_outputDown[pitch] || _pendingAttack[pitch] != null)
                assigned = Math.Max(assigned, AddInterval(firstBoundary));
            if (assigned > AddInterval(firstBoundary))
            {
                RejectGroup(head);
                ClearCandidate(pitch);
                return;
            }
            if (assigned > firstBoundary && _delayedHead[pitch] < 0)
            {
                _delayedHead[pitch] = head;
                _delayedRepresentative[pitch] = _candidateRepresentative[pitch];
                _delayedMembers[pitch] = _candidateMembers[pitch];
                _delayedBoundary[pitch] = assigned;
                if (_outputDown[pitch]) _preparatoryReleaseBoundary[pitch] = firstBoundary;
                ClearCandidate(pitch);
                return;
            }

            int representative = _candidateRepresentative[pitch];
            if (representative < 0 || GetOccurrence(representative).RemovedByRouting)
                representative = FindBestGroupMember(head);
            if (representative < 0)
            {
                DiscardRemovedGroup(head);
                ClearCandidate(pitch);
                return;
            }
            MidiEvent representativeEvent = GetOccurrence(representative).NoteOn;

            int liveSupports = 0;
            int endedCount = 0;
            int selectedMembers = 0;
            MidiEvent latestOff = null;
            int node = head;
            while (node >= 0)
            {
                SourceOccurrence occurrence = GetOccurrence(node);
                int next = occurrence.NextGroup;
                occurrence.NextGroup = -1;
                if (!occurrence.RemovedByRouting)
                {
                    selectedMembers++;
                    occurrence.Disposition = 1;
                    if (occurrence.Ended)
                    {
                        endedCount++;
                        if (occurrence.NoteOff != null && (latestOff == null ||
                            IsLaterSourceEvent(occurrence.NoteOff, latestOff))) latestOff = occurrence.NoteOff;
                    }
                    else liveSupports++;
                    SetOccurrence(node, occurrence);
                }
                if (occurrence.Ended) FreeOccurrence(node);
                node = next;
            }

            _filteredEvents += Math.Max(0, selectedMembers - 1);
            _selectedSupportCount[pitch] += liveSupports;
            if (_selectedSupportCount[pitch] == 0)
            {
                if (endedCount > 0) _filteredEvents += endedCount - 1;
                _naturalRelease[pitch] = latestOff;
            }
            else _filteredEvents += endedCount;

            _pendingAttack[pitch] = representativeEvent;
            _attackBoundary[pitch] = assigned;
            _nextAttackBoundary[pitch] = AddInterval(AddInterval(assigned));
            if (_outputDown[pitch]) _preparatoryReleaseBoundary[pitch] = assigned - _intervalMicroseconds;
            if (_selectedSupportCount[pitch] == 0)
                ScheduleNaturalRelease(pitch, latestOff, AddInterval(assigned));
            ClearCandidate(pitch);
        }

        private void RejectGroup(int head)
        {
            int node = head;
            while (node >= 0)
            {
                SourceOccurrence occurrence = GetOccurrence(node);
                int next = occurrence.NextGroup;
                occurrence.NextGroup = -1;
                if (!occurrence.RemovedByRouting)
                {
                    occurrence.Disposition = 2;
                    _filteredEvents++;
                    if (occurrence.Ended) _filteredEvents++;
                    SetOccurrence(node, occurrence);
                }
                if (occurrence.Ended) FreeOccurrence(node);
                node = next;
            }
        }

        private void DiscardRemovedGroup(int head)
        {
            int node = head;
            while (node >= 0)
            {
                SourceOccurrence occurrence = GetOccurrence(node);
                int next = occurrence.NextGroup;
                occurrence.NextGroup = -1;
                SetOccurrence(node, occurrence);
                if (occurrence.Ended) FreeOccurrence(node);
                node = next;
            }
        }

        private void ClearCandidate(int pitch)
        {
            _candidateHead[pitch] = _candidateRepresentative[pitch] = -1;
            _candidateMembers[pitch] = 0;
            _candidateBoundary[pitch] = 0;
        }

        private void ResolveDelayedCandidate(int pitch, long boundaryMicroseconds)
        {
            if (_delayedHead[pitch] < 0 || _delayedBoundary[pitch] > boundaryMicroseconds) return;
            if (_candidateHead[pitch] >= 0 && _candidateBoundary[pitch] <= boundaryMicroseconds)
            {
                if (IsBetterRepresentative(_candidateRepresentative[pitch], _delayedRepresentative[pitch]))
                {
                    RejectGroup(_delayedHead[pitch]);
                    ClearDelayedCandidate(pitch);
                    return;
                }
                RejectGroup(_candidateHead[pitch]);
                ClearCandidate(pitch);
            }
            _candidateHead[pitch] = _delayedHead[pitch];
            _candidateRepresentative[pitch] = _delayedRepresentative[pitch];
            _candidateMembers[pitch] = _delayedMembers[pitch];
            _candidateBoundary[pitch] = boundaryMicroseconds;
            ClearDelayedCandidate(pitch);
        }

        private void ClearDelayedCandidate(int pitch)
        {
            _delayedHead[pitch] = _delayedRepresentative[pitch] = -1;
            _delayedMembers[pitch] = 0;
            _delayedBoundary[pitch] = Int64.MaxValue;
        }

        private int FindBestGroupMember(int head)
        {
            int best = -1;
            for (int node = head; node >= 0; node = GetOccurrence(node).NextGroup)
                if (!GetOccurrence(node).RemovedByRouting &&
                    (best < 0 || IsBetterRepresentative(node, best))) best = node;
            return best;
        }

        private bool IsBetterRepresentative(int candidate, int current)
        {
            MidiEvent left = GetOccurrence(candidate).NoteOn;
            MidiEvent right = GetOccurrence(current).NoteOn;
            int leftVelocity = left.GetDataByte(2) & 0x7F;
            int rightVelocity = right.GetDataByte(2) & 0x7F;
            if (leftVelocity != rightVelocity) return leftVelocity > rightVelocity;
            if (left.EventIndex != right.EventIndex) return left.EventIndex < right.EventIndex;
            return left.Order < right.Order;
        }

        private static bool IsLaterSourceEvent(MidiEvent candidate, MidiEvent current)
        {
            if (candidate.IntendedMicroseconds != current.IntendedMicroseconds)
                return candidate.IntendedMicroseconds > current.IntendedMicroseconds;
            if (candidate.AbsoluteTick != current.AbsoluteTick)
                return candidate.AbsoluteTick > current.AbsoluteTick;
            return candidate.Order > current.Order;
        }

        private void ScheduleNaturalRelease(int pitch, MidiEvent source, long requestedMicroseconds)
        {
            long boundary = BoundaryAtOrAfter(Math.Max(0, requestedMicroseconds));
            if (_minimumReleaseBoundary[pitch] != 0)
                boundary = Math.Max(boundary, _minimumReleaseBoundary[pitch]);
            if (_pendingAttack[pitch] != null)
                boundary = Math.Max(boundary, AddInterval(_attackBoundary[pitch]));
            _naturalRelease[pitch] = source;
            _naturalReleaseBoundary[pitch] = boundary;
        }

        private void FreeOccurrence(int node)
        {
            if (node < 0 || node >= _nextUnusedOccurrence || !GetOccurrence(node).InUse) return;
            SourceOccurrence cleared = new SourceOccurrence();
            cleared.NextFree = _freeOccurrence;
            cleared.NextActive = -1;
            cleared.NextGroup = -1;
            SetOccurrence(node, cleared);
            _freeOccurrence = node;
        }

        private long BoundaryAtOrAfter(long microseconds)
        {
            if (microseconds <= 0) return 0;
            long quotient = microseconds / _intervalMicroseconds;
            long remainder = microseconds % _intervalMicroseconds;
            if (remainder == 0) return quotient * _intervalMicroseconds;
            if (quotient >= Int64.MaxValue / _intervalMicroseconds) return Int64.MaxValue;
            return (quotient + 1) * _intervalMicroseconds;
        }

        private long AddInterval(long boundary)
        {
            return boundary > Int64.MaxValue - _intervalMicroseconds ? Int64.MaxValue : boundary + _intervalMicroseconds;
        }

        private void RecalculateNextBoundary()
        {
            long next = Int64.MaxValue;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                if (_candidateHead[pitch] >= 0 && _candidateBoundary[pitch] < next) next = _candidateBoundary[pitch];
                if (_delayedBoundary[pitch] < next) next = _delayedBoundary[pitch];
                if (_preparatoryReleaseBoundary[pitch] < next) next = _preparatoryReleaseBoundary[pitch];
                if (_attackBoundary[pitch] < next) next = _attackBoundary[pitch];
                if ((_outputDown[pitch] || _pendingAttack[pitch] != null) &&
                    _naturalReleaseBoundary[pitch] < next) next = _naturalReleaseBoundary[pitch];
            }
            _nextBoundary = next;
        }

        private static int Key(int channel, int pitch) { return channel * PitchCount + pitch; }

        private static MidiEvent CreateNoteOff(MidiEvent source, int pitch, int channel, int velocity,
            long boundaryMicroseconds)
        {
            if (channel < 0 || channel >= ChannelCount) channel = 0;
            return new MidiEvent
            {
                AbsoluteTick = source == null ? 0 : source.AbsoluteTick,
                IntendedMicroseconds = source == null ? boundaryMicroseconds : source.IntendedMicroseconds,
                Track = source == null ? -1 : source.Track,
                Order = source == null ? Int32.MaxValue : source.Order,
                EventIndex = source == null ? -1 : source.EventIndex,
                Kind = MidiEventKind.NoteOff,
                Channel = channel,
                Status = (byte)(0x80 | channel),
                Data = MidiEventData.FromShort((byte)(0x80 | channel), (byte)pitch, (byte)velocity, 3)
            };
        }

        internal static bool TryClassify(MidiEvent midiEvent, out int pitch, out bool noteOn)
        {
            pitch = -1;
            noteOn = false;
            if (midiEvent == null || midiEvent.Channel < 0 || midiEvent.Channel >= ChannelCount || midiEvent.DataLength < 3)
                return false;
            int command = midiEvent.Status & 0xF0;
            if (command != 0x80 && command != 0x90) return false;
            pitch = midiEvent.GetDataByte(1) & 0x7F;
            noteOn = command == 0x90 && midiEvent.GetDataByte(2) != 0;
            return true;
        }
    }
}
