using System;
using System.Collections.Generic;
using System.Threading;

namespace MidiBottleneck
{
    // Immutable segmented histories for state that can be restored without
    // replaying notes or scanning from the start of a large song.
    internal sealed class MidiStateChaseIndex
    {
        internal const int SegmentCapacity = 4096;
        private const int ControllerCount = 120;
        private const int ProgramSlot = 120;
        private const int PitchBendSlot = 121;
        private const int ChannelPressureSlot = 122;
        private const int SlotsPerChannel = 123;
        private readonly ValueSeries[] _state;
        private readonly ValueSeries[] _resets;
        private readonly ValueSeries[] _selectors;
        private readonly ParameterSeries[][] _parameters;

        private MidiStateChaseIndex(ValueSeries[] state, ValueSeries[] resets,
            ValueSeries[] selectors, ParameterSeries[][] parameters)
        { _state = state; _resets = resets; _selectors = selectors; _parameters = parameters; }

        internal long EntryCount { get; private set; }
        internal int ParameterSeriesCount { get; private set; }
        internal long ApproximateRetainedBytes
        { get { return EntryCount * 8L + ParameterSeriesCount * 24L + _state.Length * IntPtr.Size; } }

        internal static Builder CreateBuilder() { return new Builder(); }

        internal static MidiStateChaseIndex Build(IMidiEventStore events)
        {
            if (events == null) throw new ArgumentNullException("events");
            Builder builder = new Builder();
            for (int i = 0; i < events.Count; i++) builder.Add(events.GetEvent(i), i);
            return builder.Complete(CancellationToken.None);
        }

        internal IList<MidiEvent> CreateMessages(int eventIndexExclusive, ChannelRoutingState routing,
            ChannelOverrideState overrides)
        {
            List<MidiEvent> messages = new List<MidiEvent>();
            if (eventIndexExclusive <= 0) return messages;
            for (int channel = 0; channel < 16; channel++)
            {
                if (routing != null && !routing.IsEnabled(channel)) continue;
                int resetIndex = LatestValue(_resets[channel], eventIndexExclusive, Int32.MinValue);

                AddController(messages, channel, 0, eventIndexExclusive, resetIndex, overrides);
                AddController(messages, channel, 32, eventIndexExclusive, resetIndex, overrides);
                AddSpecial(messages, channel, ProgramSlot, eventIndexExclusive, resetIndex, overrides);
                for (int controller = 1; controller < ControllerCount; controller++)
                {
                    if (controller == 6 || controller == 32 || controller == 38 ||
                        controller == 96 || controller == 97 || controller == 98 ||
                        controller == 99 || controller == 100 || controller == 101) continue;
                    AddController(messages, channel, controller, eventIndexExclusive, resetIndex, overrides);
                }
                AddSpecial(messages, channel, PitchBendSlot, eventIndexExclusive, resetIndex, overrides);
                AddSpecial(messages, channel, ChannelPressureSlot, eventIndexExclusive, resetIndex, overrides);

                ParameterSeries[] parameters = _parameters[channel];
                if (parameters != null)
                {
                    for (int i = 0; i < parameters.Length; i++)
                    {
                        ValueChange change;
                        if (!TryLatest(parameters[i].Values, eventIndexExclusive, out change) ||
                            change.EventIndex <= resetIndex) continue;
                        AddParameter(messages, channel, parameters[i].Key, change.Value);
                    }
                }

                ValueChange selector;
                if (TryLatest(_selectors[channel], eventIndexExclusive, out selector) &&
                    selector.EventIndex > resetIndex)
                    AddSelector(messages, channel, selector.Value);
            }
            return messages;
        }

        private void AddController(List<MidiEvent> messages, int channel, int controller,
            int eventIndexExclusive, int resetIndex, ChannelOverrideState overrides)
        {
            if (IsForcedController(overrides, channel, controller)) return;
            ValueChange change;
            if (!TryLatest(_state[Slot(channel, controller)], eventIndexExclusive, out change)) return;
            if (change.EventIndex <= resetIndex && !PreservedAcrossReset(controller)) return;
            messages.Add(ChannelOverrideState.CreateControllerMessage(channel, controller, change.Value));
        }

        private void AddSpecial(List<MidiEvent> messages, int channel, int slot,
            int eventIndexExclusive, int resetIndex, ChannelOverrideState overrides)
        {
            ChannelAttribute attribute = slot == ProgramSlot ? ChannelAttribute.Program :
                slot == PitchBendSlot ? ChannelAttribute.PitchBend : ChannelAttribute.Aftertouch;
            if (overrides != null && overrides.GetValue(channel, attribute) != ChannelOverrideState.AutoValue) return;
            ValueChange change;
            if (!TryLatest(_state[Slot(channel, slot)], eventIndexExclusive, out change)) return;
            if (slot != ProgramSlot && change.EventIndex <= resetIndex) return;
            messages.Add(ChannelOverrideState.CreateMessage(channel, attribute, change.Value));
        }

        private static bool IsForcedController(ChannelOverrideState overrides, int channel, int controller)
        {
            if (overrides == null) return false;
            ChannelAttribute attribute;
            switch (controller)
            {
                case 0: attribute = ChannelAttribute.BankMsb; break;
                case 32: attribute = ChannelAttribute.BankLsb; break;
                case 7: attribute = ChannelAttribute.Volume; break;
                case 10: attribute = ChannelAttribute.Pan; break;
                case 11: attribute = ChannelAttribute.Expression; break;
                case 64: attribute = ChannelAttribute.Sustain; break;
                default: return false;
            }
            return overrides.GetValue(channel, attribute) != ChannelOverrideState.AutoValue;
        }

        private static bool PreservedAcrossReset(int controller)
        {
            // RP-015 leaves bank select and channel volume outside the reset
            // defaults. Program is stored separately and is also preserved.
            return controller == 0 || controller == 7 || controller == 32;
        }

        private static void AddParameter(List<MidiEvent> messages, int channel, int key, int value)
        {
            AddSelector(messages, channel, key);
            messages.Add(ChannelOverrideState.CreateControllerMessage(channel, 6, (value >> 7) & 0x7F));
            messages.Add(ChannelOverrideState.CreateControllerMessage(channel, 38, value & 0x7F));
        }

        private static void AddSelector(List<MidiEvent> messages, int channel, int encoded)
        {
            if (encoded < 0)
            {
                messages.Add(ChannelOverrideState.CreateControllerMessage(channel, 101, 127));
                messages.Add(ChannelOverrideState.CreateControllerMessage(channel, 100, 127));
                return;
            }
            bool nrpn = (encoded & 0x4000) != 0;
            int parameter = encoded & 0x3FFF;
            messages.Add(ChannelOverrideState.CreateControllerMessage(channel, nrpn ? 99 : 101,
                (parameter >> 7) & 0x7F));
            messages.Add(ChannelOverrideState.CreateControllerMessage(channel, nrpn ? 98 : 100,
                parameter & 0x7F));
        }

        private static int Slot(int channel, int slot) { return channel * SlotsPerChannel + slot; }

        private static int LatestValue(ValueSeries series, int eventIndexExclusive, int fallback)
        {
            ValueChange change;
            return TryLatest(series, eventIndexExclusive, out change) ? change.EventIndex : fallback;
        }

        private static bool TryLatest(ValueSeries series, int eventIndexExclusive, out ValueChange change)
        {
            change = default(ValueChange);
            if (series == null || series.Count == 0) return false;
            int low = 0, high = series.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (series[middle].EventIndex < eventIndexExclusive) low = middle + 1;
                else high = middle;
            }
            if (low == 0) return false;
            change = series[low - 1];
            return true;
        }

        internal sealed class Builder
        {
            private readonly SeriesBuilder[] _state = new SeriesBuilder[16 * SlotsPerChannel];
            private readonly SeriesBuilder[] _resets = new SeriesBuilder[16];
            private readonly SeriesBuilder[] _selectors = new SeriesBuilder[16];
            private readonly Dictionary<int, SeriesBuilder>[] _parameters = new Dictionary<int, SeriesBuilder>[16];
            private readonly Dictionary<int, int>[] _parameterValues = new Dictionary<int, int>[16];
            private readonly int[] _rpnMsb = NewFilled(127);
            private readonly int[] _rpnLsb = NewFilled(127);
            private readonly int[] _nrpnMsb = NewFilled(127);
            private readonly int[] _nrpnLsb = NewFilled(127);
            private readonly int[] _activeSelector = NewFilled(-1);

            internal void Add(MidiEvent midiEvent, int eventIndex)
            { if (midiEvent != null) Add(MidiEventView.FromEvent(midiEvent, eventIndex), eventIndex); }

            internal void Add(MidiEventView midiEvent, int eventIndex)
            {
                if (!midiEvent.IsValid || midiEvent.Channel < 0 || midiEvent.Channel >= 16) return;
                int channel = midiEvent.Channel;
                int status = midiEvent.Status & 0xF0;
                int first = midiEvent.DataLength > 1 ? midiEvent.GetDataByte(1) : 0;
                int second = midiEvent.DataLength > 2 ? midiEvent.GetDataByte(2) : 0;
                if (status == 0xC0) { AddState(channel, ProgramSlot, eventIndex, first); return; }
                if (status == 0xD0) { AddState(channel, ChannelPressureSlot, eventIndex, first); return; }
                if (status == 0xE0) { AddState(channel, PitchBendSlot, eventIndex, ((second << 7) | first) - 8192); return; }
                if (status != 0xB0) return;
                if (first == 121)
                {
                    AddSeries(_resets, channel, new ValueChange(eventIndex, 0));
                    _rpnMsb[channel] = _rpnLsb[channel] = 127;
                    _nrpnMsb[channel] = _nrpnLsb[channel] = 127;
                    _activeSelector[channel] = -1;
                    if (_parameterValues[channel] != null) _parameterValues[channel].Clear();
                    AddSeries(_selectors, channel, new ValueChange(eventIndex, -1));
                    return;
                }
                if (first >= 120) return;
                if (first == 101) { _rpnMsb[channel] = second; Select(channel, false, eventIndex); return; }
                if (first == 100) { _rpnLsb[channel] = second; Select(channel, false, eventIndex); return; }
                if (first == 99) { _nrpnMsb[channel] = second; Select(channel, true, eventIndex); return; }
                if (first == 98) { _nrpnLsb[channel] = second; Select(channel, true, eventIndex); return; }
                if (first == 6 || first == 38 || first == 96 || first == 97)
                { ApplyData(channel, first, second, eventIndex); return; }
                AddState(channel, first, eventIndex, second);
            }

            private void Select(int channel, bool nrpn, int eventIndex)
            {
                int msb = nrpn ? _nrpnMsb[channel] : _rpnMsb[channel];
                int lsb = nrpn ? _nrpnLsb[channel] : _rpnLsb[channel];
                int key = msb == 127 && lsb == 127 ? -1 : (nrpn ? 0x4000 : 0) | (msb << 7) | lsb;
                _activeSelector[channel] = key;
                AddSeries(_selectors, channel, new ValueChange(eventIndex, key));
            }

            private void ApplyData(int channel, int controller, int data, int eventIndex)
            {
                int key = _activeSelector[channel];
                if (key < 0) return;
                Dictionary<int, int> values = _parameterValues[channel];
                if (values == null) _parameterValues[channel] = values = new Dictionary<int, int>();
                int value;
                bool known = values.TryGetValue(key, out value);
                if (controller == 6) value = (data << 7) | (known ? value & 0x7F : 0);
                else if (controller == 38) value = (known ? value & 0x3F80 : 0) | data;
                else
                {
                    if (!known) return;
                    value = Math.Max(0, Math.Min(16383, value + (controller == 96 ? 1 : -1)));
                }
                values[key] = value;
                Dictionary<int, SeriesBuilder> channelParameters = _parameters[channel];
                if (channelParameters == null)
                    _parameters[channel] = channelParameters = new Dictionary<int, SeriesBuilder>();
                SeriesBuilder series;
                if (!channelParameters.TryGetValue(key, out series))
                    channelParameters[key] = series = new SeriesBuilder();
                series.Add(new ValueChange(eventIndex, value));
            }

            private void AddState(int channel, int slot, int eventIndex, int value)
            { AddSeries(_state, Slot(channel, slot), new ValueChange(eventIndex, value)); }

            private static void AddSeries(SeriesBuilder[] array, int slot, ValueChange value)
            {
                SeriesBuilder series = array[slot];
                if (series == null) array[slot] = series = new SeriesBuilder();
                series.Add(value);
            }

            internal MidiStateChaseIndex Complete(CancellationToken cancellationToken)
            {
                long entries = 0;
                ValueSeries[] state = Complete(_state, cancellationToken, ref entries);
                ValueSeries[] resets = Complete(_resets, cancellationToken, ref entries);
                ValueSeries[] selectors = Complete(_selectors, cancellationToken, ref entries);
                ParameterSeries[][] parameters = new ParameterSeries[16][];
                int parameterCount = 0;
                for (int channel = 0; channel < 16; channel++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Dictionary<int, SeriesBuilder> builders = _parameters[channel];
                    if (builders == null || builders.Count == 0) continue;
                    int[] keys = new int[builders.Count]; builders.Keys.CopyTo(keys, 0); Array.Sort(keys);
                    ParameterSeries[] completed = new ParameterSeries[keys.Length];
                    for (int i = 0; i < keys.Length; i++)
                    {
                        ValueSeries values = builders[keys[i]].Complete();
                        entries += values.Count;
                        completed[i] = new ParameterSeries(keys[i], values);
                    }
                    parameters[channel] = completed;
                    parameterCount += completed.Length;
                }
                MidiStateChaseIndex result = new MidiStateChaseIndex(state, resets, selectors, parameters);
                result.EntryCount = entries;
                result.ParameterSeriesCount = parameterCount;
                return result;
            }

            private static ValueSeries[] Complete(SeriesBuilder[] builders, CancellationToken token, ref long entries)
            {
                ValueSeries[] result = new ValueSeries[builders.Length];
                for (int i = 0; i < builders.Length; i++)
                {
                    if ((i & 127) == 0) token.ThrowIfCancellationRequested();
                    if (builders[i] == null) continue;
                    result[i] = builders[i].Complete();
                    entries += result[i].Count;
                }
                return result;
            }

            private static int[] NewFilled(int value)
            { int[] result = new int[16]; for (int i = 0; i < result.Length; i++) result[i] = value; return result; }
        }

        private sealed class SeriesBuilder
        {
            private readonly List<ValueChange[]> _segments = new List<ValueChange[]>();
            private ValueChange[] _current;
            private int _offset, _count;
            internal void Add(ValueChange value)
            {
                if (_current == null || _offset == _current.Length)
                { _current = new ValueChange[SegmentCapacity]; _segments.Add(_current); _offset = 0; }
                _current[_offset++] = value; _count++;
            }
            internal ValueSeries Complete()
            {
                if (_current != null && _offset != _current.Length)
                { ValueChange[] last = new ValueChange[_offset]; Array.Copy(_current, last, _offset); _segments[_segments.Count - 1] = last; }
                return new ValueSeries(_segments.ToArray(), _count);
            }
        }

        private sealed class ValueSeries
        {
            internal readonly ValueChange[][] Segments; internal readonly int Count;
            internal ValueSeries(ValueChange[][] segments, int count) { Segments = segments; Count = count; }
            internal ValueChange this[int index]
            { get { return Segments[index >> 12][index & (SegmentCapacity - 1)]; } }
        }

        private sealed class ParameterSeries
        {
            internal readonly int Key; internal readonly ValueSeries Values;
            internal ParameterSeries(int key, ValueSeries values) { Key = key; Values = values; }
        }

        private struct ValueChange
        {
            internal readonly int EventIndex; internal readonly int Value;
            internal ValueChange(int eventIndex, int value) { EventIndex = eventIndex; Value = value; }
        }
    }
}
