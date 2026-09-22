using System;
using System.Collections.Generic;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class ChannelSourceValueIndex
    {
        internal const int SegmentCapacity = 4096;
        private const int AttributeCount = 9;
        private readonly IMidiEventStore _events;
        private readonly SourceValueSeries[] _changes;

        private sealed class SourceValueSeries
        {
            internal readonly SourceValueChange[][] Segments;
            internal readonly int Count;
            internal SourceValueSeries(SourceValueChange[][] segments, int count) { Segments = segments; Count = count; }
            internal SourceValueChange this[int index]
            { get { return Segments[index >> 12][index & (SegmentCapacity - 1)]; } }
        }

        private ChannelSourceValueIndex(IMidiEventStore events, SourceValueSeries[] changes)
        { _events = events; _changes = changes; }

        internal static Builder CreateBuilder() { return new Builder(); }

        internal static ChannelSourceValueIndex Build(IMidiEventStore events)
        {
            if (events == null) throw new ArgumentNullException("events");
            Builder builder = new Builder();
            for (int index = 0; index < events.Count; index++) builder.Add(events.GetEvent(index), index);
            return builder.Complete(events, CancellationToken.None);
        }

        internal bool TryGetLatest(int channel, ChannelAttribute attribute, long positionMicroseconds, out int value)
        {
            value = 0;
            if (channel < 0 || channel >= 16) return false;
            SourceValueSeries series = _changes[channel * AttributeCount + (int)attribute];
            if (series == null || series.Count == 0) return false;
            int low = 0;
            int high = series.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (_events.GetEvent(series[middle].EventIndex).IntendedMicroseconds <= positionMicroseconds) low = middle + 1;
                else high = middle;
            }
            if (low == 0) return false;
            value = series[low - 1].Value;
            return true;
        }

        internal sealed class Builder
        {
            private readonly SeriesBuilder[] _changes = new SeriesBuilder[16 * AttributeCount];

            internal void Add(MidiEvent midiEvent, int eventIndex)
            { if (midiEvent != null) Add(MidiEventView.FromEvent(midiEvent, eventIndex), eventIndex); }

            internal void Add(MidiEventView midiEvent, int eventIndex)
            {
                if (!midiEvent.IsValid || midiEvent.Channel < 0 || midiEvent.Channel >= 16) return;
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
                SeriesBuilder series = _changes[slot];
                if (series == null) _changes[slot] = series = new SeriesBuilder();
                series.Add(new SourceValueChange(eventIndex, value));
            }

            internal ChannelSourceValueIndex Complete(IMidiEventStore events)
            { return Complete(events, CancellationToken.None); }

            internal ChannelSourceValueIndex Complete(IMidiEventStore events, CancellationToken cancellationToken)
            {
                SourceValueSeries[] completed = new SourceValueSeries[_changes.Length];
                for (int slot = 0; slot < _changes.Length; slot++)
                {
                    if ((slot & 15) == 0) cancellationToken.ThrowIfCancellationRequested();
                    if (_changes[slot] != null) completed[slot] = _changes[slot].Complete();
                }
                return new ChannelSourceValueIndex(events, completed);
            }
        }

        private sealed class SeriesBuilder
        {
            private readonly List<SourceValueChange[]> _segments = new List<SourceValueChange[]>();
            private SourceValueChange[] _current;
            private int _offset;
            private int _count;

            internal void Add(SourceValueChange value)
            {
                if (_current == null || _offset == _current.Length)
                {
                    _current = new SourceValueChange[SegmentCapacity];
                    _segments.Add(_current);
                    _offset = 0;
                }
                _current[_offset++] = value;
                _count++;
            }

            internal SourceValueSeries Complete()
            {
                if (_current != null && _offset != _current.Length)
                {
                    SourceValueChange[] trimmed = new SourceValueChange[_offset];
                    Array.Copy(_current, trimmed, _offset);
                    _segments[_segments.Count - 1] = trimmed;
                }
                return new SourceValueSeries(_segments.ToArray(), _count);
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
