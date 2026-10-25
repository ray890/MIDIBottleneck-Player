// Deterministic generator for the v2 parity fixtures. First-party code, no
// third-party dependencies. Byte layouts marked "v1 test" are copied from the
// v1.0.44 tests/TestRunner.cs fixtures cited beside them.
//
//   FixtureGenerator <fixture-dir>            bounded committed fixtures
//   FixtureGenerator <fixture-dir> --large    also large/large-8m.mid (not committed)
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

internal static class FixtureGenerator
{
    private static string _dir;
    private static readonly List<string> Manifest = new List<string>();

    private static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: FixtureGenerator <dir> [--large]"); return 2; }
        _dir = args[0];
        Directory.CreateDirectory(_dir);

        // ---- valid files
        Write("f0-all-message-types.mid", AllMessageTypes(), "format 0; every channel message kind, running status, system common/real-time, SysEx, meta");
        Write("f1-v1-test-midi.mid", V1TestMidi(), "v1 test BuildTestMidi (TestRunner.cs:21197): format 1, tempo track, Program, running-status NoteOn, SysEx");
        Write("f1-cross-track-tempo.mid", CrossTrackTempo(), "v1 test (TestRunner.cs:2179): simultaneous tempo changes in two tracks");
        Write("rmid-wrapped.rmi", Rmid(V1TestMidi(), true, true), "v1 test BuildRmid(prefix JUNK, trailing INFO) (TestRunner.cs:21388)");
        Write("rmid-plain-odd.rmi", Rmid(Single(new List<byte> { 0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00 }), false, false), "RMID with odd-length data chunk and pad byte");
        Write("running-status-repair.mid", RunningStatusRepair(), "v1 test (TestRunner.cs:2246): one post-meta running-status repair");
        Write("empty-tracks.mid", EmptyTracks(), "format 1 with zero-length MTrk, end-of-track-only, and meta-only tracks");
        Write("tempo-changes.mid", TempoChanges(), "many tempo changes, ppqn 96, notes between changes");
        Write("tempo-edge-values.mid", TempoEdgeValues(), "tempo 0 ignored, tempo meta of length 2/4 skipped, tempo 1 and 0xFFFFFF");
        Write("decimal-halfway.mid", DecimalHalfway(), "three 1/3-microsecond tempo segments then a half: .NET decimal rounds differently from exact arithmetic");
        Write("decimal-stress.mid", DecimalStress(), "odd ppqn 7, large deltas, frequent tempo changes in several tracks");
        Write("metadata-names.mid", MetadataNames(), "track/instrument names: whitespace, controls, invalid UTF-8 fallback, surrogates, truncation");
        Write("long-sysex-segment-crossing.mid", LongSysEx(), "SysEx payloads crossing the 1 MiB payload segment, plus F7 continuation packets");
        Write("sysex-variants.mid", SysExVariants(), "short (<=3 byte) and long SysEx, F7 escapes, zero-length");
        Write("header-extra-bytes.mid", HeaderExtraBytes(), "MThd length 10 (extra header bytes skipped)");
        Write("many-tracks-ties.mid", ManyTracksTies(), "40 tracks with identical ticks: merge tie order (tick, track, order)");
        Write("many-events-segments.mid", ManyEvents(), "10,000+ events per track crossing 2,048-record segments");
        Write("format0-two-tracks.mid", FormatZeroTwoTracks(), "format 0 header declaring 2 tracks (accepted by v1)");
        Write("sysex-short.mid", SysExShort(), "every short SysEx form (F0 length 0-2, F7 length 1-3) plus boundaries, in two tied tracks");
        Write("metadata-edge.mid", MetadataEdge(), "track/instrument name edge cases: 128-unit cap with spaces, 512-byte cuts, C1 controls, blank-first names");
        Write("index-chase-rich.mid", IndexChaseRich(), "controllers, RPN/NRPN, data entry/increment, resets and ties on all 16 channels (index coverage)");
        Write("models-notes.mid", ModelsNotes(), "model coverage: overlapping same-key notes across tracks, chords, drums, velocity ties, sustain, safety controllers, orphan releases, bursts and gaps");
        Write("models-dense.mid", ModelsDense(), "model coverage: dense overload (roughly 60,000 events/sec) with quiet gaps, ppqn 960");
        Write("routing-lanes.mid", RoutingLanes(), "routing coverage: multi-channel tracks, shared source channels, patch changes while sounding and silent, GM families and banks, dynamic state, sustain, CC120/121/123, same-key overlaps, percussion, names");
        Write("routing-many-lanes.mid", RoutingManyLanes(), "routing coverage: more lanes than destinations with overlapping patches and state (tier merges, omission, capacity errors)");
        Write("routing-exact-merge.mid", RoutingExactMerge(), "routing coverage: a lane compatible with two exact-safe groups joins the one on its own source channel");
        Write("routing-patches-4096.mid", RoutingPatches(4096), "routing coverage: one lane with exactly 4,096 distinct patches (the planner limit)");
        Write("routing-patches-4097.mid", RoutingPatches(4097), "routing coverage: one lane with 4,097 distinct patches (rejected)");

        // ---- strict rejections and malformed input
        Write("err-invalid-running-status.mid", Single(new List<byte> { 0x00, 0x40 }), "v1 test: data byte with no running status");
        Write("err-vlq-too-long.mid", Single(new List<byte> { 0x81, 0x81, 0x81, 0x81, 0x00 }), "v1 test: five-byte variable-length quantity");
        Write("err-truncated-sysex.mid", Single(new List<byte> { 0x00, 0xF0, 0x05, 0x01, 0x02 }), "v1 test: SysEx longer than its track");
        Write("err-post-meta-no-prior-status.mid", Single(new List<byte> { 0x00, 0xFF, 0x59, 0x02, 0x00, 0x00, 0x00, 0x3C, 0x64 }), "v1 test: post-meta data without an earlier channel status");
        Write("err-running-status-after-sysex.mid", Single(new List<byte> { 0x00, 0x90, 0x3C, 0x64, 0x00, 0xF0, 0x01, 0xF7, 0x00, 0x3E, 0x64 }), "v1 test: running status after SysEx stays strict");
        Write("err-riff-wave.rmi", Riff("WAVE", new byte[] { 0x00 }, "data", false), "v1 test: RIFF form is not RMID");
        Write("err-rmid-no-data.rmi", Riff("RMID", new byte[] { 0x01, 0x02, 0x03 }, "JUNK", true), "v1 test: RMID without a data chunk");
        Write("err-rmid-bounded.rmi", Rmid(ShortSmf(), false, true), "v1 test: track cannot read through the RMID data bound");
        Write("err-rmid-empty-data.rmi", Riff("RMID", new byte[0], "data", false), "RMID data chunk of length 0");
        Write("err-riff-size-too-large.rmi", RiffSizeTooLarge(), "RIFF size extends beyond the file");
        Write("err-riff-truncated-header.rmi", Bytes("RIFF", 0x04, 0x00, 0x00, 0x00), "RIFF header shorter than 12 bytes");
        Write("err-format2.mid", Header(2, 1, 480, Track(0x00, 0xFF, 0x2F, 0x00)), "format 2 rejected");
        Write("err-smpte-division.mid", Header(0, 1, 0xE728, Track(0x00, 0xFF, 0x2F, 0x00)), "SMPTE time division rejected");
        Write("err-zero-division.mid", Header(0, 1, 0, Track(0x00, 0xFF, 0x2F, 0x00)), "zero ticks per quarter rejected");
        Write("err-zero-tracks.mid", Header(1, 0, 480), "no tracks rejected");
        Write("err-not-midi.mid", Encoding.ASCII.GetBytes("This is a text file, not MIDI.\n"), "missing MThd");
        Write("err-tiny-file.mid", Encoding.ASCII.GetBytes("MTh"), "file shorter than four bytes");
        Write("err-header-short.mid", HeaderShort(), "MThd length 5");
        Write("err-delta-at-track-end.mid", Single(new List<byte> { 0x00, 0x90, 0x3C, 0x64, 0x00 }), "track ends after a delta time");
        Write("err-unsupported-system-status.mid", Single(new List<byte> { 0x00, 0xF4 }), "undefined system status 0xF4");
        Write("err-status-inside-channel-message.mid", Single(new List<byte> { 0x00, 0x90, 0x3C, 0x90, 0x3C, 0x64 }), "status byte inside a channel message");
        Write("err-missing-track-chunk.mid", Header(1, 2, 480, Track(0x00, 0xFF, 0x2F, 0x00)), "header declares two tracks, file has one");
        Write("err-meta-overrun.mid", Single(new List<byte> { 0x00, 0xFF, 0x01, 0x10, 0x41, 0x42 }), "meta length extends past the track");
        Write("err-wrong-track-id.mid", WrongTrackId(), "second chunk is not MTrk");
        Write("err-header-truncated.mid", Bytes("MThd", 0x00, 0x00, 0x00, 0x06, 0x00), "file ends inside the header fields");
        Write("err-track-length-truncated.mid", Truncated(Header(0, 1, 480, Track(0x00, 0xFF, 0x2F, 0x00)), 16), "file ends inside an MTrk length");
        Write("err-header-extra-truncated.mid", HeaderExtraTruncated(), "declared extra header bytes missing");

        File.WriteAllLines(Path.Combine(_dir, "MANIFEST.txt"), Manifest.ToArray());

        if (args.Length > 1 && args[1] == "--large")
        {
            string large = Path.Combine(_dir, "large");
            Directory.CreateDirectory(large);
            string path = Path.Combine(large, "large-8m.mid");
            File.WriteAllBytes(path, LargeFile(16, 500000));
            Console.WriteLine("wrote " + path + " (" + new FileInfo(path).Length + " bytes)");
            // Routing active-attack limit (TrackRoutePlanner.MaximumActiveAttacks = 1,000,000): exactly at
            // the limit (accepted) and one over it (rejected). Not committed; compared live by parity.ps1.
            foreach (int held in new int[] { 1000000, 1000001 })
            {
                path = Path.Combine(large, "routing-held-" + held + ".mid");
                File.WriteAllBytes(path, RoutingHeld(held));
                Console.WriteLine("wrote " + path + " (" + new FileInfo(path).Length + " bytes)");
            }
        }
        Console.WriteLine("wrote " + Manifest.Count + " fixtures to " + _dir);
        return 0;
    }

    private static void Write(string name, byte[] bytes, string description)
    {
        File.WriteAllBytes(Path.Combine(_dir, name), bytes);
        Manifest.Add(name + "\t" + description);
    }

    // ------------------------------------------------------------ builders

    private static void Add(List<byte> t, params int[] values) { foreach (int v in values) t.Add((byte)v); }
    private static void AddAscii(List<byte> t, string s) { t.AddRange(Encoding.ASCII.GetBytes(s)); }
    private static void AddU16(List<byte> t, int v) { t.Add((byte)(v >> 8)); t.Add((byte)v); }
    private static void AddU32(List<byte> t, uint v) { t.Add((byte)(v >> 24)); t.Add((byte)(v >> 16)); t.Add((byte)(v >> 8)); t.Add((byte)v); }
    private static void AddU32LE(List<byte> t, uint v) { t.Add((byte)v); t.Add((byte)(v >> 8)); t.Add((byte)(v >> 16)); t.Add((byte)(v >> 24)); }
    private static void AddVlq(List<byte> t, long value)
    {
        if (value < 0 || value > 0x0FFFFFFF) throw new ArgumentOutOfRangeException("value");
        int buffer = (int)(value & 0x7F);
        while ((value >>= 7) != 0) buffer = (buffer << 8) | (int)((value & 0x7F) | 0x80);
        while (true) { t.Add((byte)buffer); if ((buffer & 0x80) == 0) break; buffer >>= 8; }
    }
    private static void Meta(List<byte> t, long delta, int type, byte[] data)
    { AddVlq(t, delta); Add(t, 0xFF, type); AddVlq(t, data.Length); t.AddRange(data); }
    private static void Tempo(List<byte> t, long delta, int tempo)
    { AddVlq(t, delta); Add(t, 0xFF, 0x51, 0x03, (tempo >> 16) & 0xFF, (tempo >> 8) & 0xFF, tempo & 0xFF); }
    private static void End(List<byte> t, long delta = 0) { AddVlq(t, delta); Add(t, 0xFF, 0x2F, 0x00); }
    private static List<byte> Track(params int[] values) { List<byte> t = new List<byte>(); Add(t, values); return t; }

    private static byte[] Header(int format, int tracks, int division, params List<byte>[] trackData)
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 6); AddU16(b, format); AddU16(b, tracks); AddU16(b, division);
        foreach (List<byte> t in trackData) { AddAscii(b, "MTrk"); AddU32(b, (uint)t.Count); b.AddRange(t); }
        return b.ToArray();
    }
    private static byte[] Single(List<byte> track) { return Header(0, 1, 480, track); }
    private static byte[] Bytes(string ascii, params int[] values)
    { List<byte> b = new List<byte>(); AddAscii(b, ascii); Add(b, values); return b.ToArray(); }

    private static void AddRiffChunk(List<byte> t, string id, byte[] payload, bool pad)
    {
        AddAscii(t, id); AddU32LE(t, (uint)payload.Length); t.AddRange(payload);
        if (pad && (payload.Length & 1) != 0) t.Add(0);
    }
    private static byte[] Rmid(byte[] smf, bool junk, bool trailing)
    {
        List<byte> body = new List<byte>();
        AddAscii(body, "RMID");
        if (junk) AddRiffChunk(body, "JUNK", new byte[] { 0x11, 0x22, 0x33 }, true);
        AddRiffChunk(body, "data", smf, true);
        if (trailing) AddRiffChunk(body, "INFO", new byte[] { 0x4D, 0x54, 0x68, 0x64 }, true);
        List<byte> r = new List<byte>(); AddAscii(r, "RIFF"); AddU32LE(r, (uint)body.Count); r.AddRange(body);
        return r.ToArray();
    }
    private static byte[] Riff(string form, byte[] payload, string chunkId, bool pad)
    {
        List<byte> body = new List<byte>();
        AddAscii(body, form); AddRiffChunk(body, chunkId, payload, pad);
        List<byte> r = new List<byte>(); AddAscii(r, "RIFF"); AddU32LE(r, (uint)body.Count); r.AddRange(body);
        return r.ToArray();
    }

    // -------------------------------------------------------------- valid

    private static byte[] AllMessageTypes()
    {
        List<byte> t = new List<byte>();
        Meta(t, 0, 0x03, Encoding.ASCII.GetBytes("All types"));
        Meta(t, 0, 0x01, Encoding.ASCII.GetBytes("text meta is skipped"));
        Tempo(t, 0, 600000);
        Add(t, 0x00, 0xC3, 0x10);              // Program, channel 4
        Add(t, 0x00, 0xB3, 0x07, 0x64);        // CC volume
        Add(t, 0x00, 0x00, 0x01);              // running status CC 0 = 1 (bank)
        Add(t, 0x05, 0x93, 0x3C, 0x50);        // NoteOn
        Add(t, 0x00, 0x40, 0x51);              // running-status NoteOn
        Add(t, 0x05, 0xA3, 0x3C, 0x20);        // poly aftertouch
        Add(t, 0x05, 0xD3, 0x30);              // channel aftertouch
        Add(t, 0x00, 0x31);                    // running-status channel aftertouch
        Add(t, 0x05, 0xE3, 0x00, 0x40);        // pitch bend
        Add(t, 0x05, 0x93, 0x3C, 0x00);        // NoteOn velocity 0
        Add(t, 0x00, 0x83, 0x40, 0x40);        // NoteOff
        Add(t, 0x05, 0xF1, 0x23);              // MTC quarter frame
        Add(t, 0x00, 0xF2, 0x10, 0x20);        // song position
        Add(t, 0x00, 0xF3, 0x05);              // song select
        Add(t, 0x00, 0xF6);                    // tune request
        Add(t, 0x00, 0xF8, 0x00, 0xF9, 0x00, 0xFA, 0x00, 0xFB, 0x00, 0xFC, 0x00, 0xFD, 0x00, 0xFE);
        Add(t, 0x05, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7); // GM on (6 bytes stored)
        Add(t, 0x05, 0x9F, 0x24, 0x7F);        // channel 16 NoteOn
        Add(t, 0x05, 0x8F, 0x24, 0x00);
        Add(t, 0x00, 0xB0, 0x79, 0x00);        // reset all controllers
        Add(t, 0x00, 0xB0, 0x78, 0x00);        // all sound off
        End(t, 10);
        return Header(0, 1, 480, t);
    }

    private static byte[] V1TestMidi()
    {
        List<byte> tempo = Track(0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40, 0x00, 0xFF, 0x2F, 0x00);
        List<byte> notes = Track(0x00, 0xC0, 0x05, 0x00, 0x90, 0x3C, 0x64, 0x81, 0x70, 0x3E, 0x64, 0x81, 0x70, 0x80, 0x3C, 0x00,
            0x83, 0x60, 0xF0, 0x03, 0x7D, 0x01, 0xF7, 0x00, 0x80, 0x3E, 0x00, 0x00, 0xFF, 0x2F, 0x00);
        return Header(1, 2, 480, tempo, notes);
    }

    private static byte[] CrossTrackTempo()
    {
        List<byte> t0 = Track(0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x83, 0x60, 0xFF, 0x51, 0x03, 0x06, 0x1A, 0x80, 0x83, 0x60, 0xFF, 0x2F, 0x00);
        List<byte> t1 = Track(0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40, 0x83, 0x60, 0xFF, 0x2F, 0x00);
        List<byte> t2 = Track(0x83, 0x60, 0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00);
        return Header(1, 3, 480, t0, t1, t2);
    }

    private static byte[] RunningStatusRepair()
    {
        return Single(Track(0x00, 0x90, 0x3C, 0x64, 0x00, 0xFF, 0x59, 0x02, 0x00, 0x00, 0x00, 0x3E, 0x64, 0x00, 0x3F, 0x64,
            0x10, 0x80, 0x3C, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x3F, 0x00, 0x00, 0xFF, 0x2F, 0x00));
    }

    private static byte[] ShortSmf()
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 6); AddU16(b, 0); AddU16(b, 1); AddU16(b, 480);
        AddAscii(b, "MTrk"); AddU32(b, 4); Add(b, 0x00, 0x90);
        return b.ToArray();
    }

    private static byte[] EmptyTracks()
    {
        List<byte> meta = new List<byte>();
        Meta(meta, 0, 0x03, Encoding.ASCII.GetBytes("Meta only"));
        Tempo(meta, 100, 400000);
        End(meta, 50);
        List<byte> notes = Track(0x00, 0x90, 0x40, 0x40, 0x60, 0x80, 0x40, 0x00);
        End(notes);
        return Header(1, 4, 96, new List<byte>(), Track(0x00, 0xFF, 0x2F, 0x00), meta, notes);
    }

    private static byte[] TempoChanges()
    {
        List<byte> tempo = new List<byte>();
        int[] values = { 500000, 428571, 333333, 1000000, 250000, 600001, 499999, 123457, 777777, 500000 };
        for (int i = 0; i < 40; i++) Tempo(tempo, i == 0 ? 0 : 37 + i, values[i % values.Length] + i);
        End(tempo);
        List<byte> notes = new List<byte>();
        for (int i = 0; i < 200; i++)
        {
            AddVlq(notes, i == 0 ? 0 : 5 + (i % 13));
            Add(notes, 0x90, 0x30 + (i % 24), 0x40 + (i % 32));
            AddVlq(notes, 3); Add(notes, 0x80, 0x30 + (i % 24), 0x00);
        }
        End(notes);
        return Header(1, 2, 96, tempo, notes);
    }

    private static byte[] TempoEdgeValues()
    {
        List<byte> t = new List<byte>();
        Tempo(t, 0, 0);                                         // ignored (tempo 0)
        AddVlq(t, 0); Add(t, 0xFF, 0x51, 0x02, 0x07, 0xA1);     // length 2: skipped
        AddVlq(t, 0); Add(t, 0xFF, 0x51, 0x04, 0x07, 0xA1, 0x20, 0x00); // length 4: skipped
        Add(t, 0x00, 0x90, 0x3C, 0x64);
        Tempo(t, 10, 1);
        Add(t, 0x0A, 0x80, 0x3C, 0x00);
        Tempo(t, 0, 0xFFFFFF);
        Add(t, 0x81, 0x00, 0x90, 0x3E, 0x64);
        Add(t, 0x83, 0x60, 0x80, 0x3E, 0x00);
        End(t);
        return Header(0, 1, 120, t);
    }

    // ppqn 6, tempo 2: each one-tick segment contributes exactly 1/3 us, which
    // .NET decimal stores as 0.3333...3 (28 digits). After three segments the
    // decimal sum is 0.9999...9 instead of 1. A following tempo-3 tick adds
    // exactly 0.5, so the exact value 1.5 rounds (away from zero) to 2 while
    // the decimal value 1.4999...9 rounds to 1.
    private static byte[] DecimalHalfway()
    {
        List<byte> t = new List<byte>();
        Tempo(t, 0, 2);
        Tempo(t, 1, 2);
        Tempo(t, 1, 2);
        Tempo(t, 1, 3);
        Add(t, 0x01, 0x90, 0x3C, 0x64);   // tick 4
        Add(t, 0x01, 0x80, 0x3C, 0x00);   // tick 5
        Tempo(t, 1, 500000);
        Add(t, 0x07, 0x90, 0x3E, 0x64);
        Add(t, 0x03, 0x80, 0x3E, 0x00);
        End(t);
        return Header(0, 1, 6, t);
    }

    private static uint _state = 0x2545F491;
    private static uint Next()
    {
        uint x = _state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; _state = x; return x;
    }

    private static byte[] DecimalStress()
    {
        _state = 0x2545F491;
        List<byte>[] tracks = new List<byte>[4];
        for (int track = 0; track < tracks.Length; track++)
        {
            List<byte> t = new List<byte>();
            for (int i = 0; i < 300; i++)
            {
                uint r = Next();
                uint pick = r % 5;
                long delta = pick == 0 ? 0 : pick == 1 ? 1 + (Next() % 7) : pick == 2 ? Next() % 1000
                    : pick == 3 ? Next() % 0x0FFFFFFF : 0x0FFFFFF0 + (Next() % 16);
                AddVlq(t, delta);
                if ((Next() % 3) == 0 || track == 0)
                {
                    int tempo = (int)(1 + Next() % 0xFFFFFF);
                    Add(t, 0xFF, 0x51, 0x03, (tempo >> 16) & 0xFF, (tempo >> 8) & 0xFF, tempo & 0xFF);
                }
                else Add(t, 0x90 | track, 0x20 + (int)(Next() % 64), 1 + (int)(Next() % 127));
            }
            End(t);
            tracks[track] = t;
        }
        return Header(1, tracks.Length, 7, tracks);
    }

    private static byte[] MetadataNames()
    {
        List<List<byte>> tracks = new List<List<byte>>();
        Func<string, byte[]> utf8 = s => new UTF8Encoding(false).GetBytes(s);
        Action<List<byte>> notes = t => { Add(t, 0x00, 0x90, 0x3C, 0x40, 0x10, 0x80, 0x3C, 0x00); End(t); };

        List<byte> t0 = new List<byte>(); // whitespace collapse, control as separator
        Meta(t0, 0, 0x03, utf8("  Lead \t  Piano  \r\n"));
        Meta(t0, 0, 0x04, new byte[] { 0x41, 0x63, 0x6F, 0x75, 0x01, 0x47, 0x72, 0x61, 0x6E, 0x64 });
        notes(t0); tracks.Add(t0);

        List<byte> t1 = new List<byte>(); // invalid UTF-8 -> byte fallback; valid multi-byte instrument
        Meta(t1, 0, 0x03, new byte[] { 0x43, 0x61, 0x66, 0xE9, 0x20, 0xC3, 0x28 });
        Meta(t1, 0, 0x04, utf8("Été 日本"));
        notes(t1); tracks.Add(t1);

        List<byte> t2 = new List<byte>(); // surrogate pair split by the 128-unit cap
        Meta(t2, 0, 0x03, utf8(new string('a', 127) + "\U0001F600\U0001F600"));
        Meta(t2, 0, 0x04, utf8("\U0001F3B9 keys"));
        notes(t2); tracks.Add(t2);

        List<byte> t3 = new List<byte>(); // > 512 bytes retained as 512
        Meta(t3, 0, 0x03, utf8(new string('x', 600)));
        Meta(t3, 0, 0x04, utf8(new string('y', 128)));
        notes(t3); tracks.Add(t3);

        List<byte> t4 = new List<byte>(); // multi-byte sequence cut at byte 512 -> whole-string fallback
        StringBuilder cut = new StringBuilder();
        for (int i = 0; i < 255; i++) cut.Append('é');
        cut.Append('a').Append('é').Append("tail");
        Meta(t4, 0, 0x03, utf8(cut.ToString()));
        Meta(t4, 0, 0x04, utf8(new string('z', 129)));
        notes(t4); tracks.Add(t4);

        List<byte> t5 = new List<byte>(); // blank first name does not stick; first instrument wins
        Meta(t5, 0, 0x03, utf8("   "));
        Meta(t5, 0, 0x04, utf8("Strings"));
        Meta(t5, 0, 0x03, utf8("Second Name"));
        Meta(t5, 0, 0x04, utf8("Ignored"));
        Meta(t5, 0, 0x03, utf8("Third Ignored"));
        notes(t5); tracks.Add(t5);

        List<byte> t6 = new List<byte>(); // Unicode whitespace/control classification
        int[] separators = { 0xFEFF, 0x00A0, 0x2028, 0x180E, 0x0085, 0x1680, 0x2000, 0x200A, 0x202F, 0x205F, 0x3000, 0x2029, 0x200B, 0x0009 };
        StringBuilder classified = new StringBuilder();
        for (int i = 0; i < separators.Length; i++) classified.Append((char)separators[i]).Append((char)('A' + i));
        Meta(t6, 0, 0x03, utf8(classified.ToString()));
        Meta(t6, 0, 0x04, new byte[0]);
        notes(t6); tracks.Add(t6);

        List<byte> t7 = new List<byte>(); // quoting
        Meta(t7, 0, 0x03, utf8("He said \"hi\" \\ ok"));
        Meta(t7, 0, 0x04, new byte[] { 0xC0, 0xAF, 0x41 });          // overlong -> fallback
        notes(t7); tracks.Add(t7);

        List<byte> t8 = new List<byte>();
        Meta(t8, 0, 0x03, new byte[] { 0x53, 0xED, 0xA0, 0x80, 0x54 }); // encoded surrogate -> fallback
        Meta(t8, 0, 0x04, new byte[] { 0xF4, 0x90, 0x80, 0x80 });       // > U+10FFFF -> fallback
        notes(t8); tracks.Add(t8);

        List<byte> t9 = new List<byte>(); // exactly 512 and 513 bytes
        Meta(t9, 0, 0x03, utf8(new string('n', 512)));
        Meta(t9, 0, 0x04, utf8(new string('i', 513)));
        notes(t9); tracks.Add(t9);

        List<byte> t10 = new List<byte>(); // no names at all, plus noncharacter and BOM-only text
        Meta(t10, 0, 0x01, utf8("not a name"));
        notes(t10); tracks.Add(t10);

        List<byte> t11 = new List<byte>();
        Meta(t11, 0, 0x03, utf8("￾￿ end"));
        Meta(t11, 0, 0x04, utf8("﻿"));
        notes(t11); tracks.Add(t11);

        return Header(1, tracks.Count, 480, tracks.ToArray());
    }

    private static byte[] LongSysEx()
    {
        List<byte> t = new List<byte>();
        int first = 700000, second = 500000;
        AddVlq(t, 0); Add(t, 0xF0); AddVlq(t, first);
        for (int i = 0; i < first - 1; i++) t.Add((byte)(i & 0x7F));
        t.Add(0xF7);
        Add(t, 0x10, 0x90, 0x3C, 0x64);
        AddVlq(t, 0); Add(t, 0xF0); AddVlq(t, second);           // packet without terminating F7
        for (int i = 0; i < second; i++) t.Add((byte)((i * 7) & 0x7F));
        AddVlq(t, 1); Add(t, 0xF7); AddVlq(t, 9);                // continuation
        Add(t, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xF7);
        Add(t, 0x10, 0x80, 0x3C, 0x00);
        End(t);
        return Single(t);
    }

    private static byte[] SysExVariants()
    {
        List<byte> t = new List<byte>();
        Add(t, 0x00, 0xF0, 0x00);                          // zero-length F0: stored length 1
        Add(t, 0x01, 0xF0, 0x01, 0xF7);                    // F0 F7: stored length 2
        Add(t, 0x01, 0xF0, 0x02, 0x43, 0xF7);              // stored length 3
        Add(t, 0x01, 0xF0, 0x03, 0x43, 0x10, 0xF7);        // stored length 4 (long)
        Add(t, 0x01, 0xF7, 0x00);                          // zero-length F7 escape
        Add(t, 0x01, 0xF7, 0x01, 0xF8);                    // escaped real-time byte
        Add(t, 0x01, 0xF7, 0x03, 0x90, 0x3C, 0x64);        // escaped raw message (length 3)
        Add(t, 0x01, 0xF7, 0x05, 0xF0, 0x41, 0x10, 0x42, 0xF7); // escaped full SysEx (length 5)
        Add(t, 0x01, 0x90, 0x3C, 0x64, 0x05, 0x80, 0x3C, 0x00);
        End(t);
        return Single(t);
    }

    private static byte[] HeaderExtraBytes()
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 10); AddU16(b, 0); AddU16(b, 1); AddU16(b, 240); Add(b, 0xDE, 0xAD, 0xBE, 0xEF);
        List<byte> t = Track(0x00, 0x90, 0x3C, 0x64, 0x78, 0x80, 0x3C, 0x00); End(t);
        AddAscii(b, "MTrk"); AddU32(b, (uint)t.Count); b.AddRange(t);
        return b.ToArray();
    }

    private static byte[] ManyTracksTies()
    {
        List<byte>[] tracks = new List<byte>[40];
        for (int track = 0; track < tracks.Length; track++)
        {
            List<byte> t = new List<byte>();
            if (track % 7 == 3) Tempo(t, 0, 400000 + track * 1000);
            for (int i = 0; i < 12; i++)
            {
                AddVlq(t, i == 0 ? 0 : (i % 3 == 0 ? 0 : 24));
                Add(t, 0x90 | (track % 16), 0x30 + (track % 40), 1 + ((track + i) % 127));
                if ((i + track) % 5 == 0) { AddVlq(t, 0); Add(t, 0xB0 | (track % 16), 0x07, track); }
                if (i == 6 && track % 7 == 3) Tempo(t, 0, 600000 - track * 1000);
            }
            End(t, 24);
            tracks[track] = t;
        }
        return Header(1, tracks.Length, 24, tracks);
    }

    private static byte[] ManyEvents()
    {
        List<byte>[] tracks = new List<byte>[2];
        for (int track = 0; track < 2; track++)
        {
            List<byte> t = new List<byte>();
            Add(t, 0x00, 0x90 | track, 0x3C, 0x40);
            for (int i = 1; i < 10500; i++)
            {
                AddVlq(t, (i * (track + 3)) % 4);
                Add(t, 0x3C + (i % 12), (i % 3) == 0 ? 0x00 : 0x40);
            }
            End(t);
            tracks[track] = t;
        }
        return Header(1, 2, 480, tracks);
    }

    private static byte[] FormatZeroTwoTracks()
    {
        List<byte> a = Track(0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00); End(a);
        List<byte> b = Track(0x30, 0x91, 0x40, 0x64, 0x60, 0x81, 0x40, 0x00); End(b);
        return Header(0, 2, 96, a, b);
    }

    // v1.0.44 stores SysEx of 1-3 stored bytes as zero bytes (improvement
    // DD-001 in v2/docs/DIFFERENCES_FROM_V1.md).
    private static byte[] SysExShort()
    {
        List<byte> a = new List<byte>();
        Add(a, 0x00, 0xF0, 0x00);                          // F0 length 0: stored F0
        Add(a, 0x01, 0xF0, 0x01, 0xF7);                    // F0 length 1: stored F0 F7
        Add(a, 0x01, 0xF0, 0x01, 0x7E);                    // F0 length 1, unterminated
        Add(a, 0x01, 0xF0, 0x02, 0x43, 0xF7);              // F0 length 2: stored F0 43 F7
        Add(a, 0x01, 0xF0, 0x02, 0x41, 0x42);              // F0 length 2, unterminated
        Add(a, 0x01, 0xF7, 0x01, 0xF8);                    // F7 length 1
        Add(a, 0x01, 0xF7, 0x01, 0xF7);                    // F7 length 1: lone terminator packet
        Add(a, 0x01, 0xF7, 0x02, 0x7F, 0xF7);              // F7 length 2
        Add(a, 0x01, 0xF7, 0x03, 0x90, 0x3C, 0x64);        // F7 length 3
        Add(a, 0x01, 0xF7, 0x03, 0x01, 0x02, 0xF7);        // F7 length 3 continuation end
        Add(a, 0x01, 0xF0, 0x03, 0x01, 0x02, 0xF7);        // boundary: F0 length 3 (stored 4, long)
        Add(a, 0x01, 0xF7, 0x00);                          // boundary: F7 length 0
        Add(a, 0x01, 0xF7, 0x04, 0x01, 0x02, 0x03, 0xF7);  // boundary: F7 length 4 (long)
        Add(a, 0x01, 0x90, 0x3C, 0x64, 0x01, 0x80, 0x3C, 0x00);
        End(a);
        List<byte> b = new List<byte>();
        Add(b, 0x00, 0x91, 0x40, 0x50);                    // ties with track 0 at every tick
        Add(b, 0x01, 0xF0, 0x01, 0xF7);
        Add(b, 0x01, 0xF7, 0x02, 0x10, 0x20);
        Add(b, 0x01, 0xF0, 0x00);
        Add(b, 0x01, 0xF7, 0x03, 0x7F, 0x7F, 0x7F);
        Add(b, 0x05, 0x81, 0x40, 0x00);
        End(b);
        return Header(1, 2, 96, a, b);
    }

    private static byte[] MetadataEdge()
    {
        List<List<byte>> tracks = new List<List<byte>>();
        Func<string, byte[]> utf8 = s => new UTF8Encoding(false).GetBytes(s);
        Action<List<byte>> notes = t => { Add(t, 0x00, 0x90, 0x3C, 0x40, 0x10, 0x80, 0x3C, 0x00); End(t); };

        List<byte> t0 = new List<byte>(); // 128 units then trailing spaces: not truncated
        Meta(t0, 0, 0x03, utf8(new string('a', 128) + "   "));
        Meta(t0, 0, 0x04, utf8(new string('b', 128) + " c"));           // space not kept, truncated
        notes(t0); tracks.Add(t0);

        List<byte> t1 = new List<byte>(); // 127 units, a separator, then text: ends with a space
        Meta(t1, 0, 0x03, utf8(new string('d', 127) + " e"));
        Meta(t1, 0, 0x04, utf8(new string('f', 126) + "\U0001F600"));    // pair fits exactly
        notes(t1); tracks.Add(t1);

        List<byte> t2 = new List<byte>(); // 3-byte and 4-byte sequences cut at byte 512
        StringBuilder three = new StringBuilder();
        for (int i = 0; i < 170; i++) three.Append('日');                   // 510 bytes
        three.Append('日');                                                  // bytes 511-513: cut
        Meta(t2, 0, 0x03, utf8(three.ToString()));
        StringBuilder four = new StringBuilder();
        for (int i = 0; i < 127; i++) four.Append("\U0001F3B9");                 // 508 bytes
        four.Append("\U0001F3B9");                                               // bytes 509-512: fits
        four.Append("\U0001F3B9");                                               // beyond 512: dropped
        Meta(t2, 0, 0x04, utf8(four.ToString()));
        notes(t2); tracks.Add(t2);

        List<byte> t3 = new List<byte>(); // C1 controls via valid UTF-8, DEL, NUL, latin-1 fallback bytes
        Meta(t3, 0, 0x03, new byte[] { 0x41, 0xC2, 0x80, 0x42, 0xC2, 0x9F, 0x43, 0x7F, 0x44, 0x00, 0x45 });
        Meta(t3, 0, 0x04, new byte[] { 0xFE, 0xFF, 0x80, 0x9F, 0xA0, 0x41, 0x85, 0x42 });
        notes(t3); tracks.Add(t3);

        List<byte> t4 = new List<byte>(); // names after events; empty and control-only names do not stick
        Add(t4, 0x00, 0x92, 0x30, 0x40);
        Meta(t4, 0, 0x03, new byte[0]);
        Meta(t4, 0, 0x03, new byte[] { 0x01, 0x02, 0x1F });
        Meta(t4, 0, 0x04, utf8(" 　 "));
        Meta(t4, 5, 0x03, utf8("Late Name"));
        Meta(t4, 0, 0x04, utf8("Late Instrument"));
        Add(t4, 0x10, 0x82, 0x30, 0x00);
        End(t4); tracks.Add(t4);

        List<byte> t5 = new List<byte>(); // 513 bytes whose first 512 are whitespace: empty, so a later name wins
        Meta(t5, 0, 0x03, utf8(new string(' ', 512) + "x"));
        Meta(t5, 0, 0x03, utf8("After Blank"));
        Meta(t5, 0, 0x04, utf8(new string('\t', 600)));
        Meta(t5, 0, 0x04, utf8("Inst"));
        notes(t5); tracks.Add(t5);

        List<byte> t6 = new List<byte>(); // lone surrogates in fallback text are impossible; lone continuation bytes
        Meta(t6, 0, 0x03, new byte[] { 0x80, 0x41, 0xBF });
        Meta(t6, 0, 0x04, new byte[] { 0xE2, 0x82 });                            // truncated 3-byte sequence
        notes(t6); tracks.Add(t6);

        List<byte> t7 = new List<byte>(); // meta-only track with names, no events
        Meta(t7, 0, 0x03, utf8("Only Meta"));
        Meta(t7, 0, 0x04, utf8(" Lead Synth "));
        End(t7); tracks.Add(t7);

        return Header(1, tracks.Count, 480, tracks.ToArray());
    }

    // Index coverage: every attribute the source-value and chase indexes record,
    // on all 16 channels, with RPN/NRPN selection, data entry, increment and
    // decrement (known, unknown and clamped), Reset All Controllers, ignored
    // channel-mode controllers, and same-tick ties across four tracks.
    private static byte[] IndexChaseRich()
    {
        List<byte>[] tracks = new List<byte>[4];
        for (int i = 0; i < tracks.Length; i++) tracks[i] = new List<byte>();

        // Track 0: scripted sequences on channels 0-3.
        List<byte> s = tracks[0];
        Add(s, 0x00, 0xB0, 0x00, 0x05, 0x00, 0xB0, 0x20, 0x02, 0x00, 0xC0, 0x11);      // bank 5/2, program 17
        Add(s, 0x00, 0xB0, 0x07, 0x64, 0x00, 0xB0, 0x0A, 0x20, 0x00, 0xB0, 0x0B, 0x50); // volume, pan, expression
        Add(s, 0x00, 0xB0, 0x40, 0x3F, 0x01, 0xB0, 0x40, 0x40);                         // sustain below / at 64
        Add(s, 0x00, 0xE0, 0x00, 0x40, 0x01, 0xE0, 0x7F, 0x7F, 0x01, 0xE0, 0x00, 0x00); // bend centre, max, min
        Add(s, 0x00, 0xD0, 0x22, 0x00, 0xA0, 0x3C, 0x10);                               // channel and poly pressure
        Add(s, 0x01, 0xB0, 0x60, 0x01);                                                 // increment with no selector
        Add(s, 0x01, 0xB0, 0x65, 0x00, 0x00, 0xB0, 0x64, 0x00);                         // RPN 0/0
        Add(s, 0x00, 0xB0, 0x60, 0x01);                                                 // increment, value unknown
        Add(s, 0x00, 0xB0, 0x06, 0x02, 0x00, 0xB0, 0x26, 0x05);                         // data entry MSB, LSB
        Add(s, 0x01, 0xB0, 0x60, 0x01, 0x00, 0xB0, 0x61, 0x01, 0x00, 0xB0, 0x61, 0x01); // inc, dec, dec
        Add(s, 0x01, 0xB0, 0x63, 0x01, 0x00, 0xB0, 0x62, 0x02);                         // NRPN 1/2
        Add(s, 0x00, 0xB0, 0x26, 0x7F, 0x00, 0xB0, 0x06, 0x7F);                         // LSB first, then MSB
        Add(s, 0x00, 0xB0, 0x60, 0x00, 0x00, 0xB0, 0x60, 0x00);                         // clamp at 16383
        Add(s, 0x01, 0xB0, 0x65, 0x7F, 0x00, 0xB0, 0x64, 0x7F);                         // null RPN
        Add(s, 0x00, 0xB0, 0x06, 0x10);                                                 // data with null selector
        Add(s, 0x01, 0xB0, 0x65, 0x00, 0x00, 0xB0, 0x64, 0x01, 0x00, 0xB0, 0x26, 0x01); // RPN 0/1, LSB only
        Add(s, 0x00, 0xB0, 0x61, 0x00, 0x00, 0xB0, 0x61, 0x00);                         // decrement to clamp 0
        Add(s, 0x00, 0xB0, 0x01, 0x30, 0x00, 0xB0, 0x5B, 0x28, 0x00, 0xB0, 0x77, 0x01); // mod, reverb, CC119
        Add(s, 0x01, 0xB0, 0x78, 0x00, 0x00, 0xB0, 0x7A, 0x7F, 0x00, 0xB0, 0x7B, 0x00); // CC120, 122, 123 (ignored)
        Add(s, 0x00, 0xB0, 0x7C, 0x00, 0x00, 0xB0, 0x7F, 0x00);                         // CC124, CC127 (ignored)
        Add(s, 0x01, 0xB0, 0x79, 0x00);                                                 // Reset All Controllers
        Add(s, 0x00, 0xB0, 0x06, 0x05, 0x00, 0xB0, 0x60, 0x00);                         // data after reset: no selector
        Add(s, 0x01, 0xB0, 0x0B, 0x40, 0x00, 0xE0, 0x10, 0x30, 0x00, 0xC0, 0x12);       // after reset
        Add(s, 0x01, 0xB0, 0x63, 0x05, 0x00, 0xB0, 0x06, 0x09);                         // NRPN MSB only (LSB still 127)
        Add(s, 0x00, 0xB3, 0x79, 0x00, 0x00, 0xB3, 0x07, 0x10, 0x00, 0xC3, 0x01);       // channel 3 reset first
        End(s, 1);

        // Tracks 1-3: seeded mixtures on every channel with shared ticks.
        _state = 0x7F4A7C15;
        for (int t = 1; t < tracks.Length; t++)
        {
            List<byte> track = tracks[t];
            for (int i = 0; i < 400; i++)
            {
                uint r = Next();
                AddVlq(track, (r & 3) == 0 ? 0 : 1 + (r >> 2) % 3);
                int channel = (int)((r >> 8) % 16);
                int pick = (int)((r >> 12) % 20);
                int v1 = (int)((r >> 17) & 0x7F), v2 = (int)((r >> 24) & 0x7F);
                int[] controllers = { 0, 32, 7, 10, 11, 64, 1, 2, 74, 91, 93, 121, 101, 100, 99, 98, 6, 38, 96, 97, 120, 123 };
                if (pick < 11) Add(track, 0xB0 | channel, controllers[(int)((r >> 20) % (uint)controllers.Length)], v2);
                else if (pick < 13) Add(track, 0xC0 | channel, v1);
                else if (pick < 15) Add(track, 0xE0 | channel, v1, v2);
                else if (pick < 16) Add(track, 0xD0 | channel, v1);
                else if (pick < 17) Add(track, 0xA0 | channel, v1, v2);
                else Add(track, 0x90 | channel, v1, v2);
            }
            End(track, 1);
        }
        return Header(1, tracks.Length, 96, tracks);
    }

    // Absolute-time track builder: events at equal ticks keep insertion order.
    private sealed class TimedTrack
    {
        private readonly List<long> _ticks = new List<long>();
        private readonly List<int[]> _bytes = new List<int[]>();
        internal void At(long tick, params int[] bytes) { _ticks.Add(tick); _bytes.Add(bytes); }
        internal List<byte> Build()
        {
            int[] order = new int[_ticks.Count];
            for (int i = 0; i < order.Length; i++) order[i] = i;
            Array.Sort(order, (a, b) => _ticks[a] != _ticks[b] ? _ticks[a].CompareTo(_ticks[b]) : a.CompareTo(b));
            List<byte> t = new List<byte>();
            long last = 0;
            foreach (int i in order)
            {
                AddVlq(t, _ticks[i] - last);
                last = _ticks[i];
                Add(t, _bytes[i]);
            }
            End(t, 1);
            return t;
        }
    }

    private static void Note(TimedTrack track, long tick, int channel, int pitch, int velocity, long duration, bool offAsZeroVelocity, int offVelocity)
    {
        track.At(tick, 0x90 | channel, pitch, velocity);
        if (offAsZeroVelocity) track.At(tick + duration, 0x90 | channel, pitch, 0);
        else track.At(tick + duration, 0x80 | channel, pitch, offVelocity);
    }

    private static byte[] ModelsNotes()
    {
        _state = 0x1B873593;
        TimedTrack tempo = new TimedTrack();
        tempo.At(0, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);     // 500,000 us per quarter
        tempo.At(960, 0xFF, 0x51, 0x03, 0x06, 0x1A, 0x80);   // 400,000
        TimedTrack[] tracks = new TimedTrack[6];
        tracks[0] = tempo;
        for (int t = 1; t < tracks.Length; t++) tracks[t] = new TimedTrack();
        int[] velocities = { 1, 64, 64, 100, 127, 100 };
        for (int t = 1; t < tracks.Length; t++)
        {
            long tick = 0;
            for (int i = 0; i < 260; i++)
            {
                uint r = Next();
                tick += (r & 3) == 0 ? 0 : (r >> 2) % 12;
                if (i == 120) tick += 800;                      // quiet gap (pacer capacity is not banked)
                int channel = t == 1 ? 0 : t == 2 ? (int)((r >> 6) & 1) : t == 3 ? 9 : (int)((r >> 6) % 16);
                int pitch = 48 + (int)((r >> 10) % 24);
                int velocity = velocities[(r >> 15) % (uint)velocities.Length];
                long duration = ((r >> 18) & 15) == 0 ? 0 : 1 + (r >> 18) % 40;
                bool zeroOff = ((r >> 23) & 1) != 0;
                int pick = (int)((r >> 24) % 32);
                if (pick < 20) Note(tracks[t], tick, channel, pitch, velocity, duration, zeroOff, (int)((r >> 5) & 0x7F));
                else if (pick < 22)
                {
                    int size = 3 + (int)(r % 4);                // chord at one tick
                    for (int k = 0; k < size; k++)
                        Note(tracks[t], tick, channel, pitch + k * 4 - 6, velocities[(r + (uint)k) % (uint)velocities.Length], duration + k, zeroOff, 0);
                }
                else if (pick == 22) tracks[t].At(tick, 0x80 | channel, pitch, 0);             // orphan release
                else if (pick == 23) tracks[t].At(tick, 0x90 | channel, pitch, velocity);       // never released
                else if (pick == 24) tracks[t].At(tick, 0xB0 | channel, 64, (r & 0x100) != 0 ? 127 : 0);
                else if (pick == 25) tracks[t].At(tick, 0xB0 | channel, 64, (r & 0x100) != 0 ? 64 : 63);
                else if (pick == 26) tracks[t].At(tick, 0xB0 | channel, 120 + (int)(r % 8), 0); // safety / channel mode
                else if (pick == 27) tracks[t].At(tick, 0xC0 | channel, (int)(r & 0x7F));
                else if (pick == 28) tracks[t].At(tick, 0xE0 | channel, (int)(r & 0x7F), (int)((r >> 7) & 0x7F));
                else if (pick == 29) tracks[t].At(tick, 0xB0 | channel, 7, (int)(r & 0x7F));
                else if (pick == 30) tracks[t].At(tick, 0xA0 | channel, pitch, (int)(r & 0x7F));
                else tracks[t].At(tick, 0xD0 | channel, (int)(r & 0x7F));
            }
        }
        // A same-tick burst over many channels and pitches, and one long SysEx.
        for (int k = 0; k < 48; k++) Note(tracks[4], 2000, k % 16, 30 + k, velocities[k % velocities.Length], 5 + k % 3, false, 0);
        tracks[5].At(2000, 0xF0, 0x06, 0x7E, 0x7F, 0x09, 0x01, 0x02, 0xF7);
        List<byte>[] built = new List<byte>[tracks.Length];
        for (int t = 0; t < tracks.Length; t++) built[t] = tracks[t].Build();
        return Header(1, tracks.Length, 96, built);
    }

    private static byte[] ModelsDense()
    {
        _state = 0x68E31DA4;
        TimedTrack tempo = new TimedTrack();
        tempo.At(0, 0xFF, 0x51, 0x03, 0x01, 0x86, 0xA0);     // 100,000 us per quarter: about 104 us per tick
        TimedTrack[] tracks = new TimedTrack[9];
        tracks[0] = tempo;
        for (int t = 1; t < tracks.Length; t++)
        {
            tracks[t] = new TimedTrack();
            long tick = 0;
            for (int i = 0; i < 600; i++)
            {
                uint r = Next();
                tick += (r >> 3) % 6;
                if (i == 300) tick += 12000;                    // more than a second of silence
                int channel = (r & 7) == 0 ? (int)((r >> 4) % 16) : t;
                if ((r % 11) == 0) tracks[t].At(tick, 0xB0 | channel, 1 + (int)((r >> 8) % 10), (int)((r >> 16) & 0x7F));
                else Note(tracks[t], tick, channel, 36 + (int)((r >> 8) % 48), 1 + (int)((r >> 16) % 127), 1 + (r >> 24) % 20, ((r >> 5) & 1) != 0, 0);
            }
        }
        List<byte>[] built = new List<byte>[tracks.Length];
        for (int t = 0; t < tracks.Length; t++) built[t] = tracks[t].Build();
        return Header(1, tracks.Length, 960, built);
    }

    // ------------------------------------------------------------ routing

    private static void NameMeta(TimedTrack track, int type, string text)
    {
        byte[] data = new UTF8Encoding(false).GetBytes(text);
        int[] bytes = new int[3 + data.Length];
        bytes[0] = 0xFF; bytes[1] = type; bytes[2] = data.Length;
        for (int i = 0; i < data.Length; i++) bytes[3 + i] = data[i];
        track.At(0, bytes);
    }

    // Track/channel lanes for the routing planner: multi-channel tracks,
    // shared source channels across tracks, bank/program changes while
    // sounding and while silent, GM families and nonzero banks, dynamic
    // state, sustain, CC120/121/123, same-key overlaps, percussion, system
    // traffic, names (including UTF-8), and notes never released.
    private static byte[] RoutingLanes()
    {
        _state = 0x3C6EF372;
        TimedTrack[] tracks = new TimedTrack[12];
        for (int t = 0; t < tracks.Length; t++) tracks[t] = new TimedTrack();
        tracks[0].At(0, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);
        NameMeta(tracks[0], 0x03, "Conductor");
        string[] names = { null, "Strings A", "Strings B", "Drums", "Bass – left", "ピアノ", "", "Brass", null, "Lead", "Pads", "FX" };
        string[] instruments = { null, "Violins", "Violas", "Kit", null, "Piano", "Organ", null, "Trumpet", "Synth", null, "Noise" };
        // Each track's channels: two tracks share channel 0, two share 9.
        int[][] channels = { null, new[] { 0 }, new[] { 0, 1 }, new[] { 9 }, new[] { 2 }, new[] { 3, 4 }, new[] { 5 },
            new[] { 6, 7 }, new[] { 8 }, new[] { 10, 9 }, new[] { 11, 12 }, new[] { 13, 14, 15 } };
        int[] programs = { 0, 48, 49, 0, 33, 0, 16, 56, 57, 80, 88, 120 };
        for (int t = 1; t < tracks.Length; t++)
        {
            if (names[t] != null) NameMeta(tracks[t], 0x03, names[t]);
            if (instruments[t] != null) NameMeta(tracks[t], 0x04, instruments[t]);
            foreach (int ch in channels[t])
            {
                if (ch != 9)
                {
                    if (t == 10) { tracks[t].At(0, 0xB0 | ch, 0, 8); tracks[t].At(0, 0xB0 | ch, 32, 1); }
                    tracks[t].At(0, 0xC0 | ch, programs[t] + (ch & 1));
                }
            }
        }
        tracks[6].At(0, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7);           // system traffic in track 6
        for (int t = 1; t < tracks.Length; t++)
        {
            long tick = (t % 3) * 400;                                        // staggered starts (handoffs)
            int count = t == 8 ? 12 : 70;
            for (int i = 0; i < count; i++)
            {
                uint r = Next();
                tick += 2 + (r & 7);
                int[] lane = channels[t];
                int ch = lane[(int)((r >> 4) % (uint)lane.Length)];
                int pitch = ch == 9 ? 35 + (int)((r >> 8) % 12) : 48 + (int)((r >> 8) % 16);
                int velocity = 20 + (int)((r >> 12) % 100);
                int pick = (int)((r >> 20) % 40);
                if (pick < 28) Note(tracks[t], tick, ch, pitch, velocity, 1 + (r >> 24) % 24, ((r >> 3) & 1) != 0, 0);
                else if (pick == 28) tracks[t].At(tick, 0xC0 | ch, programs[t] + (int)((r >> 9) % 3));          // while sounding
                else if (pick == 29) tracks[t].At(tick, 0xB0 | ch, 0, (int)((r >> 9) % 3));
                else if (pick == 30) tracks[t].At(tick, 0xB0 | ch, 32, (int)((r >> 9) % 2));
                else if (pick == 31) tracks[t].At(tick, 0xB0 | ch, 7, (int)(r & 0x7F));
                else if (pick == 32) tracks[t].At(tick, 0xE0 | ch, (int)(r & 0x7F), (int)((r >> 7) & 0x7F));
                else if (pick == 33) tracks[t].At(tick, 0xB0 | ch, 64, 127);
                else if (pick == 34) tracks[t].At(tick, 0xB0 | ch, 64, 0);
                else if (pick == 35) tracks[t].At(tick, 0xB0 | ch, 121, 0);
                else if (pick == 36) tracks[t].At(tick, 0xB0 | ch, (r & 1) != 0 ? 120 : 123, 0);
                else if (pick == 37) tracks[t].At(tick, 0xD0 | ch, (int)(r & 0x7F));
                else if (pick == 38) tracks[t].At(tick, 0x90 | ch, pitch, velocity);                     // never released
                else tracks[t].At(tick, 0x80 | ch, pitch, 0);                                             // orphan release
            }
        }
        // Same-key overlap across two lanes of channel 0, then a silent
        // program change on channel 3 (handoff) and a late lane on channel 2.
        Note(tracks[1], 1000, 0, 60, 90, 50, false, 0);
        Note(tracks[2], 1010, 0, 60, 80, 50, false, 0);
        tracks[5].At(1200, 0xC0 | 3, 1);
        Note(tracks[5], 1210, 3, 62, 70, 10, false, 0);
        Note(tracks[4], 1500, 2, 40, 100, 30, true, 0);
        List<byte>[] built = new List<byte>[tracks.Length];
        for (int t = 0; t < tracks.Length; t++) built[t] = tracks[t].Build();
        return Header(1, tracks.Length, 96, built);
    }

    // More lanes than destinations, heavily overlapping with differing
    // patches and controller state: drives GM-family and aggressive merges,
    // destructive-merge omission, and capacity errors.
    private static byte[] RoutingManyLanes()
    {
        _state = 0xA54FF53A;
        TimedTrack[] tracks = new TimedTrack[25];
        for (int t = 0; t < tracks.Length; t++) tracks[t] = new TimedTrack();
        tracks[0].At(0, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90);
        for (int t = 1; t < tracks.Length; t++)
        {
            int ch = (t - 1) % 16;
            int second = (t * 7) % 16;
            if ((t % 4) == 0) tracks[t].At(0, 0xB0 | ch, 0, t % 3);
            tracks[t].At(0, 0xC0 | ch, (t * 5) % 128);
            long tick = 0;
            int count = 10 + (t * 13) % 50;
            for (int i = 0; i < count; i++)
            {
                uint r = Next();
                tick += 1 + (r & 3);
                int channel = (i % 9) == 8 ? second : ch;
                int pitch = 50 + (int)((r >> 6) % 10);
                Note(tracks[t], tick, channel, pitch, 1 + (int)((r >> 12) % 127), 2 + (r >> 20) % (uint)(4 + t), false, 0);
                if ((r % 17) == 0) tracks[t].At(tick, 0xB0 | channel, 11, (int)((r >> 8) & 0x7F));
                if ((r % 23) == 0) tracks[t].At(tick, 0xE0 | channel, 0, (int)((r >> 9) & 0x7F));
            }
        }
        List<byte>[] built = new List<byte>[tracks.Length];
        for (int t = 0; t < tracks.Length; t++) built[t] = tracks[t].Build();
        return Header(1, tracks.Length, 96, built);
    }

    // Exact-safe merging: a lane compatible with two existing destination
    // groups joins the one on its own source channel (v1 FindExactGroup).
    // Track 1 (channel 3) dominates; track 2 (channel 6) overlaps its keys,
    // so it opens its own group; track 3 (channel 6, other keys) fits both.
    private static byte[] RoutingExactMerge()
    {
        TimedTrack[] tracks = new TimedTrack[4];
        for (int t = 0; t < tracks.Length; t++) tracks[t] = new TimedTrack();
        tracks[0].At(0, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);
        for (int i = 0; i < 12; i++)
        {
            Note(tracks[1], i * 48L, 2, 60, 120, 40, false, 0);
            Note(tracks[2], i * 48L + 4, 5, 60, 60, 20, false, 0);
            Note(tracks[3], i * 48L + 30, 5, 72 + (i % 3), 30, 4, true, 0);
        }
        List<byte>[] built = new List<byte>[tracks.Length];
        for (int t = 0; t < tracks.Length; t++) built[t] = tracks[t].Build();
        return Header(1, tracks.Length, 96, built);
    }

    // One lane attacking `distinct` different (bank LSB, program) patches:
    // 4,096 is the planner's bounded limit; one more is rejected.
    private static byte[] RoutingPatches(int distinct)
    {
        TimedTrack track = new TimedTrack();
        for (int i = 0; i < distinct; i++)
        {
            long tick = i * 2L;
            track.At(tick, 0xB0, 32, i / 128);
            track.At(tick, 0xC0, i % 128);
            Note(track, tick, 0, 60 + (i % 3), 64, 1, (i & 1) != 0, 0);
        }
        return Header(1, 1, 96, track.Build());
    }

    // ------------------------------------------------------------ malformed

    private static byte[] RiffSizeTooLarge()
    {
        List<byte> r = new List<byte>(); AddAscii(r, "RIFF"); AddU32LE(r, 1000); AddAscii(r, "RMID");
        AddRiffChunk(r, "data", Single(Track(0x00, 0xFF, 0x2F, 0x00)), true);
        return r.ToArray();
    }

    private static byte[] HeaderShort()
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 5); AddU16(b, 0); AddU16(b, 1); Add(b, 0x01);
        return b.ToArray();
    }

    private static byte[] Truncated(byte[] bytes, int length)
    {
        byte[] result = new byte[length];
        Array.Copy(bytes, result, length);
        return result;
    }

    private static byte[] HeaderExtraTruncated()
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 100); AddU16(b, 0); AddU16(b, 1); AddU16(b, 480); Add(b, 0x01, 0x02);
        return b.ToArray();
    }

    private static byte[] WrongTrackId()
    {
        List<byte> b = new List<byte>();
        AddAscii(b, "MThd"); AddU32(b, 6); AddU16(b, 1); AddU16(b, 2); AddU16(b, 480);
        AddAscii(b, "MTrk"); AddU32(b, 4); Add(b, 0x00, 0xFF, 0x2F, 0x00);
        AddAscii(b, "XTrk"); AddU32(b, 4); Add(b, 0x00, 0xFF, 0x2F, 0x00);
        return b.ToArray();
    }

    // ---------------------------------------------------------------- large

    // One format-0 track, one channel: `held` note-ons (keys cycling 0-127, velocity 64, all at tick 0, running
    // status) with no release, so all `held` notes sound at once; then `held` note-offs one tick later. About
    // 3 bytes per event.
    private static byte[] RoutingHeld(int held)
    {
        List<byte> t = new List<byte>(held * 6 + 64);
        for (int i = 0; i < held; i++)
        {
            if (i == 0) Add(t, 0x00, 0x90); else t.Add(0x00);
            Add(t, i % 128, 64);
        }
        for (int i = 0; i < held; i++)
        {
            if (i == 0) Add(t, 0x01, 0x80); else t.Add(0x00);
            Add(t, i % 128, 64);
        }
        End(t);
        return Header(0, 1, 480, t);
    }

    // tracks x eventsPerTrack channel events (running status, mixed kinds),
    // a tempo change every 2,000 events in track 0, and a 24-byte SysEx every
    // 50,000 events per track.
    private static byte[] LargeFile(int trackCount, int eventsPerTrack)
    {
        _state = 0x9E3779B9;
        MemoryStream file = new MemoryStream();
        List<byte> header = new List<byte>();
        AddAscii(header, "MThd"); AddU32(header, 6); AddU16(header, 1); AddU16(header, trackCount); AddU16(header, 480);
        file.Write(header.ToArray(), 0, header.Count);
        for (int track = 0; track < trackCount; track++)
        {
            List<byte> t = new List<byte>(eventsPerTrack * 3 + 1024);
            int channel = track % 16;
            int status = -1;
            for (int i = 0; i < eventsPerTrack; i++)
            {
                uint r = Next();
                AddVlq(t, r % 4);
                if (track == 0 && i % 2000 == 0) { Add(t, 0xFF, 0x51, 0x03, 0x07, (int)(0xA1 + (r >> 8) % 8), (int)((r >> 16) & 0xFF)); status = -1; AddVlq(t, 0); }
                if (i % 50000 == 25000)
                {
                    Add(t, 0xF0, 23); for (int k = 0; k < 22; k++) t.Add((byte)((r >> (k % 24)) & 0x7F)); t.Add(0xF7);
                    status = -1; AddVlq(t, 0);
                }
                int kind = (int)((r >> 4) % 16);
                int s = kind < 12 ? 0x90 | channel : kind < 14 ? 0xB0 | channel : kind < 15 ? 0xE0 | channel : 0xD0 | channel;
                if (s != status) { t.Add((byte)s); status = s; }
                if ((s & 0xF0) == 0xD0) t.Add((byte)((r >> 12) & 0x7F));
                else { t.Add((byte)((r >> 12) & 0x7F)); t.Add((byte)(((r >> 20) & 0x7F) * ((kind & 3) == 0 ? 0 : 1))); }
            }
            End(t);
            byte[] chunk = new byte[8];
            chunk[0] = (byte)'M'; chunk[1] = (byte)'T'; chunk[2] = (byte)'r'; chunk[3] = (byte)'k';
            chunk[4] = (byte)(t.Count >> 24); chunk[5] = (byte)(t.Count >> 16); chunk[6] = (byte)(t.Count >> 8); chunk[7] = (byte)t.Count;
            file.Write(chunk, 0, 8);
            file.Write(t.ToArray(), 0, t.Count);
        }
        return file.ToArray();
    }
}
