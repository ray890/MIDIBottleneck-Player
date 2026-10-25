MIDIBottleneck - Windows 11 MIDI driver check
=============================================

What this is for
  Windows 11 has a new MIDI system called "Windows MIDI Services". This small
  check finds out two things on your PC:
    1. whether the MIDI driver of your synthesizers can be used directly
       (without going through Windows' older MIDI layer), and
    2. how much faster that is.
  The answer helps decide how the MIDIBottleneck player should talk to
  synthesizers on Windows 11.

What the check does
  - It reports your Windows version.
  - It reports whether Windows MIDI Services is present.
  - It lists the MIDI drivers Windows has.
  - It tries each synthesizer driver silently, and then measures how fast
    messages can be handed over, the normal way and the direct way.
  It is silent: it sends nothing that makes a sound (only a few harmless
  setting messages that no synthesizer plays). It changes no setting on your
  PC, needs no administrator rights, and installs nothing. It writes only one
  result file, in the same folder.

How to run it
  1. Extract the zip file to any folder (right-click the zip, "Extract All").
     Do not run it from inside the zip window.
  2. Double-click RUN-CHECK.cmd.
  3. Wait about one minute (up to three if your PC is busy) until the window
     says the check has finished, then press a key to close it.
     For the most reliable result, close music players, MIDI programs and
     synthesizer settings windows (for example OmniMIDI or VirtualMIDISynth
     configuration programs) before you start, and do not start the check
     twice. If the window is still open after five minutes, close it anyway
     and send the result file: it is complete as soon as it ends with the
     line "DIRECT-CHECK: DONE".

If Windows shows a blue "Windows protected your PC" (SmartScreen) message
  The check program is an unsigned private test program, so Windows does not
  know it. This warning is normal for such programs. If you are willing to
  run it, click "More info", then "Run anyway". If you would rather not,
  that is fine too; just tell us.

What to send back
  ONE file, which appears in the same folder:
      direct-check-result-<your computer name>.txt
  It is a plain text file; you can open it in Notepad to see what it says.
  It contains the Windows version, the list of MIDI drivers and the test
  results.

Thank you for helping!
