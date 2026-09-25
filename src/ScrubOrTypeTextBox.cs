using System;
using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class ScrubValueEventArgs : EventArgs
    {
        internal readonly int Value;
        internal Exception Error;

        internal ScrubValueEventArgs(int value) { Value = value; }
    }

    internal sealed class ScrubOrTypeTextBox : TextBox
    {
        private const int WmContextMenu = 0x007B;
        private const int DragThreshold = 3;
        private readonly ToolTip _toolTip = new ToolTip();
        private int _minimum;
        private int _maximum = 127;
        private int _value;
        private int _displayOffset;
        private int _coarseUnits = 1;
        private int _coarsePixels = 1;
        private int _fineUnits = 1;
        private int _finePixels = 1;
        private int _pixelRemainder;
        private bool _remainderFine;
        private int _gestureStartValue;
        private Point _gestureStartScreen;
        private Point _scrubAnchorScreen;
        private bool _gestureArmed;
        private bool _scrubbing;
        private bool _typing;
        private bool _cursorHidden;
        private bool _ignoreWarpMove;
        private bool _forced;
        private bool _historical;
        private bool _numericOnly;
        private int _numericIncrement = 1;
        private bool _thousandsSeparator;
        private int _typingStartValue;
        private string _lastError;
        private Func<int, string> _displayFormatter;

        [DllImport("user32.dll")]
        private static extern bool HideCaret(IntPtr window);
        [DllImport("user32.dll")]
        private static extern bool ShowCaret(IntPtr window);

        internal event EventHandler<ScrubValueEventArgs> ValueRequested;
        internal event EventHandler<ScrubValueEventArgs> AutoRequested;
        internal event EventHandler<ScrubValueEventArgs> ChaseRequested;
        internal event EventHandler ValueChanged;

        internal ScrubOrTypeTextBox()
        {
            BorderStyle = BorderStyle.None;
            ReadOnly = true;
            TabStop = false;
            TextAlign = HorizontalAlignment.Right;
            Cursor = Cursors.SizeWE;
            BackColor = SystemColors.Window;
        }

        internal int CurrentValue { get { return _value; } }
        internal bool IsTyping { get { return _typing; } }
        internal bool IsScrubbing { get { return _scrubbing; } }
        internal bool CursorIsHidden { get { return _cursorHidden; } }
        internal bool IsForced { get { return _forced; } }
        internal string LastError { get { return _lastError ?? String.Empty; } }

        internal void ConfigureNumeric(int value, int minimum, int maximum,
            int coarseUnits, int coarsePixels, int fineUnits, int finePixels)
        {
            if (minimum < 0 || maximum < minimum) throw new ArgumentOutOfRangeException("maximum");
            Configure(value, minimum, maximum, 0, coarseUnits, coarsePixels, fineUnits, finePixels,
                false, false, null, null);
            _numericOnly = true;
            _thousandsSeparator = true;
            TabStop = true;
            SetFlatText();
        }

        internal decimal Value
        {
            get { return _value; }
            set
            {
                if (value < _minimum || value > _maximum || value != Decimal.Truncate(value))
                    throw new ArgumentOutOfRangeException("value");
                SetNumericValue((int)value);
            }
        }

        internal decimal Minimum
        {
            get { return _minimum; }
            set { SetNumericRange((int)value, _maximum); }
        }

        internal decimal Maximum
        {
            get { return _maximum; }
            set { SetNumericRange(_minimum, (int)value); }
        }

        internal decimal Increment
        {
            get { return _numericIncrement; }
            set { _numericIncrement = Math.Max(1, (int)value); }
        }

        internal bool ThousandsSeparator
        {
            get { return _thousandsSeparator; }
            set { _thousandsSeparator = value; if (!_typing) SetFlatText(); }
        }

        internal int DecimalPlaces
        {
            get { return 0; }
            set { if (value != 0) throw new ArgumentOutOfRangeException("value"); }
        }

        private void SetNumericRange(int minimum, int maximum)
        {
            if (minimum < 0 || maximum < minimum) throw new ArgumentOutOfRangeException("maximum");
            _minimum = minimum;
            _maximum = maximum;
            if (_value < minimum || _value > maximum) SetNumericValue(Clamp(_value));
        }

        private void SetNumericValue(int value)
        {
            if (_value == value) return;
            _value = value;
            if (!_typing) SetFlatText();
            EventHandler handler = ValueChanged;
            if (handler != null) handler(this, EventArgs.Empty);
        }

        internal void Configure(int value, int minimum, int maximum, int displayOffset,
            int coarseUnits, int coarsePixels, int fineUnits, int finePixels,
            bool forced, bool historical, Func<int, string> displayFormatter, string help)
        {
            EndGesture(false);
            _minimum = minimum;
            _maximum = maximum;
            _displayOffset = displayOffset;
            _coarseUnits = Math.Max(1, coarseUnits);
            _coarsePixels = Math.Max(1, coarsePixels);
            _fineUnits = Math.Max(1, fineUnits);
            _finePixels = Math.Max(1, finePixels);
            _pixelRemainder = 0;
            _remainderFine = false;
            _displayFormatter = displayFormatter;
            _numericOnly = false;
            _value = Clamp(value);
            _forced = forced;
            _historical = historical;
            _lastError = null;
            ReadOnly = true;
            TabStop = false;
            _typing = false;
            SetFlatText();
            _toolTip.SetToolTip(this, help ?? String.Empty);
        }

        internal void BeginPointerGesture(Point screenPosition, MouseButtons button)
        {
            if (button == MouseButtons.Right)
            {
                if (_numericOnly) return;
                if (_forced) RequestAuto();
                else if (_historical) RequestChase();
                return;
            }
            if (button != MouseButtons.Left) return;
            _gestureArmed = true;
            _gestureStartScreen = screenPosition;
            _scrubAnchorScreen = screenPosition;
            _gestureStartValue = _value;
            _lastError = null;
            SelectionStart = 0;
            SelectionLength = 0;
        }

        internal void ContinuePointerGesture(Point screenPosition, bool fine, bool leftButtonDown)
        {
            if (!_gestureArmed) return;
            if (!leftButtonDown)
            {
                EndGesture(false);
                return;
            }
            int totalDistance = screenPosition.X - _gestureStartScreen.X;
            if (!_scrubbing && Math.Abs(totalDistance) > DragThreshold) StartScrubbing(screenPosition);
            if (!_scrubbing) return;
            if (_ignoreWarpMove && screenPosition == _scrubAnchorScreen)
            {
                _ignoreWarpMove = false;
                return;
            }
            int delta = screenPosition.X - _scrubAnchorScreen.X;
            if (delta == 0) return;
            ApplyScrubDelta(delta, fine);
            WarpCursorToAnchor();
        }

        internal void EndPointerGesture()
        {
            if (!_gestureArmed) return;
            if (_scrubbing) EndGesture(false);
            else
            {
                _gestureArmed = false;
                EnterTypingMode();
            }
        }

        internal void ApplyScrubDeltaForTesting(int horizontalPixels, bool fine)
        {
            ApplyScrubDelta(horizontalPixels, fine);
        }

        internal void EnterTypingForTesting() { EnterTypingMode(); }
        internal bool CommitTextForTesting(string value) { Text = value; return CommitTypedValue(true); }
        internal bool CommitFocusLossForTesting(string value) { Text = value; return CommitTypedValue(false); }
        internal void EscapeForTesting() { AbandonInteraction(); }
        internal void RequestAutoForTesting() { if (_forced) RequestAuto(); }
        internal void FinishHostInteraction(bool commitTypedValue)
        {
            if (_typing && commitTypedValue) CommitTypedValue(false);
            else if (_typing) AbandonInteraction();
            EndGesture(false);
        }

        internal bool GestureArmedForTesting { get { return _gestureArmed; } }
        internal void ContinuePointerGestureForTesting(Point screenPosition, bool fine, bool leftButtonDown)
        {
            ContinuePointerGesture(screenPosition, fine, leftButtonDown);
        }
        internal void ArrowForTesting(bool increase) { AdjustTypedValue(increase ? 1 : -1); }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (_typing) { base.OnMouseDown(e); return; }
            BeginPointerGesture(PointToScreen(e.Location), e.Button);
        }

        protected override void WndProc(ref Message message)
        {
            if (message.Msg == WmContextMenu) return;
            base.WndProc(ref message);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_typing) { base.OnMouseMove(e); return; }
            ContinuePointerGesture(PointToScreen(e.Location), (ModifierKeys & Keys.Shift) != 0,
                (e.Button & MouseButtons.Left) != 0 || (Control.MouseButtons & MouseButtons.Left) != 0);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (_typing) { base.OnMouseUp(e); return; }
            if (e.Button == MouseButtons.Left) EndPointerGesture();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (_numericOnly && !_typing && (e.KeyCode == Keys.F2 || e.KeyCode == Keys.Enter))
            {
                EnterTypingMode(); e.SuppressKeyPress = true; return;
            }
            if (_numericOnly && !_typing && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down))
            {
                RequestValue(ClampLong((long)_value + (e.KeyCode == Keys.Up ? _numericIncrement : -_numericIncrement)));
                e.SuppressKeyPress = true; return;
            }
            if (e.KeyCode == Keys.Enter && _typing)
            {
                CommitTypedValue(true); e.SuppressKeyPress = true; return;
            }
            if (e.KeyCode == Keys.Escape)
            {
                AbandonInteraction(); e.SuppressKeyPress = true; return;
            }
            if (_typing && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down))
            {
                AdjustTypedValue(e.KeyCode == Keys.Up ? 1 : -1);
                e.SuppressKeyPress = true;
                return;
            }
            base.OnKeyDown(e);
        }

        protected override void OnLostFocus(EventArgs e)
        {
            if (_typing) CommitTypedValue(false);
            if (_scrubbing || _gestureArmed) EndGesture(false);
            base.OnLostFocus(e);
        }

        protected override void OnGotFocus(EventArgs e)
        {
            base.OnGotFocus(e);
            if (!_typing && IsHandleCreated) HideCaret(Handle);
        }

        protected override void OnMouseCaptureChanged(EventArgs e)
        {
            if (_scrubbing && !Capture) EndGesture(false);
            base.OnMouseCaptureChanged(e);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                EndGesture(false);
                _toolTip.Dispose();
            }
            base.Dispose(disposing);
        }

        private void StartScrubbing(Point screenPosition)
        {
            _scrubbing = true;
            _scrubAnchorScreen = screenPosition;
            _pixelRemainder = 0;
            _remainderFine = false;
            Capture = true;
            if (!_cursorHidden)
            {
                Cursor.Hide();
                _cursorHidden = true;
            }
        }

        private void ApplyScrubDelta(int pixels, bool fine)
        {
            if (!_numericOnly && _minimum == 0 && _maximum == 1)
            {
                if (pixels > 0) RequestValue(1);
                else if (pixels < 0) RequestValue(0);
                return;
            }
            if (_remainderFine != fine)
            {
                _pixelRemainder = 0;
                _remainderFine = fine;
            }
            int divisor = fine ? _finePixels : _coarsePixels;
            int units = fine ? _fineUnits : _coarseUnits;
            int accumulated = _pixelRemainder + pixels;
            int steps = accumulated / divisor;
            _pixelRemainder = accumulated - steps * divisor;
            if (steps == 0) return;
            long proposed = (long)_value + (long)steps * units;
            RequestValue(ClampLong(proposed));
        }

        private void WarpCursorToAnchor()
        {
            try
            {
                _ignoreWarpMove = true;
                Cursor.Position = _scrubAnchorScreen;
            }
            catch
            {
                _ignoreWarpMove = false;
                EndGesture(false);
                throw;
            }
        }

        private void EnterTypingMode()
        {
            EndGesture(false);
            _typing = true;
            _typingStartValue = _value;
            ReadOnly = false;
            TabStop = true;
            Cursor = Cursors.IBeam;
            Text = FormatEditable(_value);
            Focus();
            if (IsHandleCreated) ShowCaret(Handle);
            SelectAll();
        }

        private void AdjustTypedValue(int direction)
        {
            if (!_typing) return;
            int parsed;
            if (!TryParseEditable(Text, out parsed)) parsed = _value;
            parsed = ClampLong((long)parsed + direction);
            Text = FormatEditable(parsed);
            SelectAll();
            _lastError = null;
        }

        private bool CommitTypedValue(bool explicitCommit)
        {
            if (!_typing) return true;
            int parsed;
            if (!TryParseEditable(Text, out parsed) || parsed < _minimum || parsed > _maximum)
            {
                _lastError = "Enter a value from " + FormatEditable(_minimum) + " through " + FormatEditable(_maximum) + ".";
                ExitTypingMode();
                return false;
            }
            if (explicitCommit || parsed != _typingStartValue) RequestValue(parsed);
            else _lastError = null;
            ExitTypingMode();
            return String.IsNullOrEmpty(_lastError);
        }

        private void ExitTypingMode()
        {
            _typing = false;
            ReadOnly = true;
            TabStop = _numericOnly;
            Cursor = Cursors.SizeWE;
            if (Focused && IsHandleCreated) HideCaret(Handle);
            SelectionLength = 0;
            SetFlatText();
            if (!String.IsNullOrEmpty(_lastError)) _toolTip.Show(_lastError, this, 0, Height, 2500);
        }

        private void AbandonInteraction()
        {
            if (_typing)
            {
                _lastError = null;
                ExitTypingMode();
            }
            EndGesture(false);
        }

        private void EndGesture(bool restoreStart)
        {
            if (restoreStart) _value = _gestureStartValue;
            _gestureArmed = false;
            _scrubbing = false;
            _ignoreWarpMove = false;
            _pixelRemainder = 0;
            if (Capture) Capture = false;
            if (_cursorHidden)
            {
                Cursor.Show();
                _cursorHidden = false;
            }
            if (!_typing) Cursor = Cursors.SizeWE;
            if (!_typing) SetFlatText();
        }

        private void RequestValue(int value)
        {
            value = Clamp(value);
            if (_numericOnly)
            {
                if (value == _value) return;
                int previous = _value;
                try { SetNumericValue(value); _lastError = null; }
                catch (Exception ex)
                {
                    _value = previous;
                    _lastError = "Could not apply: " + ex.Message;
                    _toolTip.Show(_lastError, this, 0, Height, 2500);
                }
                if (!_typing) SetFlatText();
                return;
            }
            if (value == _value && _forced) return;
            ScrubValueEventArgs request = new ScrubValueEventArgs(value);
            EventHandler<ScrubValueEventArgs> handler = ValueRequested;
            try { if (handler != null) handler(this, request); }
            catch (Exception ex) { request.Error = ex; }
            _value = value;
            _forced = true;
            _lastError = request.Error == null ? null : "Could not apply: " + request.Error.Message;
            if (!String.IsNullOrEmpty(_lastError)) _toolTip.SetToolTip(this, _lastError);
            if (!_typing) SetFlatText();
        }

        private void RequestAuto()
        {
            ScrubValueEventArgs request = new ScrubValueEventArgs(ChannelOverrideState.AutoValue);
            EventHandler<ScrubValueEventArgs> handler = AutoRequested;
            try { if (handler != null) handler(this, request); }
            catch (Exception ex) { request.Error = ex; }
            _lastError = request.Error == null ? null : "Could not return to Auto: " + request.Error.Message;
            if (request.Error == null) _forced = false;
            if (!String.IsNullOrEmpty(_lastError)) _toolTip.Show(_lastError, this, 0, Height, 2500);
            SetFlatText();
        }

        private void RequestChase()
        {
            ScrubValueEventArgs request = new ScrubValueEventArgs(_value);
            EventHandler<ScrubValueEventArgs> handler = ChaseRequested;
            try { if (handler != null) handler(this, request); }
            catch (Exception ex) { request.Error = ex; }
            _lastError = request.Error == null ? null : "Could not restore source value: " + request.Error.Message;
            if (!String.IsNullOrEmpty(_lastError)) _toolTip.Show(_lastError, this, 0, Height, 2500);
            SetFlatText();
        }

        private void SetFlatText()
        {
            if (_typing) return;
            Text = _numericOnly && _thousandsSeparator
                ? _value.ToString("N0", CultureInfo.CurrentCulture)
                : _displayFormatter == null ? FormatEditable(_value) : _displayFormatter(_value);
            SelectionLength = 0;
        }

        private string FormatEditable(int value)
        {
            if (!_numericOnly && _minimum == 0 && _maximum == 1 && _displayOffset == 0)
                return value == 0 ? "Off" : "On";
            return (value + _displayOffset).ToString(CultureInfo.CurrentCulture);
        }

        private bool TryParseEditable(string text, out int value)
        {
            string normalized = (text ?? String.Empty).Trim();
            if (!_numericOnly && _minimum == 0 && _maximum == 1 && _displayOffset == 0)
            {
                if (String.Equals(normalized, "On", StringComparison.OrdinalIgnoreCase)) { value = 1; return true; }
                if (String.Equals(normalized, "Off", StringComparison.OrdinalIgnoreCase)) { value = 0; return true; }
            }
            int shown;
            if (!Int32.TryParse(normalized, _numericOnly
                ? NumberStyles.Integer | NumberStyles.AllowThousands : NumberStyles.Integer,
                CultureInfo.CurrentCulture, out shown))
            {
                value = _value; return false;
            }
            value = shown - _displayOffset;
            return true;
        }

        private int Clamp(int value) { return Math.Max(_minimum, Math.Min(_maximum, value)); }
        private int ClampLong(long value)
        {
            if (value < _minimum) return _minimum;
            if (value > _maximum) return _maximum;
            return (int)value;
        }
    }
}
