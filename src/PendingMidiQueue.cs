using System;
using System.Collections;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Pending source events occupy reusable value nodes, not one heap object
    // per event. Service order and eligible-attack order use separate links.
    // A paired pending release is linked directly to its attack, so overflow
    // never scans the queue to reclaim a note occurrence.
    internal sealed class PendingMidiQueue : IEnumerable<int>
    {
        private const int SegmentShift = 8;
        private const int SegmentSize = 1 << SegmentShift;
        private const int SegmentMask = SegmentSize - 1;

        internal struct Entry
        {
            internal int EventIndex;
            internal int PairedAttackIndex;
            internal bool IsNoteOn;
            internal bool IsNoteOff;
        }

        private struct Node
        {
            internal Entry Entry;
            internal int Previous;
            internal int Next;
            internal int EligiblePrevious;
            internal int EligibleNext;
            internal int PairedReleaseNode;
            internal int FreeNext;
        }

        private readonly List<Node[]> _segments = new List<Node[]>();
        private readonly Dictionary<int, int> _pendingAttackNodes = new Dictionary<int, int>();
        private int _nextFresh;
        private int _free = -1;
        private int _head = -1;
        private int _tail = -1;
        private int _eligibleHead = -1;
        private int _eligibleTail = -1;
        private int _eligibleCount;
        private int _count;
        private bool _noteIndexEnabled;

        internal PendingMidiQueue(int capacity, bool noteIndexEnabled = false)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            _noteIndexEnabled = noteIndexEnabled;
        }
        internal int Count { get { return _count; } }
        internal int PeakBackingCount { get { return _nextFresh; } }
        internal int BackingEntryCount { get { return _count; } }
        internal int EligibleBackingCount { get { return _eligibleCount; } }
        internal int ActiveEligibleCount { get { return _eligibleCount; } }
        internal int AllocatedSegmentCount { get { return _segments.Count; } }

        internal void Enqueue(int eventIndex)
        {
            Enqueue(new Entry { EventIndex = eventIndex, PairedAttackIndex = -1 });
        }

        internal void Enqueue(Entry entry)
        {
            int id = Allocate();
            Node node = new Node
            {
                Entry = entry,
                Previous = _tail,
                Next = -1,
                EligiblePrevious = -1,
                EligibleNext = -1,
                PairedReleaseNode = -1,
                FreeNext = -1
            };
            Set(id, node);
            if (_tail >= 0)
            {
                Node tail = Get(_tail);
                tail.Next = id;
                Set(_tail, tail);
            }
            else _head = id;
            _tail = id;
            _count++;

            if (_noteIndexEnabled && entry.IsNoteOn)
            {
                node = Get(id);
                node.EligiblePrevious = _eligibleTail;
                Set(id, node);
                if (_eligibleTail >= 0)
                {
                    Node tail = Get(_eligibleTail);
                    tail.EligibleNext = id;
                    Set(_eligibleTail, tail);
                }
                else _eligibleHead = id;
                _eligibleTail = id;
                _eligibleCount++;
                _pendingAttackNodes.Add(entry.EventIndex, id);
            }
            else if (_noteIndexEnabled && entry.IsNoteOff && entry.PairedAttackIndex >= 0)
            {
                int attackNode;
                if (_pendingAttackNodes.TryGetValue(entry.PairedAttackIndex, out attackNode))
                {
                    Node attack = Get(attackNode);
                    attack.PairedReleaseNode = id;
                    Set(attackNode, attack);
                }
            }
        }

        internal int Dequeue() { return DequeueEntry().EventIndex; }

        internal Entry DequeueEntry()
        {
            if (_head < 0) throw new InvalidOperationException("The pending MIDI queue is empty.");
            int id = _head;
            Entry entry = Get(id).Entry;
            Remove(id);
            return entry;
        }

        internal bool TryEvictOldestCompleteNote(out int attackIndex, out int releaseIndex)
        {
            if (!_noteIndexEnabled)
                throw new InvalidOperationException("The pending-note index has not been enabled.");
            if (_eligibleHead < 0)
            {
                attackIndex = -1;
                releaseIndex = -1;
                return false;
            }
            int attackNode = _eligibleHead;
            Node attack = Get(attackNode);
            attackIndex = attack.Entry.EventIndex;
            releaseIndex = -1;
            if (attack.PairedReleaseNode >= 0)
            {
                int releaseNode = attack.PairedReleaseNode;
                releaseIndex = Get(releaseNode).Entry.EventIndex;
                Remove(releaseNode);
            }
            Remove(attackNode);
            return true;
        }

        // A live policy edit may build or retire the note index once at its
        // control boundary. Ordinary queue operations remain O(1), and no
        // overflow performs a queue-length scan.
        internal void SetNoteIndexEnabled(bool enabled)
        {
            if (_noteIndexEnabled == enabled) return;
            _noteIndexEnabled = enabled;
            _pendingAttackNodes.Clear();
            _eligibleHead = _eligibleTail = -1;
            _eligibleCount = 0;
            if (!enabled) return;

            for (int id = _head; id >= 0; id = Get(id).Next)
            {
                Node node = Get(id);
                if (!node.Entry.IsNoteOn) continue;
                node.EligiblePrevious = _eligibleTail;
                node.EligibleNext = -1;
                node.PairedReleaseNode = -1;
                Set(id, node);
                if (_eligibleTail >= 0)
                {
                    Node previous = Get(_eligibleTail);
                    previous.EligibleNext = id;
                    Set(_eligibleTail, previous);
                }
                else _eligibleHead = id;
                _eligibleTail = id;
                _eligibleCount++;
                _pendingAttackNodes.Add(node.Entry.EventIndex, id);
            }
            for (int id = _head; id >= 0; id = Get(id).Next)
            {
                Node release = Get(id);
                if (!release.Entry.IsNoteOff || release.Entry.PairedAttackIndex < 0) continue;
                int attackId;
                if (_pendingAttackNodes.TryGetValue(release.Entry.PairedAttackIndex, out attackId))
                {
                    Node attack = Get(attackId);
                    attack.PairedReleaseNode = id;
                    Set(attackId, attack);
                }
            }
        }

        internal void Clear()
        {
            _head = _tail = _eligibleHead = _eligibleTail = -1;
            _free = -1;
            _nextFresh = 0;
            _eligibleCount = _count = 0;
            _pendingAttackNodes.Clear();
        }

        private void Remove(int id)
        {
            Node node = Get(id);
            if (node.Previous >= 0)
            {
                Node previous = Get(node.Previous);
                previous.Next = node.Next;
                Set(node.Previous, previous);
            }
            else _head = node.Next;
            if (node.Next >= 0)
            {
                Node next = Get(node.Next);
                next.Previous = node.Previous;
                Set(node.Next, next);
            }
            else _tail = node.Previous;

            if (_noteIndexEnabled && node.Entry.IsNoteOn)
            {
                if (node.EligiblePrevious >= 0)
                {
                    Node previous = Get(node.EligiblePrevious);
                    previous.EligibleNext = node.EligibleNext;
                    Set(node.EligiblePrevious, previous);
                }
                else _eligibleHead = node.EligibleNext;
                if (node.EligibleNext >= 0)
                {
                    Node next = Get(node.EligibleNext);
                    next.EligiblePrevious = node.EligiblePrevious;
                    Set(node.EligibleNext, next);
                }
                else _eligibleTail = node.EligiblePrevious;
                _eligibleCount--;
                _pendingAttackNodes.Remove(node.Entry.EventIndex);
            }
            else if (_noteIndexEnabled && node.Entry.IsNoteOff && node.Entry.PairedAttackIndex >= 0)
            {
                int attackNode;
                if (_pendingAttackNodes.TryGetValue(node.Entry.PairedAttackIndex, out attackNode))
                {
                    Node attack = Get(attackNode);
                    if (attack.PairedReleaseNode == id)
                    {
                        attack.PairedReleaseNode = -1;
                        Set(attackNode, attack);
                    }
                }
            }

            node.FreeNext = _free;
            Set(id, node);
            _free = id;
            _count--;
        }

        private int Allocate()
        {
            if (_free >= 0)
            {
                int id = _free;
                _free = Get(id).FreeNext;
                return id;
            }
            int fresh = _nextFresh++;
            if ((fresh >> SegmentShift) >= _segments.Count) _segments.Add(new Node[SegmentSize]);
            return fresh;
        }

        private Node Get(int id) { return _segments[id >> SegmentShift][id & SegmentMask]; }
        private void Set(int id, Node value) { _segments[id >> SegmentShift][id & SegmentMask] = value; }

        public IEnumerator<int> GetEnumerator()
        {
            for (int id = _head; id >= 0; id = Get(id).Next)
                yield return Get(id).Entry.EventIndex;
        }

        IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
    }
}
