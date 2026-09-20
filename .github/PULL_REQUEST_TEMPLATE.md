## Summary

Describe the focused change and why it is needed.

## Verification

- [ ] Focused tests passed.
- [ ] Complete x86 suite passed.
- [ ] Complete x64 suite passed.
- [ ] Visible changes were inspected.
- [ ] Native-provider testing, if any, was bounded and identified separately.

## Risk review

- [ ] MIDI ordering, payloads, cancellation, and lifecycle behavior are preserved or explicitly tested.
- [ ] Queue/Analysis/statistics semantics remain consistent.
- [ ] Hot-path allocation or throughput impact was measured where relevant.
- [ ] No proprietary MIDI, provider binary, personal path, credential, or unclear third-party material is included.
