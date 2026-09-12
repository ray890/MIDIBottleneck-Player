using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum MidiEventKind
    {
        NoteOff,
        NoteOn,
        PolyphonicAftertouch,
        ControlChange,
        ProgramChange,
        ChannelAftertouch,
        PitchBend,
        SystemExclusive,
        SystemMessage
    }

    internal sealed class MidiEvent
    {
        public long AbsoluteTick;
        public long IntendedMicroseconds;
        public int Track;
        public int Order;
        public MidiEventKind Kind;
        public int Channel;
        public byte Status;
        public byte[] Data;
        public int EventIndex;

        public string Description
        {
            get
            {
                if (Channel >= 0)
                    return Kind + " ch " + (Channel + 1);
                return Kind.ToString();
            }
        }
    }

    internal sealed class MidiSong
    {
        public string FilePath;
        public long FileSizeBytes;
        public int Format;
        public int TrackCount;
        public int TicksPerQuarterNote;
        public long NoteCount;
        public List<MidiEvent> Events;
        public long DurationMicroseconds;
    }

    internal enum ProcessingMode
    {
        Queue,
        Drop
    }

    internal enum OverflowPolicy
    {
        DropNewest,
        DropOldest,
        ClearBufferAndCatchUp,
        DropIncomingCompleteNotes
    }

    internal enum PlaybackState
    {
        Stopped,
        Playing,
        Paused,
        Completed
    }

    internal sealed class PlaybackSnapshot
    {
        public PlaybackState State;
        public long ProcessingMicroseconds;
        public long QueueLength;
        public long OutstandingEvents;
        public long MaximumQueueLength;
        public long ProcessedEvents;
        public long DroppedEvents;
        public long PlaybackMicroseconds;
        public long IntendedTimelineMicroseconds;
        public long LastDispatchedTimelineMicroseconds;
        public long CurrentLagMicroseconds;
        public long MaximumLagMicroseconds;
        public ServiceDurationMode ServiceDurationMode;
        public long MidiBitrate;
        public bool SimulateSlowdown;
        public bool QueueLengthLimitEnabled;
        public int QueueLengthLimit;
        public OverflowPolicy OverflowPolicy;
    }

    internal interface IMidiOutput
    {
        void Send(MidiEvent midiEvent);
        void Panic();
        void Reset();
    }

    internal interface IMidiOutputContext
    {
        string SourceFile { get; set; }
    }

    internal static class MidiOutputSafety
    {
        // Reset discards queued/native work. Panic must follow it so the
        // controller messages cannot themselves be discarded by that reset.
        public static void ResetAndSilence(IMidiOutput output)
        {
            if (output == null) return;
            Exception resetFailure = null;
            try { output.Reset(); }
            catch (Exception ex) { resetFailure = ex; }
            try { output.Panic(); }
            catch (Exception panicFailure)
            {
                if (resetFailure == null) throw;
                throw new InvalidOperationException("MIDI reset and post-reset panic both failed.",
                    new AggregateException(resetFailure, panicFailure));
            }
            if (resetFailure != null)
                throw new InvalidOperationException("MIDI reset failed; a post-reset panic was still attempted.", resetFailure);
        }
    }
}
