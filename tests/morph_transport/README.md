# Isolated kit Morph transport-command regression

This test belongs to a narrowly scoped proposed upstream fix. It operates on the
public firmware source commit a43cd4fcda23ec3d577dc425261c3eb28bd5abff.
It does not assert source/binary equivalence to the manufacturer's official
v1.71 application image. The owner reports that the affected behavior exists
on original v1.71 firmware.

Run from the repository root:

    python3 tests/morph_transport/run.py

The runner compiles the original production Morph and immediate kit-load
function bodies with a recording MIDI sink and the firmware's actual
parameter-order table, under AddressSanitizer and UndefinedBehaviorSanitizer.

Two separate outcomes are required:

1. The unmodified upstream functions must fail the expected assertion when
   they dispatch reserved NRPN Data Entry as a sound parameter. Only the
   transport-slot predicate used by the assertion comes from the candidate.
2. The isolated corrected functions must pass all 256 Morph positions,
   identical-kit dispatch coverage, reserved-slot exclusion, retained index
   progression and restart behavior, eventual convergence, immediate-kit-load
   dispatch, and exactly 247 soundengine service calls.

The fixed production file is unchanged from the previously reviewed 21-line
Morph/kit-load portion of the laboratory BUG 001 patch. No MIDI parser, filter,
Drive curve, smoothing, preset serialization, or CLP code changes are included.

The test's MIDI sink, soundengine service, modulation targets, and hardware
effects are host mocks. It is not an instrument audio test, a physical-device
test, an ARM build, or proof of runtime timing/stack margins. Older real-device
observations were made with the combined laboratory patch and must not be
represented as a separate device validation of this isolated candidate.
