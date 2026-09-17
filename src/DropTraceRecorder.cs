using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace MidiBottleneck
{
    internal sealed class DropTraceRecorder
    {
        private sealed class Entry
        {
            public int EventIndex;
            public MidiEvent Event;
            public long DecisionMicroseconds;
            public long ServiceStartMicroseconds;
            public long ServiceEndMicroseconds;
            public bool HasService;
            public bool Accepted;
            public string Reason;
            public int ClusterSize;
            public int ConsecutiveDrops;
            public long BusyUntilMicroseconds;
            public int BufferOccupancy;
            public int MaximumBufferOccupancy;
        }

        private readonly object _sync = new object();
        private readonly long _fromMicroseconds;
        private readonly long _toMicroseconds;
        private readonly int _maximumRows;
        private readonly List<Entry> _entries = new List<Entry>();
        private readonly Dictionary<int, Entry> _entriesByEventIndex = new Dictionary<int, Entry>();

        public DropTraceRecorder(long fromMicroseconds, long toMicroseconds, int maximumRows)
        {
            _fromMicroseconds = fromMicroseconds;
            _toMicroseconds = toMicroseconds;
            _maximumRows = maximumRows;
        }

        public void RecordAcceptedAdmission(int eventIndex, MidiEvent midiEvent, long decision, int clusterSize, int bufferOccupancy, int maximumBufferOccupancy, long busyUntil)
        {
            Record(eventIndex, midiEvent, decision, 0, 0, false, true, "accepted into bounded buffer", clusterSize, 0, busyUntil, bufferOccupancy, maximumBufferOccupancy);
        }

        public void RecordService(int eventIndex, long serviceStart, long serviceEnd, long busyUntil)
        {
            lock (_sync)
            {
                Entry entry;
                if (!_entriesByEventIndex.TryGetValue(eventIndex, out entry)) return;
                entry.ServiceStartMicroseconds = serviceStart;
                entry.ServiceEndMicroseconds = serviceEnd;
                entry.HasService = true;
                entry.BusyUntilMicroseconds = busyUntil;
            }
        }

        public void RecordDropped(int eventIndex, MidiEvent midiEvent, long decision, string reason, int clusterSize, int consecutiveDrops, long busyUntil, int bufferOccupancy, int maximumBufferOccupancy)
        {
            Record(eventIndex, midiEvent, decision, 0, 0, false, false, reason, clusterSize, consecutiveDrops, busyUntil, bufferOccupancy, maximumBufferOccupancy);
        }

        private void Record(int eventIndex, MidiEvent midiEvent, long decision, long serviceStart, long serviceEnd, bool hasService, bool accepted, string reason, int clusterSize, int consecutiveDrops, long busyUntil, int bufferOccupancy, int maximumBufferOccupancy)
        {
            if (midiEvent.IntendedMicroseconds < _fromMicroseconds || midiEvent.IntendedMicroseconds > _toMicroseconds)
                return;
            lock (_sync)
            {
                Entry existing;
                if (_entriesByEventIndex.TryGetValue(eventIndex, out existing))
                {
                    existing.DecisionMicroseconds = decision;
                    if (hasService)
                    {
                        existing.ServiceStartMicroseconds = serviceStart;
                        existing.ServiceEndMicroseconds = serviceEnd;
                        existing.HasService = true;
                    }
                    existing.Accepted = accepted;
                    existing.Reason = reason;
                    existing.ClusterSize = clusterSize;
                    existing.ConsecutiveDrops = consecutiveDrops;
                    existing.BusyUntilMicroseconds = busyUntil;
                    existing.BufferOccupancy = bufferOccupancy;
                    existing.MaximumBufferOccupancy = maximumBufferOccupancy;
                    return;
                }
                if (_entries.Count >= _maximumRows) return;
                Entry entry = new Entry();
                entry.EventIndex = eventIndex;
                entry.Event = midiEvent;
                entry.DecisionMicroseconds = decision;
                entry.ServiceStartMicroseconds = serviceStart;
                entry.ServiceEndMicroseconds = serviceEnd;
                entry.HasService = hasService;
                entry.Accepted = accepted;
                entry.Reason = reason;
                entry.ClusterSize = clusterSize;
                entry.ConsecutiveDrops = consecutiveDrops;
                entry.BusyUntilMicroseconds = busyUntil;
                entry.BufferOccupancy = bufferOccupancy;
                entry.MaximumBufferOccupancy = maximumBufferOccupancy;
                _entries.Add(entry);
                _entriesByEventIndex[eventIndex] = entry;
            }
        }

        public int RowCount { get { lock (_sync) return _entries.Count; } }

        public void Save(string path)
        {
            List<Entry> entries;
            lock (_sync) entries = new List<Entry>(_entries);
            entries.Sort(delegate(Entry left, Entry right) { return left.EventIndex.CompareTo(right.EventIndex); });
            StringBuilder text = new StringBuilder();
            text.AppendLine("event_index,intended_us,decision_us,service_start_us,service_end_us,decision,reason,track,event_type,channel,status,data,same_timestamp_cluster_size,consecutive_drops,buffer_occupancy,max_buffer_occupancy,busy_until_us");
            for (int i = 0; i < entries.Count; i++)
            {
                Entry entry = entries[i];
                text.Append(entry.EventIndex).Append(',');
                text.Append(entry.Event.IntendedMicroseconds).Append(',');
                text.Append(entry.DecisionMicroseconds).Append(',');
                if (entry.HasService) text.Append(entry.ServiceStartMicroseconds);
                text.Append(',');
                if (entry.HasService) text.Append(entry.ServiceEndMicroseconds);
                text.Append(',').Append(entry.Accepted ? "accepted" : "dropped").Append(',');
                AppendCsv(text, entry.Reason);
                text.Append(',').Append(entry.Event.Track);
                text.Append(',').Append(entry.Event.Kind);
                text.Append(',').Append(entry.Event.Channel);
                text.Append(",0x").Append(entry.Event.Status.ToString("X2", CultureInfo.InvariantCulture)).Append(',');
                AppendCsv(text, entry.Event.Data.ToHexString());
                text.Append(',').Append(entry.ClusterSize);
                text.Append(',').Append(entry.ConsecutiveDrops);
                text.Append(',').Append(entry.BufferOccupancy);
                text.Append(',').Append(entry.MaximumBufferOccupancy);
                text.Append(',').Append(entry.BusyUntilMicroseconds);
                text.AppendLine();
            }
            File.WriteAllText(path, text.ToString(), new UTF8Encoding(false));
        }

        private static void AppendCsv(StringBuilder text, string value)
        {
            text.Append('"').Append((value ?? String.Empty).Replace("\"", "\"\"")).Append('"');
        }
    }
}
