# SGX530: the 3D-side invalidate waits for 0x45

2026-11-26, an OMAP3630 board, a transfer (3D only) through powervr_sgx and this branch's
microkernel.

`k_transfer` (and `do_3d`) request the 3D-side invalidate (PDS_INV1 and the CSC) and then
wait for the SGX545's done bits, 0x4b, in 0x138. On this core PDS_INV1 sets b2 and b6, and the
CSC b0, so the wait timed out on every transfer: bounded wait 7, logged at control block +0xa0.
With `INV_DONE_3D = 0x45` no wait times out (control block words 0x28/0x29 stay 0). That image
is byte for byte the one run on the board.

The transfer's 3D then starts and stops on fixed reads. That's written up on dev:
`docs/log/entries/2026-11-26-1000-sgx530-3d-only-transfer.md`.
