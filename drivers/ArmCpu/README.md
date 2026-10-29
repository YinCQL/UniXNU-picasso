# ArmCpu for picasso

Based on UniXNU/ArmCpu `1951cc8444b340d7d8879e9418220cf29eb711d7`.
The local change reads `/arm-io/intc/virtual-timer-intid` before mapping the
GIC, validates a little-endian u32 in 16..31, and uses that PPI for both
redistributor enables and IRQ dispatch. Picasso supplies 19; omission keeps 27.
The picasso board DT's third architectural timer entry is `<1 3 0xff08>`:
GIC PPI 3 is absolute INTID 19. The ADT generator emits `13 00 00 00`, not
Linux FDT's big-endian cells. A startup log reports the selected INTID;
only increasing timer interrupt counts establish delivery on hardware.

SMP remains opt-in through upstream's `sheng-smp` argument. Picasso boots
with `cpus=1`; describing eight CPU nodes does not enable SMP. No new-kernel
SMP result is claimed.

Run `python scripts/check-picasso-adt.py --output out/adt-check-NEW` from the
device checkout to generate and decode minimal, configured and Data-enabled
trees. Compose runs the same check before building. The check validates the
ADT binding and surrounding GIC/timebase fields, not hardware IRQ delivery.
