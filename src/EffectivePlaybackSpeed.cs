using System;
using System.Collections.Generic;
using System.Globalization;

namespace MidiBottleneck
{
    internal sealed class EffectivePlaybackSpeed
    {
        public const long InstantaneousWindowMicroseconds = 0;
        public const long DefaultWindowMicroseconds = 250000;
        public const long MinimumCustomWindowMicroseconds = 25000;
        public const long MaximumCustomWindowMicroseconds = 10000000;

        private sealed class Sample
        {
            public long ClockMicroseconds;
            public long FrontierMicroseconds;
            public long ProcessedEvents;
            public long SampleTimeMicroseconds;
        }

        private readonly List<Sample> _samples = new List<Sample>();
        private long _windowMicroseconds = DefaultWindowMicroseconds;

        public long WindowMicroseconds
        {
            get { return _windowMicroseconds; }
            set
            {
                if (!IsValidWindow(value)) throw new ArgumentOutOfRangeException("value");
                if (_windowMicroseconds == value) return;
                _windowMicroseconds = value;
                Reset();
            }
        }

        internal static bool IsValidWindow(long microseconds)
        {
            return microseconds == InstantaneousWindowMicroseconds ||
                (microseconds >= MinimumCustomWindowMicroseconds && microseconds <= MaximumCustomWindowMicroseconds);
        }

        public void Reset() { _samples.Clear(); }

        public double? Add(long playbackClockMicroseconds, long intendedTimelineMicroseconds,
            long outputFrontierMicroseconds, long processedEvents, bool synchronized,
            long sampleTimeMicroseconds)
        {
            long frontier = synchronized ? intendedTimelineMicroseconds : outputFrontierMicroseconds;
            if (_samples.Count > 0)
            {
                Sample last = _samples[_samples.Count - 1];
                if (playbackClockMicroseconds < last.ClockMicroseconds || frontier < last.FrontierMicroseconds ||
                    processedEvents < last.ProcessedEvents || sampleTimeMicroseconds <= last.SampleTimeMicroseconds)
                    Reset();
            }

            Sample current = new Sample
            {
                ClockMicroseconds = playbackClockMicroseconds,
                FrontierMicroseconds = frontier,
                ProcessedEvents = processedEvents,
                SampleTimeMicroseconds = sampleTimeMicroseconds
            };
            _samples.Add(current);

            long retention = _windowMicroseconds == InstantaneousWindowMicroseconds
                ? MaximumCustomWindowMicroseconds : Math.Max(_windowMicroseconds * 2, 500000);
            int remove = 0;
            while (remove + 1 < _samples.Count && sampleTimeMicroseconds - _samples[remove + 1].SampleTimeMicroseconds > retention)
                remove++;
            if (remove > 0) _samples.RemoveRange(0, remove);

            return _windowMicroseconds == InstantaneousWindowMicroseconds
                ? CalculateInstantaneous(current) : CalculateRegression(current);
        }

        private double? CalculateInstantaneous(Sample current)
        {
            for (int i = _samples.Count - 2; i >= 0; i--)
            {
                Sample previous = _samples[i];
                if (previous.FrontierMicroseconds == current.FrontierMicroseconds) continue;
                long clockAdvance = current.ClockMicroseconds - previous.ClockMicroseconds;
                long frontierAdvance = current.FrontierMicroseconds - previous.FrontierMicroseconds;
                if (clockAdvance <= 0 || frontierAdvance <= 0) return null;
                return 100.0 * frontierAdvance / clockAdvance;
            }
            return null;
        }

        private double? CalculateRegression(Sample current)
        {
            long from = current.SampleTimeMicroseconds - _windowMicroseconds;
            int first = 0;
            while (first + 1 < _samples.Count && _samples[first + 1].SampleTimeMicroseconds < from) first++;
            int count = _samples.Count - first;
            long span = current.SampleTimeMicroseconds - _samples[first].SampleTimeMicroseconds;
            long minimumSpan = Math.Min(100000, Math.Max(25000, _windowMicroseconds / 2));
            if (count < 3 || span < minimumSpan) return null;

            double originX = _samples[first].ClockMicroseconds;
            double originY = _samples[first].FrontierMicroseconds;
            double meanX = 0;
            double meanY = 0;
            for (int i = first; i < _samples.Count; i++)
            {
                meanX += _samples[i].ClockMicroseconds - originX;
                meanY += _samples[i].FrontierMicroseconds - originY;
            }
            meanX /= count;
            meanY /= count;
            double covariance = 0;
            double variance = 0;
            for (int i = first; i < _samples.Count; i++)
            {
                double x = (_samples[i].ClockMicroseconds - originX) - meanX;
                double y = (_samples[i].FrontierMicroseconds - originY) - meanY;
                covariance += x * y;
                variance += x * x;
            }
            if (variance <= 0 || Math.Abs(covariance) < 0.0001) return null;
            return 100.0 * covariance / variance;
        }

        internal static string Format(double? percent)
        {
            if (!percent.HasValue || Double.IsNaN(percent.Value) || Double.IsInfinity(percent.Value)) return "—";
            return percent.Value.ToString("N1", CultureInfo.CurrentCulture) + "%";
        }

        internal static string DescribeWindow(long microseconds)
        {
            if (microseconds == InstantaneousWindowMicroseconds) return "Instantaneous";
            if (microseconds >= 1000000) return (microseconds / 1000000.0).ToString("0.###", CultureInfo.CurrentCulture) + " s";
            return (microseconds / 1000.0).ToString("0.###", CultureInfo.CurrentCulture) + " ms";
        }
    }

    internal sealed class RollingOutputRate
    {
        private sealed class Sample { public long Count; public long TimeMicroseconds; }
        private readonly Queue<Sample> _samples = new Queue<Sample>();
        private const long WindowMicroseconds = 250000;
        private double? _maximumObserved;

        public void Reset()
        {
            _samples.Clear();
            _maximumObserved = null;
        }

        public void RestartWindow() { _samples.Clear(); }
        public double? MaximumObserved { get { return _maximumObserved; } }

        public double? Add(long processedEvents, long sampleTimeMicroseconds)
        {
            if (_samples.Count > 0)
            {
                Sample last = null;
                foreach (Sample item in _samples) last = item;
                if (last != null && (processedEvents < last.Count || sampleTimeMicroseconds <= last.TimeMicroseconds)) Reset();
            }
            _samples.Enqueue(new Sample { Count = processedEvents, TimeMicroseconds = sampleTimeMicroseconds });
            while (_samples.Count > 2 && sampleTimeMicroseconds - _samples.Peek().TimeMicroseconds > WindowMicroseconds)
                _samples.Dequeue();
            Sample first = _samples.Peek();
            long elapsed = sampleTimeMicroseconds - first.TimeMicroseconds;
            long count = processedEvents - first.Count;
            if (elapsed < 100000 || count <= 0) return null;
            double rate = count * 1000000.0 / elapsed;
            if (!_maximumObserved.HasValue || rate > _maximumObserved.Value) _maximumObserved = rate;
            return rate;
        }

        internal static string Format(double? eventsPerSecond)
        {
            if (!eventsPerSecond.HasValue || Double.IsNaN(eventsPerSecond.Value) || Double.IsInfinity(eventsPerSecond.Value)) return "—";
            return eventsPerSecond.Value.ToString("N1", CultureInfo.CurrentCulture) + " events/sec";
        }
    }
}
