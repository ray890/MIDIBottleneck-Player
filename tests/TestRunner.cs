using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Forms;

namespace MidiBottleneck.Tests
{
    internal static class TestRunner
    {
        private static int _passed;

        [STAThread]
        private static int Main()
        {
            try
            {
                Run("tempo map, multiple tracks, running status, and SysEx", TestMidiParser);
                Run("FIFO queue accumulation", TestQueueSimulation);
                Run("drop-when-busy decisions", TestDropSimulation);
                Run("zero processing time", TestZeroServiceTime);
                Run("dense 200,000-event MIDI parsing", TestDenseMidiParser);
                Run("Windows MIDI device enumeration", TestMidiDeviceEnumeration);
                Run("WinForms interface construction", TestInterfaceConstruction);
                Console.WriteLine("PASS: " + _passed + " tests");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("FAIL: " + ex.Message);
                return 1;
            }
        }

        private static void Run(string name, Action test)
        {
            test();
            _passed++;
            Console.WriteLine("  OK  " + name);
        }

        private static void TestMidiParser()
        {
            string path = Path.Combine(Path.GetTempPath(), "midi-bottleneck-parser-test-" + Guid.NewGuid().ToString("N") + ".mid");
            try
            {
                File.WriteAllBytes(path, BuildTestMidi());
                MidiSong song = MidiFileParser.Load(path);
                Equal(1, song.Format, "format");
                Equal(2, song.TrackCount, "track count");
                Equal(480, song.TicksPerQuarterNote, "PPQN");
                Equal(6, song.Events.Count, "dispatchable event count");
                Equal(0L, song.Events[0].IntendedMicroseconds, "program time");
                Equal(0L, song.Events[1].IntendedMicroseconds, "first note time");
                Equal(250000L, song.Events[2].IntendedMicroseconds, "running-status note time");
                Equal(500000L, song.Events[3].IntendedMicroseconds, "pre-change note-off time");
                Equal(1500000L, song.Events[4].IntendedMicroseconds, "SysEx time after tempo change");
                Equal(1500000L, song.Events[5].IntendedMicroseconds, "last note-off time");
                Equal(MidiEventKind.NoteOn, song.Events[2].Kind, "running-status kind");
                Equal((byte)0x90, song.Events[2].Data[0], "expanded running status");
                Equal((byte)62, song.Events[2].Data[1], "running-status note");
                Equal(MidiEventKind.SystemExclusive, song.Events[4].Kind, "SysEx kind");
                Equal((byte)0xF0, song.Events[4].Data[0], "SysEx prefix");
                Equal(1500000L, song.DurationMicroseconds, "duration");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestQueueSimulation()
        {
            long[] arrivals = new long[] { 0, 200, 400, 600, 800 };
            SimulationResult result = BottleneckSimulator.Run(arrivals, 500, ProcessingMode.Queue);
            Sequence(new long[] { 500, 1000, 1500, 2000, 2500 }, result.DispatchMicroseconds, "queue dispatches");
            Equal(0, result.DroppedEvents, "queue drops");
            Equal(3, result.MaximumQueueLength, "maximum queue");
        }

        private static void TestDropSimulation()
        {
            long[] arrivals = new long[] { 0, 200, 400, 600, 800 };
            SimulationResult result = BottleneckSimulator.Run(arrivals, 500, ProcessingMode.Drop);
            Sequence(new long[] { 500, 1100 }, result.DispatchMicroseconds, "drop dispatches");
            Equal(3, result.DroppedEvents, "drop count");
        }

        private static void TestZeroServiceTime()
        {
            long[] arrivals = new long[] { 0, 0, 10 };
            SimulationResult queue = BottleneckSimulator.Run(arrivals, 0, ProcessingMode.Queue);
            SimulationResult drop = BottleneckSimulator.Run(arrivals, 0, ProcessingMode.Drop);
            Sequence(arrivals, queue.DispatchMicroseconds, "zero-time queue");
            Sequence(arrivals, drop.DispatchMicroseconds, "zero-time drop");
            Equal(0, drop.DroppedEvents, "zero-time drops");
        }

        private static void TestDenseMidiParser()
        {
            const int eventCount = 200000;
            string path = Path.Combine(Path.GetTempPath(), "midi-bottleneck-dense-test-" + Guid.NewGuid().ToString("N") + ".mid");
            try
            {
                List<byte> track = new List<byte>(eventCount * 3 + 8);
                Add(track, 0x00, 0x90, 0x3C, 0x01);
                for (int i = 1; i < eventCount; i++)
                    Add(track, 0x00, 0x3C, 0x01);
                Add(track, 0x00, 0xFF, 0x2F, 0x00);

                List<byte> bytes = new List<byte>(track.Count + 22);
                AddAscii(bytes, "MThd");
                AddUInt32(bytes, 6);
                AddUInt16(bytes, 0);
                AddUInt16(bytes, 1);
                AddUInt16(bytes, 480);
                AddTrack(bytes, track);
                File.WriteAllBytes(path, bytes.ToArray());

                MidiSong song = MidiFileParser.Load(path);
                Equal(eventCount, song.Events.Count, "dense event count");
                Equal(0L, song.Events[eventCount - 1].IntendedMicroseconds, "dense last timestamp");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestMidiDeviceEnumeration()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            if (devices == null) throw new Exception("device enumeration returned null");
            Console.WriteLine("      Found " + devices.Count + " Windows MIDI output device(s)");
        }

        private static void TestInterfaceConstruction()
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                IntPtr handle = form.Handle;
                if (!form.IsHandleCreated) throw new Exception("main window handle was not created");
                if (handle == IntPtr.Zero) throw new Exception("main window handle is zero");
                if (form.Controls.Count == 0) throw new Exception("main window has no controls");
                Equal("MIDI Event Bottleneck Simulator", form.Text, "window title");
                form.Close();
            }
        }

        private static byte[] BuildTestMidi()
        {
            List<byte> bytes = new List<byte>();
            AddAscii(bytes, "MThd");
            AddUInt32(bytes, 6);
            AddUInt16(bytes, 1);
            AddUInt16(bytes, 2);
            AddUInt16(bytes, 480);

            List<byte> tempoTrack = new List<byte>();
            Add(tempoTrack, 0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);
            Add(tempoTrack, 0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40);
            Add(tempoTrack, 0x00, 0xFF, 0x2F, 0x00);
            AddTrack(bytes, tempoTrack);

            List<byte> noteTrack = new List<byte>();
            Add(noteTrack, 0x00, 0xC0, 0x05);
            Add(noteTrack, 0x00, 0x90, 0x3C, 0x64);
            Add(noteTrack, 0x81, 0x70, 0x3E, 0x64);
            Add(noteTrack, 0x81, 0x70, 0x80, 0x3C, 0x00);
            Add(noteTrack, 0x83, 0x60, 0xF0, 0x03, 0x7D, 0x01, 0xF7);
            Add(noteTrack, 0x00, 0x80, 0x3E, 0x00);
            Add(noteTrack, 0x00, 0xFF, 0x2F, 0x00);
            AddTrack(bytes, noteTrack);
            return bytes.ToArray();
        }

        private static void AddTrack(List<byte> target, List<byte> track)
        {
            AddAscii(target, "MTrk");
            AddUInt32(target, (uint)track.Count);
            target.AddRange(track);
        }

        private static void Add(List<byte> target, params byte[] values)
        {
            target.AddRange(values);
        }

        private static void AddAscii(List<byte> target, string value)
        {
            for (int i = 0; i < value.Length; i++) target.Add((byte)value[i]);
        }

        private static void AddUInt16(List<byte> target, ushort value)
        {
            target.Add((byte)(value >> 8));
            target.Add((byte)value);
        }

        private static void AddUInt32(List<byte> target, uint value)
        {
            target.Add((byte)(value >> 24));
            target.Add((byte)(value >> 16));
            target.Add((byte)(value >> 8));
            target.Add((byte)value);
        }

        private static void Sequence(IList<long> expected, IList<long> actual, string name)
        {
            Equal(expected.Count, actual.Count, name + " count");
            for (int i = 0; i < expected.Count; i++)
                Equal(expected[i], actual[i], name + "[" + i + "]");
        }

        private static void Equal<T>(T expected, T actual, string name)
        {
            if (!EqualityComparer<T>.Default.Equals(expected, actual))
                throw new Exception(name + ": expected " + expected + ", got " + actual);
        }
    }
}
