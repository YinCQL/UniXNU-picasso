# Boot regression on main: what changed and what this branch restores

Reported by the upstream maintainer on 2026-11-03: `main` no longer reaches the
system after an earlier PR was merged, and a few AI-assisted repair attempts did
not fix it.

This branch is the smallest change that puts `main` back on the pipeline that
boots on hardware (Redmi K30 5G / SM7250 "picasso"). It carries no GPU work and
no board-specific driver changes.

## Finding 1: the PR #12 merge did not change `main` at all

```
$ git log -1 --format='%h %p %s' origin/main
147d038 7efd7de e18e85a Merge pull request #12 from silenzio111/stage12/picasso-cpufreq-big

$ git merge-base --is-ancestor e18e85a 7efd7de && echo ancestor
ancestor

$ git diff --stat 7efd7de 147d038
(no output)
```

The merge's first parent already contained the branch tip, so the merge commit
is content-identical to `7efd7de`. **Nothing from PR #12 can be the cause of a
boot regression**; whatever broke must come from elsewhere (a local conflict
resolution, a local build, or a tree state that predates the working pipeline).

## Finding 2: `main`'s build pipeline installs one patch less than the tree that boots

`unixnu` (the launcher) prepares a patched copy of the pinned upstream revision
and carries the board patches:

| Patch | `main` | this branch |
|---|---|---|
| `patches/unixnu-source-kernel.patch` | yes | yes |
| `patches/picasso-usb-console-xnu.patch` | yes | yes |
| `patches/picasso-cpu-idle-xnu.patch` | yes | yes |
| `patches/picasso-cpu-topology-xnu.patch` | **missing** | **yes** |

The topology patch is what gives this board a two-cluster (E/P) topology and
builds the AMP scheduler. Without it the kernel is built with
`MAX_CPU_CLUSTERS == 1` while the board's ADT still declares eight CPUs across
two clusters (`reg = <0x600 …>` and `<0x700 …>`), which is the kind of mismatch
that shows up as a boot that stops before userspace with no obvious panic.

## Finding 3: the bootstrap pass of a fresh tree is hardened

`scripts/build-xnu-4k-baseline.sh` in this branch lets the first
(`XNU_BOOTSTRAP=1`) pass skip the probes whose call sites live in
`BootProgress.c`, instead of failing the build. Without it a clean tree can fail
to build at all, which reads as "it does not boot" when the image is never
produced.

## What this branch changes

```
patches/picasso-cpu-topology-xnu.patch   new    (+158)
unixnu                                   +8 -1  (carry the patch, hash it)
scripts/build-xnu-4k-baseline.sh        +31 -6  (bootstrap pass)
```

Nothing else: no drivers, no GPU code, no board configuration.

## Evidence that this pipeline boots

The same four-patch pipeline is what builds every image used in
`docs/handoff-20261102-ui-frame-cpu-topology.md`: the device reaches
SpringBoard, the lock screen and the home screen, with the CPU topology dump

```
PICASSO_CPUFREQ topo … en=6 pn=2
```

and both clusters visible in the governor line. That is the tree this branch
copies the pipeline from, unchanged.

## How to verify

```sh
git checkout fix/main-boot
python3 unixnu build kernel prelink      # or the usual build-… invocation
fastboot boot out/picasso/boot/<image>.img
```

Expected on the console, in order:

```
PICASSO_CPUFREQ topo … en=6 pn=2
… Early boot complete …
```

If it still stops before userspace, the useful thing to capture is the last
console lines (or the panic string). "Cannot boot" splits into at least four
different failures — UEFI, early kernel, launchd, SpringBoard — and the fix
differs for each.
