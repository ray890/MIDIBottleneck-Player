## Summary

Describe the focused change and why it is needed.

## Verification

- [ ] Focused tests passed.
- [ ] The checks for the area I changed passed (see `v2/BUILDING.md`: unit tests, `v2\tools\parity.ps1` ending `PARITY: PASS`, the mutation checks, as relevant).
- [ ] Every build I could affect was built (x64, x86 and, if relevant, XP).
- [ ] Visible changes were inspected.
- [ ] Real-driver testing, if any, was bounded and identified separately.

## Risk review

- [ ] MIDI ordering, payloads, cancellation, and lifecycle behavior are preserved or explicitly tested.
- [ ] Queue/Analysis/statistics semantics remain consistent.
- [ ] Hot-path allocation or throughput impact was measured where relevant.
- [ ] No proprietary MIDI, provider binary, personal path, credential, or unclear third-party material is included.
