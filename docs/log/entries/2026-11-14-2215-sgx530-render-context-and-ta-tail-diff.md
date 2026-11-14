# SGX530: the vendor's render context and TA command tail, read against ours (offline)

**2026-11-14 22:15 UTC. Offline, from the vendor kick snapshot; no board time.**

## HW render context

| word | vendor (1.14) | ours |
|---|---|---|
| 0 flags | 0x21 | 0x21 |
| 1 | 1 | 0 (our microkernel doesn't read it) |
| 2 PD | physical address | slot in the host's PD table (the one-context path) |
| 3 CCB, 4 CCB control, 6 PB descriptor | same roles | same |
| 7 | the TA-data heap base | our microkernel's per-context scratch (3D-params heap) |
| 8, 9 | the render details, twice | not in our layout |

Neither word 1 nor word 7 reaches a register through our microkernel, so neither is a knob.

## TA command tail

Up to word 15 the vendor's per-kick words line up with ours: TE AA, the macrotile words, the
TE screen, the macrotile count, TA flags 2, the VDM base and control, the region and tail
bases, the epsilons, the screen and the PB base. The 530 values of those are already in the
driver. Past word 15 the vendor tail goes on with rt_flags (0x88) at word 16, then 0, the
macrotile words again, 0x10, and two TA-data-heap addresses (0x0c800c00, 0x0c800a00). Ours
has rt_flags at word 27 and nothing else past word 15. The vendor RT-data entry also carries
the region-header pointers, which ours doesn't.

Our microkernel writes the tail into TA registers through fixed tables of SGX545 offsets
(`TA_TE`, `TA_PB` and `TA_RT` in `ukernel/cores/sgx545.py`, unchanged in `sgx530.py`). The
SGX530's GPL header names none of those TA registers. So matching the vendor's extra words
means the microkernel writing unnamed 530 TA registers. That is the next fork, and it needs
register-level observation that isn't in an approved category yet.
