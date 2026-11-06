# Why uniquely solvable fixtures are expensive

*Note from Phase 10B, written after the first 80 of 300 population B fixtures were carved
(n = 7–15, the high-sparsity targets). Carving was paused at that point; the numbers below
come from `benchmarks/fixtures/meta/B-*.csv`.*

## The short version

A population A fixture (random blanks, any number of solutions) takes milliseconds to make.
A population B fixture (exactly one solution, at an exact empty count) needs a **proof of
uniqueness after every clue removed**. On large boards, a 600 s budget runs out long before
the target is reached. All 80 fixtures carved so far used the full 600 s and ended as
`target_not_achieved`.

## What the budget buys

How far carving gets in 600 s depends almost only on the board size. The target makes no
difference: 40%, 50%, 60% and 70% targets all stop at the same point. Means over 3 seeds:

| n | Cells | Empty after 600 s | Removals / s | Search checks | Checks aborted |
|---:|---:|---:|---:|---:|---:|
| 7 | 2,401 | ~1,026 (42.7%) | 1.7 | ~655 | ~87 |
| 8 | 4,096 | ~1,560 (38.1%) | 2.6 | ~895 | ~36 |
| 9 | 6,561 | ~2,253 (34.3%) | 3.8 | ~1,180 | 0 |
| 10 | 10,000 | ~3,010 (30.1%) | 5.0 | ~1,480 | 0 |
| 11 | 14,641 | ~3,940 (26.9%) | 6.6 | ~1,850 | 0 |
| 12 | 20,736 | ~5,160 (24.9%) | 8.6 | ~2,350 | 0 |
| 13 | 28,561 | ~6,640 (23.2%) | 11 | ~2,950 | 0 |
| 14 | 38,416 | ~8,470 (22.0%) | 14 | ~3,760 | 0 |
| 15 | 50,625 | ~10,640 (21.0%) | 18 | ~4,680 | 0 |

The three seeds of each cell agree to within about 1%, so this is systematic, not bad luck.
Every fixture ran out of budget on its first attempt; none ran out of removal order.

## Where the time goes

**1. Every check is a full solver run, and there are as many checks as removals.**
- A removal is accepted for free when the cell has only one candidate (a "forced" removal).
  Otherwise it needs a search: MRV with a limit of 2 solutions, falling back to DLX when MRV
  aborts.
- Each search starts from scratch. It rebuilds the constraint state for all N² cells, and
  DLX builds a matrix of up to about 4N³ nodes. Even an easy check therefore costs time
  proportional to the board size.
- The number of removals needed also grows with the board size.
- So the total cost grows roughly with the **square of the cell count**, even while every
  proof is trivial.

That is the whole story for n ≥ 9: no check aborted, and the budget still ran out at 21–34%
empty. Reaching 70% at n = 15 means 35,438 removals, 3.3 times what 600 s achieved. That
would take over half an hour even at the opening rate, and the rate only falls as the board
empties.

**2. Uniqueness proofs get exponentially harder as the puzzle thins.**
- Proving that there is no second solution means exhausting the whole search tree.
- Near the minimal-clue frontier that tree explodes. n = 7 has already reached that region
  at about 40% empty: 13% of its checks hit the 1-million-node limit in both MRV and DLX.
- Each aborted check is the most expensive kind, and it achieves nothing: the clue has to be
  kept.
- The larger boards have not reached this region yet. They will hit it on top of cost 1
  as soon as they get past it.

## What this means for the study

- Population B will exist only at moderate sparsity on large boards. Above n ≈ 7, targets of
  40% or more will be `target_not_achieved` under the planned budget, each with its
  sparsest result saved separately as a `.best` puzzle.
- This is a generation-cost result, not evidence that those puzzles cannot exist.
- Raising the budget does not scale well. Cost 1 grows quadratically with board size, and
  cost 2 grows exponentially with sparsity.
- Ways to make it cheaper, listed for the 10B review and **not implemented**:
  - keep solver state between checks instead of rebuilding it for every removal;
  - accept more removals by cheap logical deduction before falling back to search;
  - remove clues in batches, and bisect a batch only when it breaks uniqueness;
  - accept the coverage gap and report it as a finding.
- Only the first two of those leave the fixture definition unchanged, and none can remove
  cost 2.
