# Post-v1 backlog

Work deliberately left out of v1, with where it was decided. Nothing here blocks a v1 release.

| Item | Why it waits | Source |
| --- | --- | --- |
| Filter-category hint for hosts and control surfaces | Neither CLAP nor VST3 has a standard filter-type hint. A later version could map one from the shape descriptor's family (`params::ShapeInfo`) through a CLAP extension or a control-surface protocol. | Area 08 R6, open question |
| Screen-reader support (UIA, NSAccessibility bridge) | Keyboard navigation ships in v1; the platform bridge over the accessible-control model comes after. | D1; area 15 R3, R12 |
