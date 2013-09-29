Merge Test
----------

The provided merge utility is useful for streaming/piping a
pintool-provided memory trace directly to a simulation application.

This merge test compares the output of the merge to the linux
"sort" utility (which I assume will be correct).
A proper comparison requires that the lamport timestamps be included
in both the merge output and sort output.
Additionally, trace entries with equal timestamps may correctly
appear in any order, and this order may differ between the merge
and sort outputs.  Therefore, we chech that identical entries
appear with the same timestamp.
