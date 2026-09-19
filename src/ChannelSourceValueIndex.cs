using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Compact, immutable lookup for the latest source-requested channel value.
    // Only state-changing source events are indexed; notes and unrelated MIDI
    // never add storage. The parser feeds this during its existing final pass.
    internal sealed class ChannelSourceValueIndex
    {
        private const int AttributeCount = 9;
        private readonly IMidiEventStore _events;
        private readonly List<SourceValueChange>[] _changes;

        internal ChannelSourceValueIndex(IMidiEventStore events, List<SourceValueChange>[] changes)
        {
            _events = events;
            _changes = changes;
        }

        internal static Builder CreateBuilder() { return new Builder(); }

        internal static ChannelSourceValueIndex Build(IMidiEventStore events)
        {
            if (events == null) throw new ArgumentNullException("events");
            Builder builder = new Builder();
            for (int index = 0; index < events.Count; index++) builder.Add(events.GetEvent(index), index);
            return builder.Complete(events);
        }

        internal bool TryGetLatest(int channel, ChannelAttribute attribute, long positionMicroseconds, out int value)
        {
            value = 0;
            if (channel < 0 || channel >= 16) return false;
            int slot = channel * AttributeCount + (int)attribute;
            List<SourceValueChange> list = _changes[slot];
            if (list == null || list.Count == 0) return false;
            int low = 0;
            int high = list.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                MidiEvent midiEvent = _events.GetEvent(list[middle].EventIndex);
                if (midiEvent.IntendedMicroseconds <= positionMicroseconds) low = middle + 1;
                else high = middle;
            }
            if (low == 0) return false;
            value = list[low - 1].Value;
            return true;
        }

        internal sealed class Builder
        {
            private readonly List<SourceValueChange>[] _changes = new List<SourceValueChange>[16 * AttributeCount];

            internal void Add(MidiEvent midiEvent, int eventIndex)
            {
                if (midiEvent == null || midiEvent.Channel < 0 || midiEvent.Channel >= 16) return;
                int status = midiEvent.Status & 0xF0;
                if (status == 0xB0 && midiEvent.DataLength > 2 && midiEvent.GetDataByte(1) == 121)
                {
                    Add(midiEvent.Channel, ChannelAttribute.Expression, eventIndex, 127);
                    Add(midiEvent.Channel, ChannelAttribute.Pan, eventIndex, 64);
                    Add(midiEvent.Channel, ChannelAttribute.Sustain, eventIndex, 0);
                    Add(midiEvent.Channel, ChannelAttribute.PitchBend, eventIndex, 0);
                    Add(midiEvent.Channel, ChannelAttribute.Aftertouch, eventIndex, 0);
                    return;
                }
                ChannelAttribute attribute;
                int value;
                if (ChannelOverrideState.TryClassify(midiEvent, out attribute, out value))
                    Add(midiEvent.Channel, attribute, eventIndex, value);
            }

            private void Add(int channel, ChannelAttribute attribute, int eventIndex, int value)
            {
                int slot = channel * AttributeCount + (int)attribute;
                List<SourceValueChange> list = _changes[slot];
                if (list == null) _changes[slot] = list = new List<SourceValueChange>();
                list.Add(new SourceValueChange(eventIndex, value));
            }

            internal ChannelSourceValueIndex Complete(IMidiEventStore events)
            {
                return new ChannelSourceValueIndex(events, _changes);
            }
        }

        internal struct SourceValueChange
        {
            internal readonly int EventIndex;
            internal readonly int Value;
            internal SourceValueChange(int eventIndex, int value) { EventIndex = eventIndex; Value = value; }
        }
    }
}
