using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Read-only boundary used by playback and analysis.  The list-backed
    // implementation remains the reference backend while compact backends are
    // introduced incrementally.
    internal interface IMidiEventStore
    {
        int Count { get; }
        MidiEvent GetEvent(int index);
        int LowerBoundByTime(long microseconds);
    }

    internal sealed class ListMidiEventStore : IMidiEventStore
    {
        private readonly List<MidiEvent> _events;

        public ListMidiEventStore(List<MidiEvent> events)
        {
            if (events == null) throw new ArgumentNullException("events");
            _events = events;
        }

        internal List<MidiEvent> Source { get { return _events; } }
        public int Count { get { return _events.Count; } }
        public MidiEvent GetEvent(int index) { return _events[index]; }

        public int LowerBoundByTime(long microseconds)
        {
            int low = 0;
            int high = _events.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (_events[middle].IntendedMicroseconds < microseconds) low = middle + 1;
                else high = middle;
            }
            return low;
        }
    }

    // A cached reader keeps the reference List path concrete in hot loops,
    // avoiding an interface call per event while retaining one store contract.
    internal struct MidiEventReader
    {
        private readonly IMidiEventStore _store;
        private readonly List<MidiEvent> _list;

        internal MidiEventReader(IMidiEventStore store)
        {
            if (store == null) throw new ArgumentNullException("store");
            _store = store;
            ListMidiEventStore listStore = store as ListMidiEventStore;
            _list = listStore == null ? null : listStore.Source;
        }

        public int Count { get { return _list == null ? _store.Count : _list.Count; } }
        public MidiEvent this[int index]
        {
            get { return _list == null ? _store.GetEvent(index) : _list[index]; }
        }

        public int LowerBoundByTime(long microseconds)
        {
            if (_list == null) return _store.LowerBoundByTime(microseconds);
            int low = 0;
            int high = _list.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (_list[middle].IntendedMicroseconds < microseconds) low = middle + 1;
                else high = middle;
            }
            return low;
        }
    }
}
