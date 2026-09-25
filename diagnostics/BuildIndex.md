# MIDIBottleneck Player public build index

The public history was reconstructed because original Git commits did not exist. Original handoff dates are retained as reconstructed author/committer dates. See [Public-history provenance](../docs/PUBLIC_HISTORY.md).

| Build | Internal name | Original handoff | Source confidence | Public tag |
|---:|---|---|---|---|
| 01 | initial-winforms-player | 2026-08-29 21:23 -04:00 | nearest preserved | `v1.0.1` |
| 02 | sysex-seek-hardening | 2026-08-29 22:56 -04:00 | exact | `v1.0.2` |
| 03 | drop-buffer-statistics | 2026-08-30 02:32 -04:00 | nearest preserved | `v1.0.3` |
| 04 | independent-controls-analysis | 2026-08-30 09:21 -04:00 | nearest preserved | `v1.0.4` |
| 05 | compact-analysis-refinement | approximately 2026-08-30 21:04 -04:00 | nearest preserved | `v1.0.5` |
| 06 | kdmapi-sysex-live-controls | 2026-09-02 01:11 -04:00 | nearest preserved | `v1.0.6` |
| 07 | async-loading-analysis | 2026-09-03 12:07 -04:00 | nearest preserved | `v1.0.7` |
| 08 | compatibility-refinement | 2026-09-07 12:18 -04:00 | nearest preserved | `v1.0.8` |
| 09 | corrective-pass | 2026-09-08 07:39 -04:00 | nearest preserved | `v1.0.9` |
| 10 | outbound-lifecycle-cadence | 2026-09-10 01:03 -04:00 | nearest preserved | `v1.0.10` |
| 11 | playback-correctness-axis | 2026-09-11 20:19 -04:00 | nearest preserved | `v1.0.11` |
| 12 | null-output-pass | 2026-09-12 10:58 -04:00 | nearest preserved | `v1.0.12` |
| 13 | analysis-channel-pass | 2026-09-17 00:30 -04:00 | exact | `v1.0.13` |
| 14 | override-pass | 2026-09-17 12:36 -04:00 | exact | `v1.0.14` |
| 15 | product-identity | 2026-09-17 18:52 -04:00 | nearest preserved | `v1.0.15` |
| 16 | scrub-layout | 2026-09-18 02:27 -04:00 | nearest preserved | `v1.0.16` |
| 17 | channel-control | 2026-09-18 13:33 -04:00 | exact; chronology metadata corrected | `v1.0.17` |
| 18 | archive-history-foundation | 2026-09-18 20:20 -04:00 | exact Build 17 application plus archive milestone | `v1.0.18` |
| 19 | channel-acknowledgement | 2026-09-18 21:19 -04:00 | exact; chronology metadata corrected | `v1.0.19` |
| 20 | pre-admission-filtering | 2026-09-19 11:20 -04:00 | exact corrected baseline | `v1.0.20` |
| 21 | source-value-chase | 2026-09-19 17:22 -04:00 | exact | `v1.0.21` |
| 22 | drag-drop-analysis-completion | 2026-09-20 04:21 -04:00 | exact | `v1.0.22` |
| 23 | public-documentation-packaging | 2026-09-21 00:12:11 -04:00 | exact | `v1.0.23` |
| 24 | per-note-interval-gate | 2026-09-21 07:20 -04:00 | exact | `v1.0.24` |
| 25 | per-note-gate-correction | 2026-09-22 00:48 -04:00 | exact | `v1.0.25` |
| 26 | gate-hardening-analysis-preview | 2026-09-22 05:12 -04:00 | exact | `v1.0.26` |
| 27 | compact-segmented-event-store | 2026-09-22 07:55 -04:00 | exact | `v1.0.27` |
| 28 | direct-compact-parser | 2026-09-22 12:50 -04:00 | exact | `v1.0.28` |
| 29 | large-file-preflight | 2026-09-22 20:10 -04:00 | exact | `v1.0.29` |
| 30 | human-facing-refinement | 2026-09-22 22:50 -04:00 | exact | `v1.0.30` |
| 31 | queue-state-correctness | 2026-09-23 07:22 -04:00 | exact | `v1.0.31` |
| 32 | whole-state-chase | 2026-09-23 12:20 -04:00 | exact | `v1.0.32` |
| 33 | direct-event-rate | 2026-09-23 | exact | `v1.0.33` |
| 34 | uninterrupted-rate-and-source-readouts | 2026-09-23 | exact | `v1.0.34` |
| 35 | oldest-complete-note-overflow | 2026-09-24 | exact | `v1.0.35` |
| 36 | session-view-controls | 2026-09-24 | exact | `v1.0.36` |
| 37 | interface-refinements | 2026-09-24 | exact | `v1.0.37` |
| 38 | main-scrub-refinement | 2026-09-25 | exact | `v1.0.38` |
| 39 | numeric-and-analysis | 2026-09-25 | exact | `v1.0.39` |

## Evidence-recovery result

The publication audit compared all protected implementation snapshots against source/test trees. It found no complete missing handoff tree that could justify upgrading a `nearest preserved` row. Several snapshots match reconstructed source trees but were captured after the corresponding handoff; they remain corroborating evidence, not proof of exact release identity.

The public rewrite preserves every defensible source/test/root-file delta. Private diagnostics, raw native-provider logs, personal machine paths, and recovery manifests are deliberately absent from all public commits. Release notes disclose source confidence individually.

The rewritten Build 01–22 mapping is recorded in [PublicCommitMapping.csv](PublicCommitMapping.csv). `SELF` is used where embedding a commit's own hash in its tracked contents is impossible; resolve current milestones from `main` or their version tag. The private pre-sanitization mapping remains only in local safety references. The original clean-clone gate and its successful publication resolution are recorded in [PublicationGate.md](Shared/PublicationGate.md).
