# Recovering HDF5 Output Files

This runbook describes conservative recovery procedures for an HDF5 output file that cannot be opened after a crash, forced termination, power loss, or system shutdown. It is intended for Meteoris output files but uses standard HDF5 tools.

> **Important:** Work on a copy. HDF5 recovery tools can make a damaged file easier to inspect, but they cannot reconstruct data that was never flushed to disk or repair every form of metadata corruption.

For the common unclean-shutdown case, Meteoris includes a conservative helper
that automates the safe subset of this runbook while preserving the original.
The helper is included in **both** Raspberry Pi build modes (full and
recorder-only) and is installed as `meteoris_recover_hdf5`:

```bash
meteoris_recover_hdf5 /absolute/path/to/output.h5
```

By default it creates `output.h5.recovery`, records diagnostics beside that
copy, and, if the normal `h5dump` probe fails, captures the full HDF5 error stack
before deciding whether any mutation is justified. It clears a stale write/SWMR
consistency flag only when that error stack identifies the condition, and uses
`h5clear --increment` only after confirming an EOA/EOF mismatch. Use the manual
procedure below for unusual failures or application-level validation.

## 1. Stop writers and identify the file

Do not attempt recovery while Meteoris or another process may still be writing to the file.

```bash
FILE=/absolute/path/to/output.h5

lsof -- "$FILE"
fuser -- "$FILE"
```

If either command reports a live process, stop that process normally before continuing. Do not clear HDF5 consistency flags on a file that is genuinely open for writing. On a cluster or network filesystem, also check other nodes and scheduled jobs.

Confirm that the path is a regular file and that the filesystem is available:

```bash
ls -l -- "$FILE"
df -h -- "$FILE"
df -i -- "$FILE"
```

## 2. Preserve the original

Make a byte-for-byte working copy on a filesystem with enough free space. Keep the original unchanged.

```bash
RECOVERY_FILE="${FILE}.recovery"
cp --preserve=all -- "$FILE" "$RECOVERY_FILE"
```

Record checksums so later changes are unambiguous:

```bash
sha256sum -- "$FILE" "$RECOVERY_FILE"
```

All commands below should target `$RECOVERY_FILE`, not `$FILE`.

## 3. Capture basic diagnostics

Record tool versions and test the HDF5 signature and metadata tree:

```bash
h5clear --version
h5dump --version
file -- "$RECOVERY_FILE"
h5dump -H -- "$RECOVERY_FILE" > recovery-header.txt 2> recovery-errors.txt
```

If `h5dump -H` succeeds, the file is structurally readable. Continue with [Validate recovered data](#7-validate-recovered-data).

## 4. Error: file is already open for write/SWMR write

A crash can leave write-consistency bits set in the HDF5 superblock. A typical error ends with:

```text
file is already open for write/SWMR write
(may use <h5clear file> to clear file consistency flags)
```

After verifying that no writer is active, clear the status flags on the recovery copy:

```bash
h5clear --status "$RECOVERY_FILE"
# Equivalent short option on supported versions:
# h5clear -s "$RECOVERY_FILE"
```

Then test it without modifying it further:

```bash
h5dump -H -- "$RECOVERY_FILE" > recovery-header.txt 2> recovery-errors.txt
h5ls -r -- "$RECOVERY_FILE"
```

Clearing the flag only removes the stale open-for-write marker. It does not roll back an interrupted write or prove that the last dataset/chunk is valid.

## 5. Error after a crashed SWMR writer: EOA/EOF mismatch

For a file written in SWMR mode, a crash can leave the stored end-of-address (EOA) inconsistent with the physical end-of-file (EOF). Inspect both values:

```bash
h5clear --filesize "$RECOVERY_FILE"
```

If the file still cannot be opened and the output indicates an EOA/EOF problem, create another copy before changing it:

```bash
cp --preserve=all -- "$RECOVERY_FILE" "${RECOVERY_FILE}.before-increment"
h5clear --increment "$RECOVERY_FILE"
```

`--increment` sets EOA based on EOF and adds a default safety increment. Some HDF5 versions allow an explicit value, for example `--increment=1048576`. Do not use this option merely because ordinary status-flag clearing failed; it is specifically intended for crashed SWMR files with an EOA/EOF discrepancy.

Retest:

```bash
h5dump -H -- "$RECOVERY_FILE" > recovery-header.txt 2> recovery-errors.txt
h5ls -r -- "$RECOVERY_FILE"
```

## 6. Other common failures

### Not an HDF5 file or truncated superblock

Messages such as `file signature not found`, an empty file, or a file shorter than expected may mean the wrong file was selected, the write never established a valid HDF5 superblock, or the beginning of the file is corrupt.

```bash
stat -- "$RECOVERY_FILE"
file -- "$RECOVERY_FILE"
```

`h5clear` is not a general repair utility and normally cannot repair a missing or corrupt superblock. Restore from a checkpoint/backup or retain the file for specialist analysis.

### Permission or read-only filesystem errors

Check the file, parent directory, mount, and available space:

```bash
namei -l -- "$RECOVERY_FILE"
findmnt -T "$RECOVERY_FILE"
df -h -- "$RECOVERY_FILE"
df -i -- "$RECOVERY_FILE"
```

Recover on a writable local copy rather than changing permissions or mount options without understanding their operational impact.

### Missing compression/filter plugin

Errors mentioning a required filter, filter ID, plugin, or decompression may indicate that the file is structurally valid but the reader lacks the plugin used to encode a dataset. Use the same HDF5 build and plugin environment as the writer, and verify `HDF5_PLUGIN_PATH`. Do not use `h5clear` for this problem.

### Object-header, B-tree, heap, checksum, or bad-address errors

These usually indicate interrupted or corrupt metadata. Preserve the original and all recovery copies. Try to extract unaffected objects individually, but do not overwrite the only copy:

```bash
h5ls -r -- "$RECOVERY_FILE"
h5dump -d /path/to/readable_dataset -o dataset.bin -- "$RECOVERY_FILE"
```

If metadata needed to locate objects is corrupt, standard HDF5 tools may be unable to recover them. Escalate with the original file, Meteoris/HDF5 versions, exact commands, full error stack, filesystem type, and shutdown details.

## 7. Validate recovered data

A successful open is necessary but not sufficient. Validate both structure and application-level content.

```bash
h5dump -H -- "$RECOVERY_FILE" > recovery-header.txt
h5ls -r -- "$RECOVERY_FILE" > recovery-inventory.txt
```

Check at least the following:

- Expected groups and datasets exist.
- Dataset shapes and datatypes are plausible.
- Time, iteration, or record indices are monotonic and within the expected range.
- The final record/chunk is readable and internally consistent.
- Required attributes and completion markers are present.
- Meteoris can open the file read-only, if it provides that mode.

When practical, export or read every dataset so latent chunk/filter failures are detected rather than only testing metadata. Compare results against a previous checkpoint or known invariants.

## 8. Resume safely

Do not immediately append to the original file.

1. Keep the untouched original and checksum.
2. Validate the recovery copy read-only.
3. Prefer starting Meteoris in a new output file from the last confirmed checkpoint.
4. Append to a recovered file only if Meteoris explicitly supports crash recovery and its application-level state has been validated.
5. Document any lost or discarded final timesteps/records.

## 9. Stop conditions

Stop automated recovery and preserve all copies if:

- another writer may still be active;
- the original is changing during diagnosis;
- `h5clear --status` does not make the file readable and there is no confirmed SWMR EOA/EOF issue;
- errors report corrupt object headers, B-trees, heaps, checksums, or addresses;
- the file opens but scientific/application invariants fail; or
- the file contains uniquely valuable data and further mutation could reduce recovery options.

## 10. Information to collect for escalation

```bash
h5clear --version
h5dump --version
uname -a
findmnt -T "$FILE"
stat -- "$FILE"
sha256sum -- "$FILE"
h5dump -H --enable-error-stack=2 -- "$FILE" > h5dump-header.txt 2> h5dump-errors.txt
```

Also record the Meteoris version and command line, whether SWMR or MPI/parallel HDF5 was used, the last known successful checkpoint, the shutdown type, and whether the output lived on local, NFS, Lustre, GPFS, or another network/parallel filesystem. Remove secrets and sensitive paths before sharing logs.

## Quick recovery checklist

```bash
FILE=/absolute/path/to/output.h5
RECOVERY_FILE="${FILE}.recovery"

# 1. Verify there is no live writer.
lsof -- "$FILE"
fuser -- "$FILE"

# 2. Preserve the original.
cp --preserve=all -- "$FILE" "$RECOVERY_FILE"
sha256sum -- "$FILE" "$RECOVERY_FILE"

# 3. Clear only the stale writer flag on the copy.
h5clear --status "$RECOVERY_FILE"

# 4. Test structure and inventory.
h5dump -H -- "$RECOVERY_FILE" > recovery-header.txt 2> recovery-errors.txt
h5ls -r -- "$RECOVERY_FILE" > recovery-inventory.txt

# 5. Validate application data before resuming.
```

## Reference

- [HDF Group: The HDF5 `h5clear` Tool](https://support.hdfgroup.org/documentation/hdf5/latest/_h5_t_o_o_l__c_r__u_g.html)

