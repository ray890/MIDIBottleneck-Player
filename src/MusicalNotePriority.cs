using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal struct MusicalNotePriorityCandidate
    {
        internal int Pitch;
        internal int Velocity;
        internal int Track;
        internal int Channel;
        internal int SourceOrder;
        internal long IntendedMicroseconds;
        internal long DurationMicroseconds;
    }

    // Reusable deterministic ranking shared by queue overflow, interval
    // admission, and the Note start rate gate. Dictionaries retain their
    // backing storage across calls, so steady-state overflow does not allocate
    // a candidate graph or perform a quadratic chord-boundary scan.
    internal sealed class MusicalNotePrioritySelector
    {
        private struct Annotation
        {
            internal int LaneRepresentation;
            internal int Identity;
            internal bool ChordBoundary;
        }

        private struct PitchRange
        {
            internal int Minimum;
            internal int Maximum;
        }

        private readonly Dictionary<long, int> _laneCounts = new Dictionary<long, int>();
        private readonly Dictionary<long, PitchRange> _tonalChordRanges =
            new Dictionary<long, PitchRange>();
        private Annotation[] _annotations = new Annotation[16];

        internal int SelectLeastImportant(MusicalNotePriorityCandidate[] candidates,
            int count, int fairnessCursor)
        {
            if (candidates == null) throw new ArgumentNullException("candidates");
            if (count <= 0 || count > candidates.Length)
                throw new ArgumentOutOfRangeException("count");
            EnsureCapacity(count);
            _laneCounts.Clear();
            _tonalChordRanges.Clear();

            for (int index = 0; index < count; index++)
            {
                MusicalNotePriorityCandidate candidate = candidates[index];
                long lane = Lane(candidate.Track, candidate.Channel);
                int laneCount;
                _laneCounts.TryGetValue(lane, out laneCount);
                _laneCounts[lane] = laneCount + 1;

                // General MIDI channel 10 (zero-based channel 9) is a
                // percussion lane. Drum key numbers are instruments rather
                // than bass/melody pitch extremes, so exclude them from the
                // tonal chord range.
                if (candidate.Channel == 9) continue;
                PitchRange range;
                if (!_tonalChordRanges.TryGetValue(candidate.IntendedMicroseconds, out range))
                    range.Minimum = range.Maximum = candidate.Pitch;
                else
                {
                    range.Minimum = Math.Min(range.Minimum, candidate.Pitch);
                    range.Maximum = Math.Max(range.Maximum, candidate.Pitch);
                }
                _tonalChordRanges[candidate.IntendedMicroseconds] = range;
            }

            for (int index = 0; index < count; index++)
            {
                MusicalNotePriorityCandidate candidate = candidates[index];
                long lane = Lane(candidate.Track, candidate.Channel);
                PitchRange range;
                bool boundary = candidate.Channel != 9 &&
                    _tonalChordRanges.TryGetValue(candidate.IntendedMicroseconds, out range) &&
                    (candidate.Pitch == range.Minimum || candidate.Pitch == range.Maximum);
                // Write the complete value-type annotation back to the
                // retained array. Omitting this assignment silently erases
                // chord and lane information.
                _annotations[index] = new Annotation
                {
                    LaneRepresentation = _laneCounts[lane],
                    Identity = Identity(candidate.Track, candidate.Channel),
                    ChordBoundary = boundary
                };
            }

            int victim = 0;
            for (int index = 1; index < count; index++)
                if (IsLessImportant(candidates[index], _annotations[index],
                    candidates[victim], _annotations[victim], fairnessCursor)) victim = index;
            return victim;
        }

        internal int NextFairnessCursor(MusicalNotePriorityCandidate candidate)
        {
            int identity = Identity(candidate.Track, candidate.Channel);
            return identity == Int32.MaxValue ? 0 : identity + 1;
        }

        private void EnsureCapacity(int count)
        {
            if (_annotations.Length >= count) return;
            int capacity = _annotations.Length;
            while (capacity < count && capacity <= Int32.MaxValue / 2) capacity *= 2;
            if (capacity < count) capacity = count;
            _annotations = new Annotation[capacity];
        }

        private static bool IsLessImportant(MusicalNotePriorityCandidate candidate,
            Annotation candidateAnnotation, MusicalNotePriorityCandidate current,
            Annotation currentAnnotation, int fairnessCursor)
        {
            if (candidateAnnotation.ChordBoundary != currentAnnotation.ChordBoundary)
                return !candidateAnnotation.ChordBoundary;
            if (candidate.Velocity != current.Velocity)
                return candidate.Velocity < current.Velocity;
            if (candidate.DurationMicroseconds != current.DurationMicroseconds)
                return candidate.DurationMicroseconds < current.DurationMicroseconds;
            if (candidateAnnotation.LaneRepresentation != currentAnnotation.LaneRepresentation)
                return candidateAnnotation.LaneRepresentation > currentAnnotation.LaneRepresentation;
            long candidateDistance = FairnessDistance(candidateAnnotation.Identity, fairnessCursor);
            long currentDistance = FairnessDistance(currentAnnotation.Identity, fairnessCursor);
            if (candidateDistance != currentDistance)
                return candidateDistance < currentDistance;
            return candidate.SourceOrder > current.SourceOrder;
        }

        private static long Lane(int track, int channel)
        {
            return ((long)track << 8) ^ (uint)(channel + 1);
        }

        private static int Identity(int track, int channel)
        {
            return unchecked(track * 397 ^ (channel < 0 ? 0 : channel)) & Int32.MaxValue;
        }

        private static long FairnessDistance(int identity, int cursor)
        {
            const long range = (long)Int32.MaxValue + 1L;
            long distance = identity - (long)cursor;
            return distance < 0 ? distance + range : distance;
        }
    }
}
