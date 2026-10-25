using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum TrackRoutingTier
    {
        ExactSafe,
        GmFamily,
        Aggressive
    }

    internal enum PercussionRoutingPolicy
    {
        ReserveChannel10,
        AllowAnyDestination
    }

    internal struct TrackLaneKey : IEquatable<TrackLaneKey>, IComparable<TrackLaneKey>
    {
        internal readonly int Track;
        internal readonly int SourceChannel;
        internal TrackLaneKey(int track, int sourceChannel)
        { Track = track; SourceChannel = sourceChannel; }
        public bool Equals(TrackLaneKey other)
        { return Track == other.Track && SourceChannel == other.SourceChannel; }
        public override bool Equals(object value)
        { return value is TrackLaneKey && Equals((TrackLaneKey)value); }
        public override int GetHashCode() { return unchecked(Track * 397) ^ SourceChannel; }
        public int CompareTo(TrackLaneKey other)
        {
            int result = Track.CompareTo(other.Track);
            return result != 0 ? result : SourceChannel.CompareTo(other.SourceChannel);
        }
        public override string ToString() { return "track " + (Track + 1) + ", ch " + (SourceChannel + 1); }
    }

    internal struct MidiPatchIdentity : IEquatable<MidiPatchIdentity>
    {
        internal readonly int BankMsb;
        internal readonly int BankLsb;
        internal readonly int Program;
        internal MidiPatchIdentity(int bankMsb, int bankLsb, int program)
        { BankMsb = bankMsb; BankLsb = bankLsb; Program = program; }
        internal int GmFamily { get { return Program < 0 ? -1 : Program / 8; } }
        internal bool IsGeneralMidiBank
        { get { return BankMsb == 0 && BankLsb == 0; } }
        public bool Equals(MidiPatchIdentity other)
        { return BankMsb == other.BankMsb && BankLsb == other.BankLsb && Program == other.Program; }
        public override bool Equals(object value)
        { return value is MidiPatchIdentity && Equals((MidiPatchIdentity)value); }
        public override int GetHashCode()
        { return unchecked(((BankMsb * 397) ^ BankLsb) * 397) ^ Program; }
        public override string ToString()
        {
            return "bank " + (BankMsb < 0 ? "?" : BankMsb.ToString()) + "/" +
                (BankLsb < 0 ? "?" : BankLsb.ToString()) + ", program " +
                (Program < 0 ? "?" : (Program + 1).ToString());
        }
    }

    internal sealed class TrackRouteLaneProfile
    {
        internal TrackLaneKey Lane;
        internal string TrackName;
        internal string InstrumentName;
        internal long EventCount;
        internal long AttackCount;
        internal long FirstAttackMicroseconds = -1;
        internal long LastReleaseMicroseconds = -1;
        internal bool HasDynamicState;
        internal bool HasSustain;
        internal bool TrackContainsSystemTraffic;
        internal ulong DynamicStateSignature;
        internal MidiPatchIdentity RepresentativePatch;
        internal ulong DominanceScore;
        internal HashSet<MidiPatchIdentity> PatchHistory = new HashSet<MidiPatchIdentity>();
        internal readonly List<TrackRoutePatchChange> PatchTimeline =
            new List<TrackRoutePatchChange>();
        internal readonly List<TrackRouteSoundInterval> SoundingIntervals =
            new List<TrackRouteSoundInterval>();

        internal string DisplayName
        {
            get
            {
                string name = !String.IsNullOrEmpty(TrackName) ? TrackName : InstrumentName;
                return String.IsNullOrEmpty(name) ? "Track " + (Lane.Track + 1) : name;
            }
        }
    }

    internal struct TrackRoutePatchChange
    {
        internal int EventIndex;
        internal MidiPatchIdentity Patch;
    }

    internal struct TrackRouteSoundInterval
    {
        internal int StartEventIndex;
        internal int EndEventIndex;
        internal long StartMicroseconds;
        internal long EndMicroseconds;
        internal long PrefixMaximumEndMicroseconds;
        internal int Key;
    }

    internal sealed class TrackRoutingSettings
    {
        internal readonly bool[] UsableDestinationChannels = new bool[16];
        internal int TargetCapacity = 16;
        internal TrackRoutingTier MaximumAutomaticTier = TrackRoutingTier.ExactSafe;
        internal PercussionRoutingPolicy PercussionPolicy = PercussionRoutingPolicy.ReserveChannel10;
        internal readonly Dictionary<TrackLaneKey, int> ManualDestinations =
            new Dictionary<TrackLaneKey, int>();

        internal TrackRoutingSettings()
        {
            for (int channel = 0; channel < 16; channel++) UsableDestinationChannels[channel] = true;
        }

        internal TrackRoutingSettings Clone()
        {
            TrackRoutingSettings clone = new TrackRoutingSettings
            {
                TargetCapacity = TargetCapacity,
                MaximumAutomaticTier = MaximumAutomaticTier,
                PercussionPolicy = PercussionPolicy
            };
            for (int channel = 0; channel < 16; channel++)
                clone.UsableDestinationChannels[channel] = UsableDestinationChannels[channel];
            foreach (KeyValuePair<TrackLaneKey, int> pair in ManualDestinations)
                clone.ManualDestinations[pair.Key] = pair.Value;
            return clone;
        }
    }

    internal sealed class TrackRouteAssignment
    {
        internal TrackRouteLaneProfile Profile;
        internal int DestinationChannel;
        internal TrackRoutingTier Tier;
        internal bool Manual;
        internal bool RepresentativePatch;
        internal bool StateConflict;
        internal bool SameKeyOverlap;
        internal bool TimeHandoff;
        internal bool PercussionException;
        internal bool Retimbred;
        internal bool Omitted;
        internal readonly List<string> Warnings = new List<string>();
    }

    internal sealed class TrackRouteDestinationPreview
    {
        internal int DestinationChannel;
        internal MidiPatchIdentity RepresentativePatch;
        internal TrackLaneKey RepresentativeLane;
        internal readonly List<TrackRouteAssignment> Assignments = new List<TrackRouteAssignment>();
    }

    internal sealed class TrackRouteStatistics
    {
        internal readonly int RoutedLaneCount;
        internal readonly int DestinationCount;
        internal readonly int MergedLaneCount;
        internal readonly int RetimbredLaneCount;
        internal readonly int OmittedLaneCount;
        internal readonly long OmittedNoteStartCount;
        internal readonly int GeneratedDestinationStateCount;
        internal readonly int IncludedSourceEventCount;
        internal readonly int RoutedOutputEventCount;

        internal TrackRouteStatistics(int routedLaneCount, int destinationCount,
            int mergedLaneCount, int retimbredLaneCount, int omittedLaneCount,
            long omittedNoteStartCount, int generatedDestinationStateCount,
            int includedSourceEventCount, int routedOutputEventCount)
        {
            RoutedLaneCount = routedLaneCount;
            DestinationCount = destinationCount;
            MergedLaneCount = mergedLaneCount;
            RetimbredLaneCount = retimbredLaneCount;
            OmittedLaneCount = omittedLaneCount;
            OmittedNoteStartCount = omittedNoteStartCount;
            GeneratedDestinationStateCount = generatedDestinationStateCount;
            IncludedSourceEventCount = includedSourceEventCount;
            RoutedOutputEventCount = routedOutputEventCount;
        }
    }

    internal sealed class CompiledTrackRoute
    {
        private readonly IMidiEventStore _sourceStore;
        private readonly int[] _destinations;
        private readonly sbyte[] _eventDestinations;
        private readonly Dictionary<int, List<MidiEvent>> _generatedPatchEventsBefore;
        internal readonly IList<TrackRouteLaneProfile> Lanes;
        internal readonly IList<TrackRouteAssignment> Assignments;
        internal readonly IList<TrackRouteDestinationPreview> Destinations;
        internal readonly IList<string> Errors;
        internal readonly bool RequiresExplicitApply;
        internal readonly TrackRoutingTier HighestTier;
        internal readonly TrackRouteStatistics Statistics;

        internal CompiledTrackRoute(IMidiEventStore sourceStore, int trackCount,
            List<TrackRouteLaneProfile> lanes,
            List<TrackRouteAssignment> assignments,
            List<TrackRouteDestinationPreview> destinations, List<string> errors,
            bool requiresExplicitApply, TrackRoutingTier highestTier)
        {
            if (sourceStore == null) throw new ArgumentNullException("sourceStore");
            _sourceStore = sourceStore;
            _destinations = new int[Math.Max(0, trackCount) * 16];
            for (int index = 0; index < _destinations.Length; index++) _destinations[index] = -1;
            for (int index = 0; index < assignments.Count; index++)
            {
                TrackRouteAssignment assignment = assignments[index];
                int slot = assignment.Profile.Lane.Track * 16 + assignment.Profile.Lane.SourceChannel;
                if (slot >= 0 && slot < _destinations.Length) _destinations[slot] = assignment.DestinationChannel;
            }
            int[] canonicalDestination = new int[16];
            for (int channel = 0; channel < canonicalDestination.Length; channel++)
                canonicalDestination[channel] = -1;
            for (int index = 0; index < assignments.Count; index++)
            {
                TrackRouteAssignment assignment = assignments[index];
                if (assignment.Omitted || assignment.DestinationChannel < 0) continue;
                int sourceChannel = assignment.Profile.Lane.SourceChannel;
                if (canonicalDestination[sourceChannel] < 0)
                    canonicalDestination[sourceChannel] = assignment.DestinationChannel;
            }
            _eventDestinations = new sbyte[sourceStore.Count];
            for (int eventIndex = 0; eventIndex < sourceStore.Count; eventIndex++)
            {
                MidiEventView source = sourceStore.GetEvent(eventIndex);
                if (source.Channel < 0)
                {
                    _eventDestinations[eventIndex] = -1;
                    continue;
                }
                int stateKind;
                int stateValue;
                if (TryClassifyPatchState(source, out stateKind, out stateValue))
                {
                    // Destination patch state is compiled separately below.  No source
                    // Bank/Program event is allowed to race with that canonical timeline.
                    _eventDestinations[eventIndex] = -2;
                    continue;
                }
                int slot = source.Track * 16 + source.Channel;
                int assigned = slot >= 0 && slot < _destinations.Length
                    ? _destinations[slot] : -1;
                if (assigned < 0 && IsChannelState(source))
                    assigned = canonicalDestination[source.Channel];
                _eventDestinations[eventIndex] = assigned < 0 ? (sbyte)-2 : (sbyte)assigned;
            }
            _generatedPatchEventsBefore = BuildCanonicalPatchEvents(
                sourceStore, destinations, assignments);
            Statistics = BuildStatistics(sourceStore, destinations, assignments);
            Lanes = lanes.AsReadOnly();
            Assignments = assignments.AsReadOnly();
            Destinations = destinations.AsReadOnly();
            Errors = errors.AsReadOnly();
            RequiresExplicitApply = requiresExplicitApply;
            HighestTier = highestTier;
        }

        internal bool IsValid { get { return Errors.Count == 0; } }

        internal bool IsForSource(MidiSong source)
        {
            return source != null && IsForSource(source.EventStore);
        }

        internal bool IsForSource(IMidiEventStore sourceStore)
        {
            return Object.ReferenceEquals(sourceStore, _sourceStore);
        }

        internal bool TryTransform(MidiEventView source, out MidiEventView routed)
        {
            if (source.Channel < 0)
            {
                routed = source;
                return true;
            }
            int eventIndex = source.SourceEventIndex;
            if (eventIndex < 0 || eventIndex >= _eventDestinations.Length ||
                _eventDestinations[eventIndex] < 0)
            {
                routed = default(MidiEventView);
                return false;
            }
            routed = source.WithOutputChannel(_eventDestinations[eventIndex]);
            return true;
        }

        internal bool Includes(MidiEventView source)
        {
            return source.Channel < 0 || source.SourceEventIndex >= 0 &&
                source.SourceEventIndex < _eventDestinations.Length &&
                _eventDestinations[source.SourceEventIndex] >= -1;
        }

        internal IList<MidiEvent> GeneratedPatchEventsBefore(int sourceEventIndex)
        {
            List<MidiEvent> generated;
            return _generatedPatchEventsBefore.TryGetValue(sourceEventIndex, out generated)
                ? (IList<MidiEvent>)generated : null;
        }

        private TrackRouteStatistics BuildStatistics(IMidiEventStore sourceStore,
            IList<TrackRouteDestinationPreview> destinations,
            IList<TrackRouteAssignment> assignments)
        {
            int routedLanes = 0;
            int mergedLanes = 0;
            int retimbredLanes = 0;
            int omittedLanes = 0;
            long omittedNoteStarts = 0;
            for (int index = 0; index < assignments.Count; index++)
            {
                TrackRouteAssignment assignment = assignments[index];
                if (assignment.Omitted)
                {
                    omittedLanes++;
                    omittedNoteStarts = Int64.MaxValue - omittedNoteStarts <
                        assignment.Profile.AttackCount
                        ? Int64.MaxValue : omittedNoteStarts + assignment.Profile.AttackCount;
                    continue;
                }
                if (assignment.DestinationChannel >= 0) routedLanes++;
                if (assignment.Retimbred) retimbredLanes++;
            }
            for (int index = 0; index < destinations.Count; index++)
                if (destinations[index].Assignments.Count > 1)
                    mergedLanes += destinations[index].Assignments.Count - 1;
            int generated = 0;
            foreach (KeyValuePair<int, List<MidiEvent>> pair in _generatedPatchEventsBefore)
            {
                if (Int32.MaxValue - generated < pair.Value.Count)
                    throw new InvalidOperationException(
                        "Track routing would exceed the supported indexed routed-view limit.");
                generated += pair.Value.Count;
            }
            int included = 0;
            for (int index = 0; index < sourceStore.Count; index++)
                if (Includes(sourceStore.GetEvent(index))) included++;
            if (generated > Int32.MaxValue - included)
                throw new InvalidOperationException(
                    "Track routing would exceed the supported indexed routed-view limit.");
            int output = included + generated;
            return new TrackRouteStatistics(routedLanes, destinations.Count,
                mergedLanes, retimbredLanes, omittedLanes, omittedNoteStarts,
                generated, included, output);
        }

        private static bool TryClassifyPatchState(MidiEventView source,
            out int stateKind, out int stateValue)
        {
            int command = source.Status & 0xF0;
            if (command == 0xC0 && source.DataLength >= 2)
            {
                stateKind = 2;
                stateValue = source.GetDataByte(1);
                return true;
            }
            if (command == 0xB0 && source.DataLength >= 3 &&
                (source.GetDataByte(1) == 0 || source.GetDataByte(1) == 32))
            {
                stateKind = source.GetDataByte(1) == 0 ? 0 : 1;
                stateValue = source.GetDataByte(2);
                return true;
            }
            stateKind = -1;
            stateValue = -1;
            return false;
        }

        private static bool IsChannelState(MidiEventView source)
        {
            int command = source.Status & 0xF0;
            return command != 0x80 && command != 0x90;
        }

        private Dictionary<int, List<MidiEvent>> BuildCanonicalPatchEvents(
            IMidiEventStore sourceStore,
            IList<TrackRouteDestinationPreview> destinations,
            IList<TrackRouteAssignment> assignments)
        {
            Dictionary<int, List<MidiEvent>> result =
                new Dictionary<int, List<MidiEvent>>();
            TrackRouteDestinationPreview[] byDestination =
                new TrackRouteDestinationPreview[16];
            for (int index = 0; index < destinations.Count; index++)
                byDestination[destinations[index].DestinationChannel] = destinations[index];
            Dictionary<TrackLaneKey, TrackRouteLaneProfile> byLane =
                new Dictionary<TrackLaneKey, TrackRouteLaneProfile>();
            for (int index = 0; index < assignments.Count; index++)
                if (!assignments[index].Omitted)
                    byLane[assignments[index].Profile.Lane] = assignments[index].Profile;

            MidiPatchIdentity[] current = new MidiPatchIdentity[16];
            int[] sourceDestinations = new int[16];
            for (int channel = 0; channel < current.Length; channel++)
            {
                current[channel] = new MidiPatchIdentity(0, 0, 0);
                sourceDestinations[channel] = -1;
            }
            for (int index = 0; index < assignments.Count; index++)
                if (!assignments[index].Omitted && assignments[index].DestinationChannel >= 0)
                    sourceDestinations[assignments[index].Profile.Lane.SourceChannel] =
                        assignments[index].DestinationChannel;
            for (int eventIndex = 0; eventIndex < sourceStore.Count; eventIndex++)
            {
                MidiEventView source = sourceStore.GetEvent(eventIndex);
                if (source.Channel < 0) continue;
                bool attack = (source.Status & 0xF0) == 0x90 &&
                    source.DataLength >= 3 && source.GetDataByte(2) != 0 &&
                    _eventDestinations[eventIndex] >= 0;
                int stateKind, stateValue;
                bool patchState = TryClassifyPatchState(source, out stateKind, out stateValue);
                if (!attack && !patchState) continue;
                int destinationChannel = attack
                    ? _eventDestinations[eventIndex] : sourceDestinations[source.Channel];
                if (destinationChannel < 0) continue;
                TrackRouteDestinationPreview destination = byDestination[destinationChannel];
                if (destination == null) continue;
                TrackRouteLaneProfile sourceProfile = null;
                byLane.TryGetValue(new TrackLaneKey(source.Track, source.Channel), out sourceProfile);
                if (sourceProfile == null)
                    sourceProfile = BestProfileForSourceChannel(
                        destination, source.Channel);
                TrackRouteLaneProfile owner = CanonicalProfileAtTime(
                    destination, source.IntendedMicroseconds, sourceProfile);
                if (owner == null || patchState &&
                    (!IsSoundingAt(owner, source.IntendedMicroseconds) ||
                    owner.Lane.SourceChannel != source.Channel)) continue;
                MidiPatchIdentity desired = NormalizePatch(PatchAt(owner, eventIndex));
                List<MidiEvent> generated = null;
                bool bankChanged = current[destinationChannel].BankMsb != desired.BankMsb ||
                    current[destinationChannel].BankLsb != desired.BankLsb;
                if (current[destinationChannel].BankMsb != desired.BankMsb)
                    AddGeneratedPatchEvent(ref generated, source, destinationChannel,
                        MidiEventKind.ControlChange, 0, desired.BankMsb);
                if (current[destinationChannel].BankLsb != desired.BankLsb)
                    AddGeneratedPatchEvent(ref generated, source, destinationChannel,
                        MidiEventKind.ControlChange, 32, desired.BankLsb);
                if (bankChanged || current[destinationChannel].Program != desired.Program)
                    AddGeneratedPatchEvent(ref generated, source, destinationChannel,
                        MidiEventKind.ProgramChange, -1, desired.Program);
                if (generated != null) result[eventIndex] = generated;
                current[destinationChannel] = desired;
            }
            return result;
        }

        private static TrackRouteLaneProfile BestProfileForSourceChannel(
            TrackRouteDestinationPreview destination, int sourceChannel)
        {
            TrackRouteLaneProfile result = null;
            for (int index = 0; index < destination.Assignments.Count; index++)
            {
                TrackRouteLaneProfile profile = destination.Assignments[index].Profile;
                if (profile.Lane.SourceChannel != sourceChannel) continue;
                result = PreferCanonicalProfile(
                    result, profile, destination.RepresentativeLane);
            }
            return result;
        }

        private static void AddGeneratedPatchEvent(ref List<MidiEvent> generated,
            MidiEventView source, int destinationChannel, MidiEventKind kind,
            int controller, int value)
        {
            if (generated == null) generated = new List<MidiEvent>(3);
            byte status = (byte)((kind == MidiEventKind.ProgramChange ? 0xC0 : 0xB0) |
                destinationChannel);
            generated.Add(new MidiEvent
            {
                AbsoluteTick = source.AbsoluteTick,
                IntendedMicroseconds = source.IntendedMicroseconds,
                Track = source.Track,
                Kind = kind,
                Channel = destinationChannel,
                Status = status,
                EventIndex = -1,
                Data = kind == MidiEventKind.ProgramChange
                    ? MidiEventData.FromShort(status, (byte)value, 0, 2)
                    : MidiEventData.FromShort(status, (byte)controller, (byte)value, 3)
            });
        }

        private static MidiPatchIdentity NormalizePatch(MidiPatchIdentity patch)
        {
            return new MidiPatchIdentity(
                patch.BankMsb < 0 ? 0 : patch.BankMsb,
                patch.BankLsb < 0 ? 0 : patch.BankLsb,
                patch.Program < 0 ? 0 : patch.Program);
        }

        private static MidiPatchIdentity PatchAt(TrackRouteLaneProfile profile, int eventIndex)
        {
            int low = 0;
            int high = profile.PatchTimeline.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (profile.PatchTimeline[middle].EventIndex <= eventIndex) low = middle + 1;
                else high = middle;
            }
            return low == 0 ? new MidiPatchIdentity(-1, -1, -1) :
                profile.PatchTimeline[low - 1].Patch;
        }

        private static TrackRouteLaneProfile CanonicalProfileAtTime(
            TrackRouteDestinationPreview destination, long microseconds,
            TrackRouteLaneProfile fallback)
        {
            TrackRouteLaneProfile active = null;
            int activeSourceChannel = -1;
            bool multipleSourceChannels = false;
            TrackRouteLaneProfile representative = null;
            for (int index = 0; index < destination.Assignments.Count; index++)
            {
                TrackRouteLaneProfile profile = destination.Assignments[index].Profile;
                if (profile.Lane.Equals(destination.RepresentativeLane)) representative = profile;
                if (!IsSoundingAt(profile, microseconds)) continue;
                active = PreferCanonicalProfile(active, profile, destination.RepresentativeLane);
                if (activeSourceChannel < 0) activeSourceChannel = profile.Lane.SourceChannel;
                else if (activeSourceChannel != profile.Lane.SourceChannel)
                    multipleSourceChannels = true;
            }
            if (multipleSourceChannels && representative != null) return representative;
            return active ?? fallback;
        }

        private static bool IsSoundingAt(TrackRouteLaneProfile profile, long microseconds)
        {
            int low = 0;
            int high = profile.SoundingIntervals.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (profile.SoundingIntervals[middle].StartMicroseconds <= microseconds)
                    low = middle + 1;
                else high = middle;
            }
            for (int index = low - 1; index >= 0; index--)
            {
                TrackRouteSoundInterval interval = profile.SoundingIntervals[index];
                if (interval.PrefixMaximumEndMicroseconds <= microseconds) return false;
                if (microseconds < interval.EndMicroseconds) return true;
            }
            return false;
        }

        private static TrackRouteLaneProfile PreferCanonicalProfile(
            TrackRouteLaneProfile left, TrackRouteLaneProfile right,
            TrackLaneKey representative)
        {
            if (left == null) return right;
            if (right == null) return left;
            if (right.Lane.Equals(representative)) return right;
            if (left.Lane.Equals(representative)) return left;
            if (right.DominanceScore > left.DominanceScore) return right;
            if (right.DominanceScore < left.DominanceScore) return left;
            return right.Lane.CompareTo(left.Lane) < 0 ? right : left;
        }
    }

    // Immutable event-store view shared by Playback and Analysis.  Routing is
    // applied when an event is read, so source timing/order/identity remain
    // unchanged and both consumers observe exactly the same channel payload.
    internal sealed class TrackRoutedEventStore : IMidiEventStore
    {
        private struct RoutedEntry
        {
            internal int SourceIndex;
            internal MidiEvent Generated;
        }

        private readonly IMidiEventStore _source;
        private readonly CompiledTrackRoute _route;
        private readonly RoutedEntry[] _entries;

        internal TrackRoutedEventStore(IMidiEventStore source, CompiledTrackRoute route)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (route == null) throw new ArgumentNullException("route");
            if (!route.IsForSource(source))
                throw new InvalidOperationException(
                    "The compiled track route belongs to a different source song.");
            if (!route.IsValid)
                throw new InvalidOperationException("A track route with unresolved lanes cannot be applied.");
            _source = source;
            _route = route;
            List<RoutedEntry> included = new List<RoutedEntry>(
                route.Statistics.RoutedOutputEventCount);
            for (int index = 0; index < source.Count; index++)
            {
                IList<MidiEvent> generated = route.GeneratedPatchEventsBefore(index);
                if (generated != null)
                    for (int generatedIndex = 0; generatedIndex < generated.Count; generatedIndex++)
                        included.Add(new RoutedEntry
                        {
                            SourceIndex = -1,
                            Generated = generated[generatedIndex]
                        });
                if (route.Includes(source.GetEvent(index)))
                    included.Add(new RoutedEntry { SourceIndex = index });
            }
            _entries = included.ToArray();
            if (_entries.Length != route.Statistics.RoutedOutputEventCount)
                throw new InvalidOperationException(
                    "Track-routing statistics do not match the compiled routed event view.");
        }

        public int Count { get { return _entries.Length; } }

        public MidiEventView GetEvent(int index)
        {
            RoutedEntry entry = _entries[index];
            if (entry.SourceIndex < 0)
                return MidiEventView.FromEvent(entry.Generated, -1).WithLogicalEventIndex(index);
            MidiEventView source = _source.GetEvent(entry.SourceIndex);
            MidiEventView routed;
            if (!_route.TryTransform(source, out routed))
                throw new InvalidOperationException("The compiled track route does not assign " +
                    new TrackLaneKey(source.Track, source.Channel) + ".");
            return routed.WithLogicalEventIndex(index);
        }

        public int LowerBoundByTime(long microseconds)
        {
            int low = 0;
            int high = _entries.Length;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (GetEvent(middle).IntendedMicroseconds < microseconds)
                    low = middle + 1;
                else high = middle;
            }
            return low;
        }
    }

    internal static class TrackRouteApplication
    {
        internal static MidiSong CreateRoutedSong(MidiSong source, CompiledTrackRoute route)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (route == null) throw new ArgumentNullException("route");
            MidiSong routed = new MidiSong
            {
                FilePath = source.FilePath,
                FileSizeBytes = source.FileSizeBytes,
                Format = source.Format,
                TrackCount = source.TrackCount,
                TicksPerQuarterNote = source.TicksPerQuarterNote,
                NoteCount = source.NoteCount,
                DurationMicroseconds = source.DurationMicroseconds,
                TrackMetadata = source.TrackMetadata == null ? null :
                    (MidiTrackMetadata[])source.TrackMetadata.Clone()
            };
            routed.SetEventStore(new TrackRoutedEventStore(source.EventStore, route));
            return routed;
        }
    }

    internal static class TrackRoutePlanner
    {
        internal const int MaximumActiveAttacks = 1000000;
        internal const int MaximumDistinctPatchesPerLane = 4096;
        private sealed class MutableLane
        {
            internal readonly TrackRouteLaneProfile Profile = new TrackRouteLaneProfile();
            internal int BankMsb = -1;
            internal int BankLsb = -1;
            internal int Program = -1;
            internal readonly Dictionary<MidiPatchIdentity, ulong> PatchScores =
                new Dictionary<MidiPatchIdentity, ulong>();
            internal bool SustainOn;
            internal readonly List<SustainedAttack> Sustained = new List<SustainedAttack>();
        }

        private struct ActiveAttack
        {
            internal long Time;
            internal int Velocity;
            internal MidiPatchIdentity Patch;
            internal int EventIndex;
            internal int Key;
        }

        private struct SustainedAttack
        {
            internal ActiveAttack Attack;
        }

        private struct ActiveKey : IEquatable<ActiveKey>
        {
            internal TrackLaneKey Lane;
            internal int Key;
            public bool Equals(ActiveKey other) { return Lane.Equals(other.Lane) && Key == other.Key; }
            public override bool Equals(object value) { return value is ActiveKey && Equals((ActiveKey)value); }
            public override int GetHashCode() { return unchecked(Lane.GetHashCode() * 397) ^ Key; }
        }

        private struct LanePair : IEquatable<LanePair>
        {
            internal TrackLaneKey First;
            internal TrackLaneKey Second;
            internal LanePair(TrackLaneKey first, TrackLaneKey second)
            {
                if (first.CompareTo(second) <= 0) { First = first; Second = second; }
                else { First = second; Second = first; }
            }
            public bool Equals(LanePair other) { return First.Equals(other.First) && Second.Equals(other.Second); }
            public override bool Equals(object value) { return value is LanePair && Equals((LanePair)value); }
            public override int GetHashCode() { return unchecked(First.GetHashCode() * 397) ^ Second.GetHashCode(); }
        }

        private sealed class DestinationGroup
        {
            internal int Destination;
            internal readonly List<TrackRouteLaneProfile> Lanes = new List<TrackRouteLaneProfile>();
        }

        internal static CompiledTrackRoute Compile(MidiSong song, TrackRoutingSettings settings)
        {
            if (song == null) throw new ArgumentNullException("song");
            if (settings == null) throw new ArgumentNullException("settings");
            HashSet<LanePair> sameKeyOverlaps;
            List<TrackRouteLaneProfile> profiles = BuildProfiles(song, out sameKeyOverlaps);
            List<string> errors = new List<string>();
            List<int> available = AvailableDestinations(settings, profiles, errors);
            bool needsMelodicDestination = settings.PercussionPolicy != PercussionRoutingPolicy.ReserveChannel10;
            for (int index = 0; index < profiles.Count && !needsMelodicDestination; index++)
                needsMelodicDestination = profiles[index].Lane.SourceChannel != 9;
            if (available.Count == 0 && needsMelodicDestination)
                errors.Add("No usable melodic destination channel is selected.");

            List<DestinationGroup> groups = new List<DestinationGroup>();
            List<TrackRouteAssignment> assignments = new List<TrackRouteAssignment>();
            List<TrackRouteLaneProfile> unresolved = new List<TrackRouteLaneProfile>();
            HashSet<int> used = new HashSet<int>();

            profiles.Sort(ComparePlanningOrder);
            for (int index = 0; index < profiles.Count; index++)
            {
                TrackRouteLaneProfile lane = profiles[index];
                int manual;
                if (settings.ManualDestinations.TryGetValue(lane.Lane, out manual))
                {
                    if (manual < 0 || manual > 15 || !settings.UsableDestinationChannels[manual])
                    {
                        errors.Add(lane.Lane + " has an unavailable manual destination.");
                        unresolved.Add(lane);
                        continue;
                    }
                    DestinationGroup manualGroup = FindOrCreateGroup(groups, manual, used);
                    AddAssignment(assignments, manualGroup, lane, TrackRoutingTier.Aggressive, true,
                        sameKeyOverlaps, settings);
                    continue;
                }

                if (settings.PercussionPolicy == PercussionRoutingPolicy.ReserveChannel10 &&
                    lane.Lane.SourceChannel == 9)
                {
                    if (!settings.UsableDestinationChannels[9])
                    {
                        errors.Add(lane.Lane + " is percussion but destination channel 10 is unavailable.");
                        unresolved.Add(lane);
                    }
                    else
                    {
                        DestinationGroup percussion = FindOrCreateGroup(groups, 9, used);
                        AddAssignment(assignments, percussion, lane, TrackRoutingTier.ExactSafe, false,
                            sameKeyOverlaps, settings);
                    }
                    continue;
                }

                if (settings.MaximumAutomaticTier == TrackRoutingTier.Aggressive)
                {
                    unresolved.Add(lane);
                    continue;
                }

                DestinationGroup exact = FindExactGroup(groups, lane, sameKeyOverlaps, settings);
                if (exact == null)
                {
                    int destination = ChooseUnusedDestination(available, used, lane.Lane.SourceChannel);
                    if (destination >= 0) exact = FindOrCreateGroup(groups, destination, used);
                }
                if (exact == null) unresolved.Add(lane);
                else AddAssignment(assignments, exact, lane, TrackRoutingTier.ExactSafe, false,
                    sameKeyOverlaps, settings);
            }

            if (settings.MaximumAutomaticTier == TrackRoutingTier.Aggressive)
                ResolveAggressiveGlobally(unresolved, assignments, groups, available, used,
                    sameKeyOverlaps, settings);
            else if (settings.MaximumAutomaticTier >= TrackRoutingTier.GmFamily)
                ResolveByTier(unresolved, assignments, groups, sameKeyOverlaps, settings,
                    TrackRoutingTier.GmFamily);
            for (int index = 0; index < unresolved.Count; index++)
                errors.Add(unresolved[index].Lane + " cannot be routed within the selected capacity and tier.");

            assignments.Sort(delegate(TrackRouteAssignment left, TrackRouteAssignment right)
            { return left.Profile.Lane.CompareTo(right.Profile.Lane); });
            List<TrackRouteDestinationPreview> preview = BuildPreview(groups, assignments);
            int[] stateDestinations = new int[16];
            for (int channel = 0; channel < stateDestinations.Length; channel++)
                stateDestinations[channel] = -1;
            for (int index = 0; index < assignments.Count; index++)
            {
                TrackRouteAssignment assignment = assignments[index];
                if (assignment.Omitted || assignment.DestinationChannel < 0) continue;
                int sourceChannel = assignment.Profile.Lane.SourceChannel;
                if (stateDestinations[sourceChannel] >= 0 &&
                    stateDestinations[sourceChannel] != assignment.DestinationChannel)
                    errors.Add("Source channel " + (sourceChannel + 1) +
                        " has global state but its MIDI tracks use multiple destinations.");
                else stateDestinations[sourceChannel] = assignment.DestinationChannel;
            }
            TrackRoutingTier highest = TrackRoutingTier.ExactSafe;
            bool explicitApply = false;
            for (int index = 0; index < assignments.Count; index++)
            {
                if (assignments[index].Tier > highest) highest = assignments[index].Tier;
                if (assignments[index].Tier != TrackRoutingTier.ExactSafe || assignments[index].Manual)
                    explicitApply = true;
            }
            List<TrackRouteLaneProfile> displayLanes = new List<TrackRouteLaneProfile>(profiles);
            displayLanes.Sort(delegate(TrackRouteLaneProfile left, TrackRouteLaneProfile right)
            { return left.Lane.CompareTo(right.Lane); });
            return new CompiledTrackRoute(song.EventStore, song.TrackCount,
                displayLanes, assignments, preview, errors,
                explicitApply, highest);
        }

        private static List<int> AvailableDestinations(TrackRoutingSettings settings,
            List<TrackRouteLaneProfile> profiles, List<string> errors)
        {
            List<int> result = new List<int>();
            int target = Math.Max(1, Math.Min(16, settings.TargetCapacity));
            HashSet<int> manual = new HashSet<int>();
            for (int index = 0; index < profiles.Count; index++)
            {
                int destination;
                if (!settings.ManualDestinations.TryGetValue(
                    profiles[index].Lane, out destination) || destination < 0 ||
                    destination > 15 || !settings.UsableDestinationChannels[destination]) continue;
                bool reservedPercussion = settings.PercussionPolicy ==
                    PercussionRoutingPolicy.ReserveChannel10 && destination == 9 &&
                    profiles[index].Lane.SourceChannel == 9;
                if (!reservedPercussion) manual.Add(destination);
            }
            result.AddRange(manual);
            result.Sort();
            if (result.Count > target)
                errors.Add("Manual corrections use more melodic destinations than the selected capacity.");
            for (int channel = 0; channel < 16 && result.Count < target; channel++)
                if (settings.UsableDestinationChannels[channel] &&
                    !(settings.PercussionPolicy == PercussionRoutingPolicy.ReserveChannel10 && channel == 9) &&
                    !result.Contains(channel))
                    result.Add(channel);
            return result;
        }

        private static void ResolveByTier(List<TrackRouteLaneProfile> unresolved,
            List<TrackRouteAssignment> assignments, List<DestinationGroup> groups,
            HashSet<LanePair> sameKeyOverlaps, TrackRoutingSettings settings, TrackRoutingTier tier)
        {
            for (int index = unresolved.Count - 1; index >= 0; index--)
            {
                TrackRouteLaneProfile lane = unresolved[index];
                DestinationGroup best = null;
                int bestCost = Int32.MaxValue;
                for (int groupIndex = 0; groupIndex < groups.Count; groupIndex++)
                {
                    DestinationGroup group = groups[groupIndex];
                    if (settings.PercussionPolicy == PercussionRoutingPolicy.ReserveChannel10 &&
                        group.Destination == 9) continue;
                    int cost = TierCost(group, lane, sameKeyOverlaps, tier);
                    if (cost < bestCost || (cost == bestCost && best != null &&
                        group.Destination < best.Destination))
                    { best = group; bestCost = cost; }
                }
                if (best == null || bestCost == Int32.MaxValue) continue;
                AddAssignment(assignments, best, lane, tier, false, sameKeyOverlaps, settings);
                unresolved.RemoveAt(index);
            }
        }

        private static void ResolveAggressiveGlobally(
            List<TrackRouteLaneProfile> unresolved,
            List<TrackRouteAssignment> assignments, List<DestinationGroup> groups,
            List<int> available, HashSet<int> used, HashSet<LanePair> overlaps,
            TrackRoutingSettings settings)
        {
            List<TrackRouteLaneProfile> representatives = new List<TrackRouteLaneProfile>();
            for (int index = 0; index < groups.Count; index++)
                representatives.AddRange(groups[index].Lanes);
            while (unresolved.Count != 0)
            {
                bool hasFreeDestination = false;
                for (int index = 0; index < available.Count; index++)
                    if (!used.Contains(available[index]))
                    { hasFreeDestination = true; break; }
                if (!hasFreeDestination) break;
                int bestIndex = 0;
                long bestScore = Int64.MinValue;
                for (int index = 0; index < unresolved.Count; index++)
                {
                    long score = AggressiveRepresentativeScore(
                        unresolved[index], index, unresolved.Count,
                        representatives, overlaps);
                    if (score > bestScore || score == bestScore &&
                        unresolved[index].Lane.CompareTo(unresolved[bestIndex].Lane) < 0)
                    { bestIndex = index; bestScore = score; }
                }
                TrackRouteLaneProfile selected = unresolved[bestIndex];
                int freeDestination = ChooseUnusedDestination(available, used,
                    selected.Lane.SourceChannel);
                DestinationGroup group = FindOrCreateGroup(groups, freeDestination, used);
                AddAssignment(assignments, group, selected, TrackRoutingTier.Aggressive,
                    false, overlaps, settings);
                representatives.Add(selected);
                unresolved.RemoveAt(bestIndex);
            }

            for (int index = 0; index < unresolved.Count; index++)
            {
                TrackRouteLaneProfile lane = unresolved[index];
                DestinationGroup best = null;
                int bestCost = Int32.MaxValue;
                ulong weakestRepresentative = UInt64.MaxValue;
                for (int groupIndex = 0; groupIndex < groups.Count; groupIndex++)
                {
                    DestinationGroup group = groups[groupIndex];
                    if (settings.PercussionPolicy == PercussionRoutingPolicy.ReserveChannel10 &&
                        group.Destination == 9) continue;
                    int cost = TierCost(group, lane, overlaps, TrackRoutingTier.Aggressive);
                    if (cost < bestCost || cost == bestCost && best != null &&
                        group.Destination < best.Destination)
                    { best = group; bestCost = cost; }
                    TrackRouteLaneProfile representative = DominantLane(group);
                    if (representative != null &&
                        representative.DominanceScore < weakestRepresentative)
                        weakestRepresentative = representative.DominanceScore;
                }
                bool destructiveMerge = bestCost >= 15 && weakestRepresentative != UInt64.MaxValue &&
                    lane.DominanceScore < weakestRepresentative;
                if (destructiveMerge)
                {
                    TrackRouteAssignment omitted = new TrackRouteAssignment
                    {
                        Profile = lane,
                        DestinationChannel = -1,
                        Tier = TrackRoutingTier.Aggressive,
                        Omitted = true,
                        Retimbred = true
                    };
                    omitted.Warnings.Add("Omitted " + lane.AttackCount.ToString() +
                        (lane.AttackCount == 1 ? " note start" : " note starts") +
                        " because every available merge has destructive overlap/state conflicts");
                    assignments.Add(omitted);
                }
                else if (best != null)
                    AddAssignment(assignments, best, lane, TrackRoutingTier.Aggressive,
                        false, overlaps, settings);
            }
            unresolved.Clear();
        }

        private static long AggressiveRepresentativeScore(TrackRouteLaneProfile lane,
            int dominanceIndex, int candidateCount,
            List<TrackRouteLaneProfile> selected, HashSet<LanePair> overlaps)
        {
            long score = (long)(candidateCount - dominanceIndex) * 100L;
            for (int index = 0; index < selected.Count; index++)
            {
                TrackRouteLaneProfile other = selected[index];
                if (overlaps.Contains(new LanePair(lane.Lane, other.Lane))) score += 120;
                if (StateConflict(lane, other)) score += 80;
                if (!lane.RepresentativePatch.Equals(other.RepresentativePatch)) score += 60;
                if (lane.RepresentativePatch.GmFamily != other.RepresentativePatch.GmFamily)
                    score += 30;
                if (TimeRangesOverlap(lane, other)) score += 20;
            }
            return score;
        }

        private static int TierCost(DestinationGroup group, TrackRouteLaneProfile lane,
            HashSet<LanePair> overlaps, TrackRoutingTier tier)
        {
            int cost = 0;
            for (int index = 0; index < group.Lanes.Count; index++)
            {
                TrackRouteLaneProfile other = group.Lanes[index];
                if (tier == TrackRoutingTier.GmFamily &&
                    (lane.RepresentativePatch.GmFamily < 0 ||
                    lane.RepresentativePatch.GmFamily != other.RepresentativePatch.GmFamily))
                    return Int32.MaxValue;
                if (!lane.RepresentativePatch.Equals(other.RepresentativePatch)) cost += 4;
                if (StateConflict(lane, other)) cost += 3;
                if (overlaps.Contains(new LanePair(lane.Lane, other.Lane))) cost += 8;
                if (!TimeRangesOverlap(lane, other)) cost += 1;
            }
            return cost;
        }

        private static DestinationGroup FindExactGroup(List<DestinationGroup> groups,
            TrackRouteLaneProfile lane, HashSet<LanePair> overlaps, TrackRoutingSettings settings)
        {
            DestinationGroup best = null;
            for (int groupIndex = 0; groupIndex < groups.Count; groupIndex++)
            {
                DestinationGroup group = groups[groupIndex];
                if (settings.PercussionPolicy == PercussionRoutingPolicy.ReserveChannel10 &&
                    group.Destination == 9 && lane.Lane.SourceChannel != 9) continue;
                bool compatible = true;
                for (int laneIndex = 0; laneIndex < group.Lanes.Count; laneIndex++)
                    if (!ExactCompatible(lane, group.Lanes[laneIndex], overlaps))
                    { compatible = false; break; }
                if (!compatible) continue;
                if (group.Destination == lane.Lane.SourceChannel) return group;
                if (best == null || group.Destination < best.Destination) best = group;
            }
            return best;
        }

        private static bool ExactCompatible(TrackRouteLaneProfile left, TrackRouteLaneProfile right,
            HashSet<LanePair> overlaps)
        {
            return PatchStatesRemainCompatibleWhileSounding(left, right) &&
                !StateConflict(left, right) &&
                !overlaps.Contains(new LanePair(left.Lane, right.Lane));
        }

        private static bool PatchStatesRemainCompatibleWhileSounding(
            TrackRouteLaneProfile left, TrackRouteLaneProfile right)
        {
            List<int> boundaries = new List<int>();
            Dictionary<int, int> leftSoundDeltas = new Dictionary<int, int>();
            Dictionary<int, int> rightSoundDeltas = new Dictionary<int, int>();
            AddPatchBoundaries(boundaries, left, leftSoundDeltas);
            AddPatchBoundaries(boundaries, right, rightSoundDeltas);
            boundaries.Sort();
            int leftPatch = 0, rightPatch = 0;
            int leftSound = 0, rightSound = 0;
            MidiPatchIdentity leftState = new MidiPatchIdentity(-1, -1, -1);
            MidiPatchIdentity rightState = new MidiPatchIdentity(-1, -1, -1);
            MidiPatchIdentity sharedState = new MidiPatchIdentity(-1, -1, -1);
            int previous = Int32.MinValue;
            for (int boundaryIndex = 0; boundaryIndex < boundaries.Count; boundaryIndex++)
            {
                int eventIndex = boundaries[boundaryIndex];
                if (eventIndex == previous) continue;
                previous = eventIndex;

                ApplyPatchChanges(left.PatchTimeline, eventIndex, ref leftPatch,
                    ref leftState, ref sharedState);
                ApplyPatchChanges(right.PatchTimeline, eventIndex, ref rightPatch,
                    ref rightState, ref sharedState);
                int delta;
                if (leftSoundDeltas.TryGetValue(eventIndex, out delta)) leftSound += delta;
                if (rightSoundDeltas.TryGetValue(eventIndex, out delta)) rightSound += delta;
                if (leftSound > 0 && !sharedState.Equals(leftState)) return false;
                if (rightSound > 0 && !sharedState.Equals(rightState)) return false;
            }
            return true;
        }

        private static void AddPatchBoundaries(List<int> boundaries, TrackRouteLaneProfile profile,
            Dictionary<int, int> soundDeltas)
        {
            for (int index = 0; index < profile.PatchTimeline.Count; index++)
                boundaries.Add(profile.PatchTimeline[index].EventIndex);
            for (int index = 0; index < profile.SoundingIntervals.Count; index++)
            {
                TrackRouteSoundInterval interval = profile.SoundingIntervals[index];
                AddSoundDelta(boundaries, soundDeltas, interval.StartEventIndex, 1);
                AddSoundDelta(boundaries, soundDeltas, interval.EndEventIndex, -1);
            }
        }

        private static void AddSoundDelta(List<int> boundaries, Dictionary<int, int> deltas,
            int eventIndex, int delta)
        {
            int prior;
            deltas.TryGetValue(eventIndex, out prior);
            deltas[eventIndex] = prior + delta;
            boundaries.Add(eventIndex);
        }

        private static void ApplyPatchChanges(List<TrackRoutePatchChange> changes, int eventIndex,
            ref int cursor, ref MidiPatchIdentity laneState, ref MidiPatchIdentity sharedState)
        {
            while (cursor < changes.Count && changes[cursor].EventIndex == eventIndex)
            {
                laneState = changes[cursor].Patch;
                sharedState = laneState;
                cursor++;
            }
        }

        private static bool StateConflict(TrackRouteLaneProfile left, TrackRouteLaneProfile right)
        {
            if (!TimeRangesOverlap(left, right)) return false;
            if (!left.HasDynamicState && !right.HasDynamicState) return false;
            return left.DynamicStateSignature != right.DynamicStateSignature;
        }

        private static bool TimeRangesOverlap(TrackRouteLaneProfile left, TrackRouteLaneProfile right)
        {
            if (left.FirstAttackMicroseconds < 0 || right.FirstAttackMicroseconds < 0) return false;
            return left.FirstAttackMicroseconds <= right.LastReleaseMicroseconds &&
                right.FirstAttackMicroseconds <= left.LastReleaseMicroseconds;
        }

        private static void AddAssignment(List<TrackRouteAssignment> assignments,
            DestinationGroup group, TrackRouteLaneProfile lane, TrackRoutingTier tier, bool manual,
            HashSet<LanePair> overlaps, TrackRoutingSettings settings)
        {
            TrackRouteAssignment assignment = new TrackRouteAssignment
            {
                Profile = lane, DestinationChannel = group.Destination, Tier = tier, Manual = manual
            };
            for (int index = 0; index < group.Lanes.Count; index++)
            {
                TrackRouteLaneProfile other = group.Lanes[index];
                if (StateConflict(lane, other)) assignment.StateConflict = true;
                if (overlaps.Contains(new LanePair(lane.Lane, other.Lane))) assignment.SameKeyOverlap = true;
                if (!TimeRangesOverlap(lane, other) &&
                    !lane.RepresentativePatch.Equals(other.RepresentativePatch)) assignment.TimeHandoff = true;
                if (!lane.RepresentativePatch.Equals(other.RepresentativePatch)) assignment.Retimbred = true;
            }
            assignment.PercussionException = (lane.Lane.SourceChannel == 9) != (group.Destination == 9);
            if (assignment.StateConflict) assignment.Warnings.Add("Conflicting channel-wide state");
            if (assignment.SameKeyOverlap) assignment.Warnings.Add("Receiver-dependent same-key overlap");
            if (assignment.TimeHandoff) assignment.Warnings.Add("Audible patch/state handoff");
            if (assignment.Retimbred) assignment.Warnings.Add("Representative patch retimbres this lane");
            if (tier == TrackRoutingTier.GmFamily &&
                (!lane.RepresentativePatch.IsGeneralMidiBank ||
                group.Lanes.Count != 0 && !group.Lanes[0].RepresentativePatch.IsGeneralMidiBank))
                assignment.Warnings.Add("GM-family inference uses an unknown or nonzero bank");
            if (assignment.PercussionException) assignment.Warnings.Add("Percussion convention override");
            group.Lanes.Add(lane);
            assignments.Add(assignment);
        }

        private static List<TrackRouteDestinationPreview> BuildPreview(List<DestinationGroup> groups,
            List<TrackRouteAssignment> assignments)
        {
            List<TrackRouteDestinationPreview> result = new List<TrackRouteDestinationPreview>();
            groups.Sort(delegate(DestinationGroup left, DestinationGroup right)
            { return left.Destination.CompareTo(right.Destination); });
            for (int groupIndex = 0; groupIndex < groups.Count; groupIndex++)
            {
                DestinationGroup group = groups[groupIndex];
                TrackRouteLaneProfile representative = DominantLane(group);
                TrackRouteDestinationPreview preview = new TrackRouteDestinationPreview
                {
                    DestinationChannel = group.Destination,
                    RepresentativeLane = representative.Lane,
                    RepresentativePatch = representative.RepresentativePatch
                };
                for (int index = 0; index < assignments.Count; index++)
                    if (assignments[index].DestinationChannel == group.Destination)
                    {
                        assignments[index].RepresentativePatch =
                            assignments[index].Profile.Lane.Equals(representative.Lane);
                        preview.Assignments.Add(assignments[index]);
                    }
                result.Add(preview);
            }
            return result;
        }

        private static TrackRouteLaneProfile DominantLane(DestinationGroup group)
        {
            if (group == null || group.Lanes.Count == 0) return null;
            TrackRouteLaneProfile representative = group.Lanes[0];
            for (int laneIndex = 1; laneIndex < group.Lanes.Count; laneIndex++)
                if (group.Lanes[laneIndex].DominanceScore > representative.DominanceScore ||
                    (group.Lanes[laneIndex].DominanceScore == representative.DominanceScore &&
                    group.Lanes[laneIndex].Lane.CompareTo(representative.Lane) < 0))
                    representative = group.Lanes[laneIndex];
            return representative;
        }

        private static DestinationGroup FindOrCreateGroup(List<DestinationGroup> groups,
            int destination, HashSet<int> used)
        {
            for (int index = 0; index < groups.Count; index++)
                if (groups[index].Destination == destination) return groups[index];
            DestinationGroup result = new DestinationGroup { Destination = destination };
            groups.Add(result);
            used.Add(destination);
            return result;
        }

        private static int ChooseUnusedDestination(List<int> available, HashSet<int> used, int preferred)
        {
            if (available.Contains(preferred) && !used.Contains(preferred)) return preferred;
            for (int index = 0; index < available.Count; index++)
                if (!used.Contains(available[index])) return available[index];
            return -1;
        }

        private static int ComparePlanningOrder(TrackRouteLaneProfile left, TrackRouteLaneProfile right)
        {
            int result = right.DominanceScore.CompareTo(left.DominanceScore);
            return result != 0 ? result : left.Lane.CompareTo(right.Lane);
        }

        private static List<TrackRouteLaneProfile> BuildProfiles(MidiSong song,
            out HashSet<LanePair> sameKeyOverlaps)
        {
            Dictionary<TrackLaneKey, MutableLane> lanes = new Dictionary<TrackLaneKey, MutableLane>();
            Dictionary<ActiveKey, Queue<ActiveAttack>> active =
                new Dictionary<ActiveKey, Queue<ActiveAttack>>();
            Dictionary<int, Dictionary<TrackLaneKey, int>> activeByKey =
                new Dictionary<int, Dictionary<TrackLaneKey, int>>();
            int activeAttackCount = 0;
            sameKeyOverlaps = new HashSet<LanePair>();
            bool[] trackSystem = new bool[Math.Max(0, song.TrackCount)];
            MidiEventReader events = song.GetEventReader();
            HashSet<TrackLaneKey> noteLanes = new HashSet<TrackLaneKey>();
            for (int eventIndex = 0; eventIndex < events.Count; eventIndex++)
            {
                MidiEventView source = events[eventIndex];
                if (source.Channel >= 0 && (source.Status & 0xF0) == 0x90 &&
                    source.DataLength >= 3 && source.GetDataByte(2) != 0)
                    noteLanes.Add(new TrackLaneKey(source.Track, source.Channel));
            }
            foreach (TrackLaneKey key in noteLanes)
            {
                MutableLane created = new MutableLane();
                created.Profile.Lane = key;
                MidiTrackMetadata metadata = song.MetadataForTrack(key.Track);
                created.Profile.TrackName = metadata.TrackName;
                created.Profile.InstrumentName = metadata.InstrumentName;
                lanes.Add(key, created);
            }
            int[] channelBankMsb = new int[16];
            int[] channelBankLsb = new int[16];
            int[] channelProgram = new int[16];
            for (int channel = 0; channel < 16; channel++)
                channelBankMsb[channel] = channelBankLsb[channel] =
                    channelProgram[channel] = -1;
            for (int eventIndex = 0; eventIndex < events.Count; eventIndex++)
            {
                MidiEventView midiEvent = events[eventIndex];
                if (midiEvent.Channel < 0)
                {
                    if (midiEvent.Track >= 0 && midiEvent.Track < trackSystem.Length)
                        trackSystem[midiEvent.Track] = true;
                    continue;
                }
                TrackLaneKey key = new TrackLaneKey(midiEvent.Track, midiEvent.Channel);
                MutableLane lane;
                lanes.TryGetValue(key, out lane);
                if (lane != null) lane.Profile.EventCount++;
                int command = midiEvent.Status & 0xF0;
                if (command == 0xB0 && midiEvent.DataLength >= 3)
                {
                    int controller = midiEvent.GetDataByte(1);
                    int value = midiEvent.GetDataByte(2);
                    if (controller == 0)
                    {
                        channelBankMsb[midiEvent.Channel] = value;
                        RecordPatchChangeForChannel(lanes, midiEvent.Channel,
                            eventIndex, channelBankMsb[midiEvent.Channel],
                            channelBankLsb[midiEvent.Channel], channelProgram[midiEvent.Channel]);
                    }
                    else if (controller == 32)
                    {
                        channelBankLsb[midiEvent.Channel] = value;
                        RecordPatchChangeForChannel(lanes, midiEvent.Channel,
                            eventIndex, channelBankMsb[midiEvent.Channel],
                            channelBankLsb[midiEvent.Channel], channelProgram[midiEvent.Channel]);
                    }
                    else
                    {
                        foreach (KeyValuePair<TrackLaneKey, MutableLane> pair in lanes)
                        {
                            if (pair.Key.SourceChannel != midiEvent.Channel) continue;
                            MutableLane target = pair.Value;
                            target.Profile.HasDynamicState = true;
                            if (controller == 64) target.Profile.HasSustain = true;
                            MixState(target.Profile, midiEvent);
                            if (controller == 64)
                            {
                                bool sustainOn = value >= 64;
                                if (target.SustainOn && !sustainOn)
                                    ReleaseSustained(target, midiEvent.IntendedMicroseconds,
                                        eventIndex, ref activeAttackCount, activeByKey);
                                target.SustainOn = sustainOn;
                            }
                            else if (controller == 121)
                            {
                                if (target.SustainOn)
                                    ReleaseSustained(target, midiEvent.IntendedMicroseconds,
                                        eventIndex, ref activeAttackCount, activeByKey);
                                target.SustainOn = false;
                            }
                            else if (controller == 120 || controller == 123)
                            {
                                ReleaseAllLaneAttacks(pair.Key, target,
                                    midiEvent.IntendedMicroseconds, eventIndex, active,
                                    ref activeAttackCount, activeByKey);
                                target.SustainOn = false;
                            }
                        }
                    }
                }
                else if (command == 0xC0 && midiEvent.DataLength >= 2)
                {
                    channelProgram[midiEvent.Channel] = midiEvent.GetDataByte(1);
                    RecordPatchChangeForChannel(lanes, midiEvent.Channel,
                        eventIndex, channelBankMsb[midiEvent.Channel],
                        channelBankLsb[midiEvent.Channel], channelProgram[midiEvent.Channel]);
                }
                else if (command == 0xD0 || command == 0xE0)
                {
                    foreach (KeyValuePair<TrackLaneKey, MutableLane> pair in lanes)
                        if (pair.Key.SourceChannel == midiEvent.Channel)
                        {
                            pair.Value.Profile.HasDynamicState = true;
                            MixState(pair.Value.Profile, midiEvent);
                        }
                }

                bool attack = command == 0x90 && midiEvent.DataLength >= 3 && midiEvent.GetDataByte(2) != 0;
                bool release = (command == 0x80 && midiEvent.DataLength >= 3) ||
                    (command == 0x90 && midiEvent.DataLength >= 3 && midiEvent.GetDataByte(2) == 0);
                if (attack)
                {
                    if (lane == null) continue;
                    int note = midiEvent.GetDataByte(1);
                    MidiPatchIdentity patch = new MidiPatchIdentity(
                        channelBankMsb[midiEvent.Channel], channelBankLsb[midiEvent.Channel],
                        channelProgram[midiEvent.Channel]);
                    if (!lane.Profile.PatchHistory.Contains(patch) &&
                        lane.Profile.PatchHistory.Count >= MaximumDistinctPatchesPerLane)
                        throw new InvalidOperationException("Track-routing analysis exceeded the bounded distinct-patch limit for " + key + ".");
                    lane.Profile.PatchHistory.Add(patch);
                    lane.Profile.AttackCount++;
                    if (lane.Profile.FirstAttackMicroseconds < 0)
                        lane.Profile.FirstAttackMicroseconds = midiEvent.IntendedMicroseconds;
                    ActiveKey activeKey = new ActiveKey { Lane = key, Key = note };
                    Queue<ActiveAttack> queue;
                    if (!active.TryGetValue(activeKey, out queue))
                    { queue = new Queue<ActiveAttack>(); active.Add(activeKey, queue); }
                    if (activeAttackCount >= MaximumActiveAttacks)
                        throw new InvalidOperationException("Track-routing analysis exceeded the bounded active-note occurrence limit.");
                    queue.Enqueue(new ActiveAttack { Time = midiEvent.IntendedMicroseconds,
                        Velocity = midiEvent.GetDataByte(2), Patch = patch,
                        EventIndex = eventIndex, Key = note });
                    activeAttackCount++;
                    Dictionary<TrackLaneKey, int> owners;
                    if (!activeByKey.TryGetValue(note, out owners))
                    { owners = new Dictionary<TrackLaneKey, int>(); activeByKey.Add(note, owners); }
                    foreach (KeyValuePair<TrackLaneKey, int> owner in owners)
                        if (owner.Value > 0 && !owner.Key.Equals(key))
                            sameKeyOverlaps.Add(new LanePair(key, owner.Key));
                    int ownerCount;
                    owners.TryGetValue(key, out ownerCount);
                    owners[key] = ownerCount + 1;
                }
                else if (release)
                {
                    if (lane == null) continue;
                    int note = midiEvent.GetDataByte(1);
                    ActiveKey activeKey = new ActiveKey { Lane = key, Key = note };
                    Queue<ActiveAttack> queue;
                    if (active.TryGetValue(activeKey, out queue) && queue.Count != 0)
                    {
                        ActiveAttack started = queue.Dequeue();
                        if (lane.SustainOn)
                        {
                            lane.Sustained.Add(new SustainedAttack { Attack = started });
                        }
                        else
                        {
                            FinalizeAttack(lane, started, midiEvent.IntendedMicroseconds, eventIndex);
                            activeAttackCount--;
                            DecrementOwner(activeByKey, note, key);
                        }
                    }
                }
            }
            foreach (KeyValuePair<ActiveKey, Queue<ActiveAttack>> pending in active)
            {
                MutableLane lane = lanes[pending.Key.Lane];
                while (pending.Value.Count != 0)
                {
                    ActiveAttack attack = pending.Value.Dequeue();
                    FinalizeAttack(lane, attack, song.DurationMicroseconds, events.Count);
                }
            }
            foreach (MutableLane lane in lanes.Values)
                ReleaseSustainedAtEnd(lane, song.DurationMicroseconds, events.Count);
            List<TrackRouteLaneProfile> result = new List<TrackRouteLaneProfile>(lanes.Count);
            foreach (MutableLane lane in lanes.Values)
            {
                lane.Profile.SoundingIntervals.Sort(
                    delegate(TrackRouteSoundInterval left, TrackRouteSoundInterval right)
                    {
                        int value = left.StartEventIndex.CompareTo(right.StartEventIndex);
                        if (value != 0) return value;
                        value = left.EndEventIndex.CompareTo(right.EndEventIndex);
                        return value != 0 ? value : left.Key.CompareTo(right.Key);
                    });
                long prefixMaximumEnd = Int64.MinValue;
                for (int intervalIndex = 0;
                    intervalIndex < lane.Profile.SoundingIntervals.Count; intervalIndex++)
                {
                    TrackRouteSoundInterval interval =
                        lane.Profile.SoundingIntervals[intervalIndex];
                    prefixMaximumEnd = Math.Max(prefixMaximumEnd,
                        interval.EndMicroseconds);
                    interval.PrefixMaximumEndMicroseconds = prefixMaximumEnd;
                    lane.Profile.SoundingIntervals[intervalIndex] = interval;
                }
                lane.Profile.TrackContainsSystemTraffic = lane.Profile.Lane.Track >= 0 &&
                    lane.Profile.Lane.Track < trackSystem.Length && trackSystem[lane.Profile.Lane.Track];
                MidiPatchIdentity representative = new MidiPatchIdentity(
                    channelBankMsb[lane.Profile.Lane.SourceChannel],
                    channelBankLsb[lane.Profile.Lane.SourceChannel],
                    channelProgram[lane.Profile.Lane.SourceChannel]);
                ulong best = 0;
                foreach (KeyValuePair<MidiPatchIdentity, ulong> score in lane.PatchScores)
                    if (score.Value > best || (score.Value == best && ComparePatch(score.Key, representative) < 0))
                    { best = score.Value; representative = score.Key; }
                lane.Profile.RepresentativePatch = representative;
                lane.Profile.DominanceScore = best;
                if (lane.Profile.PatchHistory.Count == 0) lane.Profile.PatchHistory.Add(representative);
                result.Add(lane.Profile);
            }
            return result;
        }

        private static void RecordPatchChangeForChannel(
            Dictionary<TrackLaneKey, MutableLane> lanes, int sourceChannel,
            int eventIndex, int bankMsb, int bankLsb, int program)
        {
            MidiPatchIdentity patch = new MidiPatchIdentity(bankMsb, bankLsb, program);
            foreach (KeyValuePair<TrackLaneKey, MutableLane> pair in lanes)
            {
                if (pair.Key.SourceChannel != sourceChannel) continue;
                List<TrackRoutePatchChange> timeline = pair.Value.Profile.PatchTimeline;
                if (timeline.Count != 0 && timeline[timeline.Count - 1].Patch.Equals(patch))
                    continue;
                timeline.Add(new TrackRoutePatchChange { EventIndex = eventIndex, Patch = patch });
            }
        }

        private static void FinalizeAttack(MutableLane lane, ActiveAttack attack,
            long endMicroseconds, int endEventIndex)
        {
            AddDominance(lane, attack, endMicroseconds);
            lane.Profile.LastReleaseMicroseconds = Math.Max(lane.Profile.LastReleaseMicroseconds,
                endMicroseconds);
            lane.Profile.SoundingIntervals.Add(new TrackRouteSoundInterval
            {
                StartEventIndex = attack.EventIndex,
                EndEventIndex = endEventIndex,
                StartMicroseconds = attack.Time,
                EndMicroseconds = endMicroseconds,
                Key = attack.Key
            });
        }

        private static void ReleaseSustained(MutableLane lane, long endMicroseconds, int endEventIndex,
            ref int activeAttackCount,
            Dictionary<int, Dictionary<TrackLaneKey, int>> activeByKey)
        {
            for (int index = 0; index < lane.Sustained.Count; index++)
            {
                ActiveAttack attack = lane.Sustained[index].Attack;
                FinalizeAttack(lane, attack, endMicroseconds, endEventIndex);
                activeAttackCount--;
                DecrementOwner(activeByKey, attack.Key, lane.Profile.Lane);
            }
            lane.Sustained.Clear();
        }

        private static void ReleaseSustainedAtEnd(MutableLane lane, long endMicroseconds,
            int endEventIndex)
        {
            for (int index = 0; index < lane.Sustained.Count; index++)
                FinalizeAttack(lane, lane.Sustained[index].Attack, endMicroseconds, endEventIndex);
            lane.Sustained.Clear();
        }

        private static void ReleaseAllLaneAttacks(TrackLaneKey laneKey, MutableLane lane,
            long endMicroseconds, int endEventIndex,
            Dictionary<ActiveKey, Queue<ActiveAttack>> active, ref int activeAttackCount,
            Dictionary<int, Dictionary<TrackLaneKey, int>> activeByKey)
        {
            foreach (KeyValuePair<ActiveKey, Queue<ActiveAttack>> pair in active)
            {
                if (!pair.Key.Lane.Equals(laneKey)) continue;
                while (pair.Value.Count != 0)
                {
                    ActiveAttack attack = pair.Value.Dequeue();
                    FinalizeAttack(lane, attack, endMicroseconds, endEventIndex);
                    activeAttackCount--;
                    DecrementOwner(activeByKey, attack.Key, laneKey);
                }
            }
            ReleaseSustained(lane, endMicroseconds, endEventIndex,
                ref activeAttackCount, activeByKey);
        }

        private static void DecrementOwner(
            Dictionary<int, Dictionary<TrackLaneKey, int>> activeByKey,
            int note, TrackLaneKey lane)
        {
            Dictionary<TrackLaneKey, int> owners;
            if (!activeByKey.TryGetValue(note, out owners)) return;
            int ownerCount;
            if (!owners.TryGetValue(lane, out ownerCount)) return;
            if (ownerCount <= 1) owners.Remove(lane); else owners[lane] = ownerCount - 1;
            if (owners.Count == 0) activeByKey.Remove(note);
        }

        private static void AddDominance(MutableLane lane, ActiveAttack attack, long releaseTime)
        {
            ulong duration = (ulong)Math.Max(1, releaseTime - attack.Time);
            ulong amount = duration > UInt64.MaxValue / (uint)Math.Max(1, attack.Velocity)
                ? UInt64.MaxValue : duration * (uint)Math.Max(1, attack.Velocity);
            ulong prior;
            lane.PatchScores.TryGetValue(attack.Patch, out prior);
            lane.PatchScores[attack.Patch] = UInt64.MaxValue - prior < amount ? UInt64.MaxValue : prior + amount;
        }

        private static void MixState(TrackRouteLaneProfile profile, MidiEventView midiEvent)
        {
            ulong hash = profile.DynamicStateSignature == 0 ? 1469598103934665603UL : profile.DynamicStateSignature;
            hash = (hash ^ (ulong)midiEvent.IntendedMicroseconds) * 1099511628211UL;
            for (int index = 0; index < midiEvent.DataLength; index++)
                hash = (hash ^ midiEvent.GetDataByte(index)) * 1099511628211UL;
            profile.DynamicStateSignature = hash;
        }

        private static int ComparePatch(MidiPatchIdentity left, MidiPatchIdentity right)
        {
            int value = left.BankMsb.CompareTo(right.BankMsb);
            if (value != 0) return value;
            value = left.BankLsb.CompareTo(right.BankLsb);
            return value != 0 ? value : left.Program.CompareTo(right.Program);
        }
    }
}
